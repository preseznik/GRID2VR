using System.Diagnostics;
using System.Text.Json;

namespace GRID2VR;

// A track GRID2VR converts from another game the user owns. As in DiRT2VR, the
// converter is compiled into this launcher and runs in a child process of it
// (GRID2VR.exe --convert-track), and only the pinned source version is accepted.
// The converted pack loads through GRID 2's own -mod overlay
// (tools/session_config.py custom_tracks); the game installation is never written.
internal sealed record CustomTrackSource(string Id, string Label, string SteamAppId, string SourceTrack, string Layout, string Pack, string Requirement, long RequiredBytes,
    string SourceName, string Summary, string Description)
{
    public override string ToString() => Label;
}
internal sealed record CustomTrackPack(string Folder, long Bytes, DateTime Built);
internal sealed record CustomTrackStatus(CustomTrackSource Source, string? SourceFolder, CustomTrackPack? Installed);

internal static class CustomTracks
{
    internal static readonly CustomTrackSource[] Supported =
    [
        new("circuits/g2vr_nordschleife/route_0", "Nordschleife", "244210", "ks_nordschleife", "nordschleife", "nordschleife",
            "your own Assetto Corsa with its Nordschleife (ks_nordschleife)", 1536L * 1024 * 1024,
            "Assetto Corsa", "Circuit · 20.7 km",
            "Nürburgring Nordschleife, converted from your Assetto Corsa copy. 20.7 km circuit · Practice and Race · Desktop and VR. Experimental: AI pace is approximate."),
    ];
    const long ManifestLimit = 64 * 1024;

    internal static string Root(string root) => Path.Combine(AppPaths.Data(root), "custom-tracks");
    static string Staging(string root) => Path.Combine(AppPaths.Data(root), "custom-tracks-staging");
    static string ChoicesPath(string root) => Path.Combine(AppPaths.Data(root), "custom-track-sources.json");

    // The source track folder: a folder the user chose (the game folder or the track
    // folder itself), else the game in any Steam library. The AI line and model must exist.
    internal static string? FindSource(CustomTrackSource source, IEnumerable<string>? installs = null, string? chosen = null)
    {
        static bool Complete(CustomTrackSource source, string track) =>
            File.Exists(Path.Combine(track, source.Layout, "ai", "fast_lane.ai")) && File.Exists(Path.Combine(track, source.SourceTrack + ".kn5"));
        if (!string.IsNullOrWhiteSpace(chosen))
        {
            var direct = Path.GetFullPath(chosen);
            foreach (var track in new[] { direct, Path.Combine(direct, "content", "tracks", source.SourceTrack) })
                if (string.Equals(Path.GetFileName(Path.TrimEndingDirectorySeparator(track)), source.SourceTrack, StringComparison.OrdinalIgnoreCase) && Complete(source, track)) return track;
        }
        IEnumerable<string> found;
        try { found = installs ?? Installation.SteamApp(source.SteamAppId).ToArray(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Security.SecurityException) { found = []; }
        foreach (var install in found)
        {
            var track = Path.Combine(install, "content", "tracks", source.SourceTrack);
            if (Complete(source, track)) return track;
        }
        return null;
    }

    internal static string? ChosenFolder(string root, CustomTrackSource source)
    {
        var path = ChoicesPath(root);
        if (!File.Exists(path) || new FileInfo(path).Length > ManifestLimit) return null;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            return document.RootElement.TryGetProperty(source.Pack, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;
        }
        catch (JsonException) { return null; }
    }

    // Remembers a folder the user picked; refuses one that does not hold the source track.
    internal static string ChooseFolder(string root, CustomTrackSource source, string folder)
    {
        var track = FindSource(source, [], folder) ?? throw new IOException($"That folder does not contain {source.SourceTrack}. Choose your Assetto Corsa folder.");
        Directory.CreateDirectory(AppPaths.Data(root));
        var choices = new Dictionary<string, string>(StringComparer.Ordinal);
        if (File.Exists(ChoicesPath(root)) && new FileInfo(ChoicesPath(root)).Length <= ManifestLimit)
            try { choices = JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(ChoicesPath(root))) ?? choices; } catch (JsonException) { }
        choices[source.Pack] = Path.GetFullPath(folder);
        File.WriteAllText(ChoicesPath(root), JsonSerializer.Serialize(choices, new JsonSerializerOptions { WriteIndented = true }));
        return track;
    }

