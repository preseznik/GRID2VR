using System.Diagnostics;
using System.Net.Http;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace GRID2VR;

internal sealed record ReleaseVersion(int Major, int Minor, int Patch, string Pre) : IComparable<ReleaseVersion>
{
    public static ReleaseVersion? Parse(string text)
    {
        var m = Regex.Match(text, @"\Av?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z.-]+)?\z");
        if (!m.Success || !int.TryParse(m.Groups[1].Value, out var a) || !int.TryParse(m.Groups[2].Value, out var b) || !int.TryParse(m.Groups[3].Value, out var c)) return null;
        var pre = m.Groups[4].Value;
        if (pre.Split('.').Any(s => s.Length > 1 && s.All(char.IsAsciiDigit) && s[0] == '0')) return null;
        return new(a, b, c, pre);
    }
    public int CompareTo(ReleaseVersion? other)
    {
        if (other is null) return 1;
        foreach (var value in new[] { Major.CompareTo(other.Major), Minor.CompareTo(other.Minor), Patch.CompareTo(other.Patch) }) if (value != 0) return value;
        if (Pre == other.Pre) return 0;
        if (Pre.Length == 0 || other.Pre.Length == 0) return Pre.Length == 0 ? 1 : -1;
        var a = Pre.Split('.'); var b = other.Pre.Split('.');
        for (int i = 0; i < Math.Min(a.Length, b.Length); i++)
        {
            var an = a[i].All(char.IsAsciiDigit); var bn = b[i].All(char.IsAsciiDigit);
            var n = an && bn ? a[i].Length.CompareTo(b[i].Length) : an != bn ? an ? -1 : 1 : 0;
            if (n == 0) n = string.CompareOrdinal(a[i], b[i]);
            if (n != 0) return n;
        }
        return a.Length.CompareTo(b.Length);
    }
}

internal sealed record ReleaseInstaller(string Name, Uri Url, long Size, string Sha256);
internal sealed record AppRelease(string Tag, ReleaseVersion Version, Uri Page, ReleaseInstaller? Installer);

