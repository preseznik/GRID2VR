// Users convert their own copy of the source track. Only the source version a
// conversion was validated on is accepted: every file the converter reads must
// match the pinned SHA-256 (the embedded pins named by its TrackDefinition).
using System.Text.Json;

namespace GRID2VR.Tracks;

internal static class SourcePins
{
    internal sealed record Pin(string Path, string Sha256);
    internal sealed record Manifest(int Schema, string Track, string Layout, Pin[] Files);

    internal static Manifest Load(TrackDefinition definition)
    {
        var name = "GRID2VR.Tracks." + definition.Pins;
        using var stream = typeof(SourcePins).Assembly.GetManifestResourceStream(name)
            ?? throw new InvalidDataException("No supported source version is known for layout " + definition.Layout + ".");
        var manifest = JsonSerializer.Deserialize<Manifest>(stream, new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
            ?? throw new InvalidDataException("Unreadable source pins for " + definition.Layout + ".");
        if (manifest.Schema != 1 || manifest.Layout != definition.Layout || manifest.Track != definition.AcTrack || manifest.Files is not { Length: > 0 })
            throw new InvalidDataException("Unsupported source pins for " + definition.Layout + ".");
        return manifest;
    }

    // Before conversion: the track folder holds exactly the pinned version of each file.
    internal static void Verify(string trackRoot, string layout, Action<string>? progress = null)
    {
        var manifest = Load(TrackDefinitions.For(trackRoot, layout));
        if (!string.Equals(Path.GetFileName(Path.TrimEndingDirectorySeparator(trackRoot)), manifest.Track, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"Choose the {manifest.Track} track folder.");
        int done = 0;
        foreach (var pin in manifest.Files)
        {
            var path = Files.Inside(trackRoot, pin.Path);
            if (!File.Exists(path)) throw new InvalidDataException($"Your {manifest.Track} is missing {pin.Path}. Verify the game's files in Steam, or reinstall the track.");
            if (!string.Equals(Files.Hash(path), pin.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"Your {manifest.Track} file {pin.Path} differs from the supported version. Verify the game's files in Steam, or reinstall the supported track version; modded tracks are not supported.");
            progress?.Invoke($"Checked source {++done}/{manifest.Files.Length}: {pin.Path}");
        }
    }

    // After reading: nothing outside the pinned set was used.
    internal static void CheckInputs(string trackRoot, string layout, IReadOnlyDictionary<string, string> inputs)
    {
        var pinned = Load(TrackDefinitions.For(trackRoot, layout)).Files.ToDictionary(p => Path.GetFullPath(Path.Combine(trackRoot, p.Path)), p => p.Sha256, StringComparer.OrdinalIgnoreCase);
        foreach (var (path, hash) in inputs)
            if (!pinned.TryGetValue(Path.GetFullPath(path), out var expected) || !string.Equals(expected, hash, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Conversion read an unpinned or changed source file: " + Path.GetRelativePath(trackRoot, path));
    }
}