    // Installed packs for one track id: folders with a bounded manifest naming it and an overlay.
    internal static CustomTrackPack[] Installed(string customRoot, string id)
    {
        if (!Directory.Exists(customRoot)) return [];
        Updates.PlainPath(customRoot);
        var packs = new List<CustomTrackPack>();
        foreach (var folder in Directory.EnumerateDirectories(customRoot))
        {
            var manifest = Path.Combine(folder, "custom-track.json");
            if (new DirectoryInfo(folder).Attributes.HasFlag(FileAttributes.ReparsePoint) || !File.Exists(manifest) ||
                new FileInfo(manifest).Length > ManifestLimit || !Directory.Exists(Path.Combine(folder, "overlay"))) continue;
            try
            {
                using var document = JsonDocument.Parse(File.ReadAllText(manifest));
                if (document.RootElement.GetProperty("schema").GetInt32() != 1 ||
                    !document.RootElement.GetProperty("tracks").EnumerateArray().Any(t => t.GetProperty("id").GetString() == id)) continue;
            }
            catch (Exception error) when (error is JsonException or KeyNotFoundException or InvalidOperationException) { continue; }
            var bytes = new DirectoryInfo(folder).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length);
            packs.Add(new(folder, bytes, File.GetLastWriteTime(manifest)));
        }
        return [.. packs.OrderByDescending(p => p.Built)];
    }

    internal static CustomTrackStatus Status(string root, CustomTrackSource source) =>
        new(source, FindSource(source, chosen: ChosenFolder(root, source)), Installed(Root(root), source.Id).FirstOrDefault());

    internal static string Describe(CustomTrackStatus status)
    {
        var installed = status.Installed is { } pack
            ? $"{status.Source.Label}: installed ({pack.Bytes / (1024.0 * 1024):N0} MB, built {pack.Built:g})."
            : $"{status.Source.Label}: not installed.";
        return installed + (status.SourceFolder is null
            ? " Needs " + status.Source.Requirement + ". If Steam does not find it, choose its folder."
            : " Source: " + status.SourceFolder);
    }

    // Converter output lines → approximate progress (0–100) for the launcher's bar.
    internal static int Percent(string line, int previous)
    {
        static int Fraction(string line, string prefix, int from, int to)
        {
            var text = line[prefix.Length..]; var slash = text.IndexOf('/'); var end = text.IndexOf(':');
            return slash > 0 && end > slash && int.TryParse(text[..slash], out var done) && int.TryParse(text[(slash + 1)..end], out var total) && total > 0
                ? from + (to - from) * Math.Clamp(done, 0, total) / total : from;
        }
        var value = line.StartsWith("Checked source ", StringComparison.Ordinal) ? Fraction(line, "Checked source ", 0, 10)
            : line.StartsWith("Reading ", StringComparison.Ordinal) ? Math.Min(40, Math.Max(previous, 10) + 2)
            : line.StartsWith("Course ", StringComparison.Ordinal) ? 45
            : line.StartsWith("Collision:", StringComparison.Ordinal) ? 55
            : line.StartsWith("Converting scenery", StringComparison.Ordinal) ? 60
            : line.StartsWith("Writing track tiles", StringComparison.Ordinal) ? 80
            : line.StartsWith("Visuals:", StringComparison.Ordinal) ? 92
            : line.StartsWith("Route probe built", StringComparison.Ordinal) ? 96 : previous;
        return Math.Max(previous, value);
    }

    // Converts into a staging folder, then replaces the installed pack in one move.
    // Requires an idle installation: no session, no GRID 2 process, no pending recovery.
    internal static async Task Build(string root, CustomTrackSource source, IProgress<(int Percent, string Line)>? progress = null, CancellationToken cancellation = default)
    {
        Installation.RequireIdle(root);
        var sourceFolder = FindSource(source, chosen: ChosenFolder(root, source)) ?? throw new IOException(source.Label + " needs " + source.Requirement + ".");
        var staging = Staging(root); Directory.CreateDirectory(staging); Updates.PlainPath(staging);
        var drive = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(staging))!);
        if (drive.AvailableFreeSpace < source.RequiredBytes)
            throw new IOException($"Not enough free space to build {source.Label}. Free at least {Math.Ceiling(source.RequiredBytes / 1073741824.0):0} GB on {drive.Name}");
        var output = Path.Combine(staging, $"{source.Pack}-{DateTime.Now:yyyyMMdd-HHmmss}");
        var self = Environment.ProcessPath ?? throw new IOException("The launcher path is unknown.");
        var info = new ProcessStartInfo(self) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = staging };
        foreach (var argument in new[] { "--convert-track", source.Pack, sourceFolder, AppPaths.Game(root), output }) info.ArgumentList.Add(argument);
        int percent = 0; string last = "";
        try
        {
            using (var process = Process.Start(info) ?? throw new IOException("Track converter did not start."))
            using (cancellation.Register(() => { try { if (!process.HasExited) process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { } catch (System.ComponentModel.Win32Exception) { } }))
            {
                var errors = process.StandardError.ReadToEndAsync(CancellationToken.None);
                while (await process.StandardOutput.ReadLineAsync(CancellationToken.None) is { } line)
                    if (line.Trim().Length > 0) { last = line.Trim(); percent = Percent(last, percent); progress?.Report((percent, last)); }
                await process.WaitForExitAsync(CancellationToken.None);
                cancellation.ThrowIfCancellationRequested();
                var error = (await errors).Trim();
                if (process.ExitCode != 0) throw new IOException($"{source.Label} conversion failed: " + (error.Length > 0 ? error.Split('\n').Last().Trim() : last));
            }
            var built = Installed(staging, source.Id).FirstOrDefault(p => p.Folder == output) ?? throw new IOException("Track conversion produced no installable pack.");
            progress?.Report((98, "Installing…"));
            Installation.RequireIdle(root);
            var installRoot = Root(root); Directory.CreateDirectory(installRoot); Updates.PlainPath(installRoot);
            foreach (var old in Installed(installRoot, source.Id)) DeletePack(installRoot, old.Folder);
            var destination = Path.Combine(installRoot, source.Pack);
            if (Directory.Exists(destination)) destination += "-" + Path.GetFileName(output)[(source.Pack.Length + 1)..];
            Directory.Move(built.Folder, destination);
            progress?.Report((100, source.Label + " installed."));
        }
        finally
        {
            if (Directory.Exists(output)) DeletePack(staging, output);
            if (Directory.Exists(staging) && !Directory.EnumerateFileSystemEntries(staging).Any()) Directory.Delete(staging);
        }
    }

    internal static int Command(string root, string action, string pack)
    {
        var source = Supported.SingleOrDefault(s => s.Pack == pack);
        if (source is null) { Console.Error.WriteLine("Unknown custom track: " + pack); return 2; }
        try
        {
            if (action == "build") Build(root, source, new Progress<(int Percent, string Line)>(p => Console.WriteLine($"{p.Percent,3}% {p.Line}"))).GetAwaiter().GetResult();
            else if (action == "remove") Remove(root, source);
            Console.WriteLine(Describe(Status(root, source)));
            return 0;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Console.Error.WriteLine(error.Message); return 1; }
    }

    internal static void Remove(string root, CustomTrackSource source)
    {
        Installation.RequireIdle(root);
        var installRoot = Root(root);
        foreach (var pack in Installed(installRoot, source.Id)) DeletePack(installRoot, pack.Folder);
    }

    // Only a direct, unlinked child of its parent is ever deleted.
    static void DeletePack(string parent, string folder)
    {
        var full = Path.GetFullPath(folder);
        if (!string.Equals(Path.GetDirectoryName(full), Path.GetFullPath(parent).TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase))
            throw new IOException("Refusing to delete outside the custom tracks folder.");
        Updates.PlainPath(full);
        if (new DirectoryInfo(full).EnumerateFileSystemInfos("*", SearchOption.AllDirectories).Any(i => i.Attributes.HasFlag(FileAttributes.ReparsePoint)))
            throw new IOException("Linked files in a custom track pack were retained.");
        Directory.Delete(full, recursive: true);
    }
}