internal sealed class Updates(HttpClient http)
{
    public const string Repository = "https://github.com/preseznik/GRID2VR";
    public const string Feed = "https://api.github.com/repos/preseznik/GRID2VR/releases";
    public const long MaxInstaller = 512L * 1024 * 1024;
    public static string CurrentVersion => typeof(Updates).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion.Split('+')[0] ?? "0.1.0";
    public static HttpClient CreateClient() => new() { Timeout = TimeSpan.FromMinutes(15) };
    private static HttpRequestMessage Request(Uri url)
    {
        var message = new HttpRequestMessage(HttpMethod.Get, url);
        message.Headers.UserAgent.ParseAdd("GRID2VR/" + CurrentVersion);
        if (url.Host == "api.github.com")
        {
            message.Headers.Accept.ParseAdd("application/vnd.github+json");
            message.Headers.Add("X-GitHub-Api-Version", "2022-11-28");
        }
        return message;
    }
    public static AppRelease? ReadRelease(JsonElement value, bool experimental)
    {
        if (value.GetProperty("draft").GetBoolean()) return null;
        var tag = value.GetProperty("tag_name").GetString() ?? "";
        var version = ReleaseVersion.Parse(tag);
        if (version is null || !experimental && (value.GetProperty("prerelease").GetBoolean() || version.Pre.Length != 0)) return null;
        var page = new Uri(Repository + "/releases/tag/" + Uri.EscapeDataString(tag));
        var expectedName = "GRID2VR-" + tag.TrimStart('v') + "-Setup.exe";
        ReleaseInstaller? installer = null;
        foreach (var asset in value.GetProperty("assets").EnumerateArray())
        {
            if (asset.GetProperty("name").GetString() != expectedName || asset.GetProperty("state").GetString() != "uploaded") continue;
            var size = asset.GetProperty("size").GetInt64();
            var digest = asset.TryGetProperty("digest", out var hash) && hash.ValueKind == JsonValueKind.String ? hash.GetString()! : "";
            var url = asset.GetProperty("browser_download_url").GetString();
            var expected = Repository + "/releases/download/" + Uri.EscapeDataString(tag) + "/" + Uri.EscapeDataString(expectedName);
            if (size is <= 0 or > MaxInstaller || !Regex.IsMatch(digest, @"\Asha256:[a-fA-F0-9]{64}\z") || url != expected) continue;
            if (installer is not null) throw new IOException("Release contains duplicate installers.");
            installer = new(expectedName, new Uri(expected), size, digest[7..].ToLowerInvariant());
        }
        return new(tag, version, page, installer);
    }
    public async Task<AppRelease?> Check(bool experimental, CancellationToken cancellation, bool allowOlder = false)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        timeout.CancelAfter(TimeSpan.FromSeconds(30));
        AppRelease? newest = null;
        var current = ReleaseVersion.Parse(CurrentVersion)!;
        // Page URLs are constructed locally; no server-provided redirect/link can change the API repository.
        for (int page = 1; page <= 10; page++)
        {
            using var request = Request(new Uri(Feed + $"?per_page=100&page={page}"));
            using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, timeout.Token);
            if (response.StatusCode == System.Net.HttpStatusCode.NotFound) throw new IOException("The public GRID2VR release feed is unavailable. Check the repository's Releases page.");
            response.EnsureSuccessStatusCode();
            using var stream = await response.Content.ReadAsStreamAsync(timeout.Token);
            using var data = new MemoryStream();
            var buffer = new byte[32768]; int count;
            while ((count = await stream.ReadAsync(buffer, timeout.Token)) != 0)
            {
                if (data.Length + count > 2 * 1024 * 1024) throw new IOException("Release response exceeds its size limit.");
                data.Write(buffer, 0, count);
            }
            using var document = JsonDocument.Parse(data.ToArray());
            var rows = document.RootElement;
            if (rows.ValueKind != JsonValueKind.Array || rows.GetArrayLength() > 100) throw new IOException("Invalid release response.");
            foreach (var item in rows.EnumerateArray())
            {
                var release = ReadRelease(item, experimental);
                if (release is not null && (allowOlder || release.Version.CompareTo(current) > 0) && (newest is null || release.Version.CompareTo(newest.Version) > 0)) newest = release;
            }
            if (rows.GetArrayLength() < 100) return newest;
        }
        throw new IOException("Release listing exceeded its page limit. Open Releases to check manually.");
    }
    internal static void PlainPath(string path)
    {
        for (var item = Path.GetFullPath(path); !string.IsNullOrEmpty(item); item = Path.GetDirectoryName(item))
            if (Path.Exists(item) && File.GetAttributes(item).HasFlag(FileAttributes.ReparsePoint)) throw new IOException("Linked update path retained.");
    }
    public async Task<string> Download(ReleaseInstaller asset, string root, IProgress<int>? progress, CancellationToken cancellation)
    {
        if (asset.Size is <= 0 or > MaxInstaller || Path.GetFileName(asset.Name) != asset.Name || !Regex.IsMatch(asset.Sha256, @"\A[0-9a-f]{64}\z") ||
            !asset.Url.AbsoluteUri.StartsWith(Repository + "/releases/download/", StringComparison.Ordinal)) throw new IOException("Invalid installer metadata.");
        var folder = Path.Combine(AppPaths.Data(root), "updates"); PlainPath(folder); Directory.CreateDirectory(folder);
        var target = Path.Combine(folder, asset.Name); PlainPath(target);
        var partial = target + "." + Guid.NewGuid().ToString("N") + ".part";
        try
        {
            using var request = Request(asset.Url);
            using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellation);
            response.EnsureSuccessStatusCode();
            if (response.Content.Headers.ContentLength is long length && length != asset.Size) throw new IOException("Installer size differs from release metadata.");
            await using (var source = await response.Content.ReadAsStreamAsync(cancellation))
            await using (var output = new FileStream(partial, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536, true))
            using (var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
            {
                var buffer = new byte[65536]; long total = 0; int count;
                while ((count = await source.ReadAsync(buffer, cancellation)) != 0)
                {
                    total += count; if (total > asset.Size) throw new IOException("Installer download exceeds its declared size.");
                    hash.AppendData(buffer, 0, count); await output.WriteAsync(buffer.AsMemory(0, count), cancellation);
                    progress?.Report((int)(100 * total / asset.Size));
                }
                if (total != asset.Size || Convert.ToHexStringLower(hash.GetHashAndReset()) != asset.Sha256) throw new IOException("Installer verification failed; download discarded.");
                await output.FlushAsync(cancellation);
            }
            cancellation.ThrowIfCancellationRequested(); File.Move(partial, target, true); return target;
        }
        finally { if (File.Exists(partial)) File.Delete(partial); }
    }
    public static ProcessStartInfo InstallerCommand(string root, string path, ReleaseInstaller asset)
    {
        var expected = Path.GetFullPath(Path.Combine(AppPaths.Data(root), "updates", asset.Name));
        if (!Path.GetFullPath(path).Equals(expected, StringComparison.OrdinalIgnoreCase)) throw new IOException("Installer is outside the download folder.");
        PlainPath(path);
        using var stream = File.OpenRead(path);
        if (stream.Length != asset.Size || Convert.ToHexStringLower(SHA256.HashData(stream)) != asset.Sha256) throw new IOException("Installer changed after download.");
        // Inno Setup contract: install to this app root; keep user settings and saves.
        var command = new ProcessStartInfo(path) { UseShellExecute = true, WorkingDirectory = root };
        command.ArgumentList.Add("/DIR=" + Path.GetFullPath(AppPaths.Game(root))); command.ArgumentList.Add("/NORESTART");
        return command;
    }
    public static void Install(string root, string path, ReleaseInstaller asset)
    {
        using var mutex = new Mutex(false, @"Global\GRID2VR.Session");
        var acquired = false;
        try
        {
            try { acquired = mutex.WaitOne(0); } catch (AbandonedMutexException) { acquired = true; }
            if (!acquired || File.Exists(Path.Combine(AppPaths.Data(root), "session-pending.json"))) throw new IOException("Finish the game session and recovery before installing an update.");
            foreach (var name in new[] { "grid2", "grid2_avx" })
            {
                var processes = Process.GetProcessesByName(name);
                try { if (processes.Length != 0) throw new IOException("Close GRID 2 before installing an update."); }
                finally { foreach (var process in processes) process.Dispose(); }
            }
            using var installer = Process.Start(InstallerCommand(root, path, asset)) ?? throw new IOException("Installer did not start.");
        }
        finally { if (acquired) mutex.ReleaseMutex(); }
    }
    internal static void BackupPreferences(string root)
    {
        var data=AppPaths.Data(root);
        var folder=Path.Combine(data,"updates","rollback-preferences",DateTime.UtcNow.ToString("yyyyMMdd-HHmmss")+"-"+Guid.NewGuid().ToString("N"));
        PlainPath(folder);Directory.CreateDirectory(folder);
        foreach(var name in new[]{"launcher-settings.json","launcher-window.json","driving-controls.json","seat-positions.json","update-preferences.json","profile-selection.json"})
        {
            var source=Path.Combine(data,name);PlainPath(source);
            if(File.Exists(source))File.Copy(source,Path.Combine(folder,name),false);
        }
    }
}
