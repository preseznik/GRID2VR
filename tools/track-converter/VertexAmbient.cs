// Adapted from DiRT2VR tools/nordschleife-converter/VertexAmbient.cs, same author.
using System.IO.Compression;
using System.Numerics;
using System.Text;

namespace GRID2VR.Tracks;

// Optional per-vertex ambient occlusion from Custom Shaders Patch
// (extension/vao-patches/<track>.vao-patch, CSP's original Patch.data format: name,
// type, first vertex, vertex count, then RGB (type 0) or scalar (type 1) half floats).
// Unlike DiRT2VR this never fails a build: players may have another CSP version or
// none, so an unreadable file is ignored and an unmatched mesh keeps neutral ambient.
// Match before compaction, batching or tiling changes the vertex order.
internal sealed class VertexAmbient
{
    sealed record Entry(string Name, Vector3 First, float[] Values);
    readonly Dictionary<string, Entry[]> entries;
    readonly HashSet<Entry> used = [];
    int unmatched, unmatchedVertices, matchedVertices;
    readonly float opacity, brightness;
    internal string Path { get; }
    internal string Hash { get; }

    VertexAmbient(string path, string hash, Dictionary<string, Entry[]> entries, float opacity, float brightness)
    { Path = path; Hash = hash; this.entries = entries; this.opacity = opacity; this.brightness = brightness; }

    // The track's patch next to the AC installation, or null (missing or unreadable).
    internal static VertexAmbient? TryLoad(string trackRoot, Action<string> log)
    {
        string track = System.IO.Path.GetFileName(System.IO.Path.TrimEndingDirectorySeparator(System.IO.Path.GetFullPath(trackRoot)));
        string acRoot = System.IO.Path.GetFullPath(System.IO.Path.Combine(trackRoot, "..", "..", ".."));
        string path = System.IO.Path.Combine(acRoot, "extension", "vao-patches", track + ".vao-patch");
        if (!File.Exists(path)) { log("No Custom Shaders Patch ambient occlusion for " + track + "; neutral ambient."); return null; }
        try
        {
            Files.NoLinks(path);
            string hash = Files.Hash(path);
            using var zip = ZipFile.OpenRead(path);
            if (zip.Entries.Count > 8) throw new InvalidDataException("unexpected archive layout");
            // Patch.data: half-float samples (DiRT2VR's pinned CSP). Patch_v5.data (newer CSP):
            // same records with one byte per sample (/255); 2,425 scalar records on this PC.
            bool v5 = zip.GetEntry("Patch.data") is null;
            var patch = zip.GetEntry(v5 ? "Patch_v5.data" : "Patch.data") ?? throw new InvalidDataException("no Patch.data or Patch_v5.data");
            if (patch.Length is < 24 or > 64 * 1024 * 1024) throw new InvalidDataException("patch data size");
            // CSP's track defaults as pinned by DiRT2VR, unless Config.ini [LIGHTING] says otherwise.
            float opacity = .85f, brightness = 1.1f;
            if (zip.GetEntry("Config.ini") is { } config && config.Length <= 4096)
            {
                using var reader = new StreamReader(config.Open());
                if (AcSource.Ini(reader.ReadToEnd()).TryGetValue("LIGHTING", out var values))
                {
                    float Value(string name, float fallback) => values.TryGetValue(name, out var v) &&
                        float.TryParse(v, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var f) && float.IsFinite(f) ? f : fallback;
                    opacity = Math.Clamp(Value("OPACITY", opacity), 0, 1); brightness = Math.Clamp(Value("BRIGHTNESS", brightness), .5f, 2);
                }
            }
            using var data = new MemoryStream(); using (var input = patch.Open()) input.CopyTo(data);
            if (data.Length != patch.Length) throw new InvalidDataException("truncated Patch.data");
            data.Position = 0; using var r = new BinaryReader(data, new UTF8Encoding(false, true)); var list = new List<Entry>();
            while (data.Position < data.Length)
            {
                int length = r.ReadInt32(); if (length is < 1 or > 1024) throw new InvalidDataException("mesh name length");
                string name = new UTF8Encoding(false, true).GetString(r.ReadBytes(length)); int type = r.ReadInt32();
                var first = new Vector3(r.ReadSingle(), r.ReadSingle(), r.ReadSingle()); int count = r.ReadInt32();
                long sampleBytes = v5 ? 1 : 2;
                if (type is not (0 or 1) || !Kn5.Finite(first) || count is < 1 or > 65535 || list.Count >= 20000 || data.Length - data.Position < count * sampleBytes * (type == 0 ? 3 : 1))
                    throw new InvalidDataException("mesh record " + name);
                float Sample()
                {
                    if (v5) return r.ReadByte() / 255f;
                    float v = (float)BitConverter.UInt16BitsToHalf(r.ReadUInt16()); if (!float.IsFinite(v) || v > 1.001f) throw new InvalidDataException("sample"); return Math.Clamp(v, 0, 1);
                }
                var samples = new float[count];
                for (int i = 0; i < count; i++) samples[i] = type == 0 ? (Sample() + Sample() + Sample()) / 3 : Sample();
                list.Add(new(name, first, samples));
            }
            if (Files.Hash(path) != hash) throw new InvalidDataException("changed while reading");
            log($"Custom Shaders Patch ambient occlusion: {list.Count:N0} meshes ({System.IO.Path.GetFileName(path)}, {(v5 ? "v5" : "v1")}).");
            return new(path, hash, list.GroupBy(e => e.Name, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.ToArray(), StringComparer.Ordinal), opacity, brightness);
        }
        catch (Exception e) when (e is InvalidDataException or IOException or EndOfStreamException or DecoderFallbackException)
        {
            log("Custom Shaders Patch ambient occlusion not used (" + e.Message + "); neutral ambient.");
            return null;
        }
    }

    // Per-vertex ambient factor in 0..1 (1 = open sky), as CSP applies it.
    internal Mesh Apply(Mesh mesh)
    {
        if (entries.TryGetValue(mesh.Name, out var candidates))
        {
            var matches = candidates.Where(e => e.Values.Length == mesh.Positions.Length && Vector3.Distance(e.First, mesh.SourceFirstPosition) < .002f && !used.Contains(e)).ToArray();
            if (matches.Length == 1)
            {
                used.Add(matches[0]); matchedVertices += mesh.Positions.Length;
                return mesh with { Ambient = matches[0].Values.Select(v => Math.Clamp((1 - opacity + v * opacity) * brightness, 0, 1)).ToArray() };
            }
        }
        unmatched++; unmatchedVertices += mesh.Positions.Length;
        return mesh;
    }

    internal object Report() => new
    {
        Source = Path, Sha256 = Hash, Opacity = opacity, Brightness = brightness, Records = entries.Values.Sum(e => e.Length),
        MatchedMeshes = used.Count, MatchedVertices = matchedVertices, UnmatchedMeshes = unmatched, UnmatchedVertices = unmatchedVertices
    };
}
