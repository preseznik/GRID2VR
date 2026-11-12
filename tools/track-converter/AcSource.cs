// Assetto Corsa layout reader. Spline and model handling adapted from DiRT2VR
// tools/nordschleife-converter/SourceScene.cs (a617f7a), same author; surface
// codes target GRID 2's surface_materials.xml instead of DiRT 2's.
using System.Globalization;
using System.Numerics;
using System.Text.RegularExpressions;
using EgoEngineLibrary.Formats.TrackQuadTree;

namespace GRID2VR.Tracks;

internal sealed class AcSource
{
    internal readonly List<QuadTreeDataTriangle> Collision = [];
    internal readonly List<Mesh> Visuals = [];
    internal readonly Dictionary<string, Texture> Textures = new(StringComparer.OrdinalIgnoreCase);
    internal VertexAmbient? Ambient { get; private set; }
    internal readonly Dictionary<string, string> Inputs = new(StringComparer.OrdinalIgnoreCase);
    internal readonly Dictionary<string, long> Surfaces = new(StringComparer.Ordinal);
    internal readonly Dictionary<string, string> CollisionMappings = new(StringComparer.Ordinal);
    internal Gate[] Gates = [];
    internal Vector3 Origin;
    internal float Length;

    // GRID 2 road surface used for grid and recovery height checks.
    internal const string Road = "TSD*";
    internal static readonly HashSet<string> RoadSurfaces = new(StringComparer.Ordinal) { Road };

    internal static bool IsGameMarker(string name) => new[] { "AC_START_", "AC_PIT_", "AC_TIME_", "AC_HOTLAP_", "AC_AUDIO_" }
        .Any(prefix => name.StartsWith(prefix, StringComparison.Ordinal));

    // Reads the closed driving loop and every physical mesh of one layout. Positions keep
    // metres, handedness and elevation; only a translation is applied (origin + shift).
    internal static AcSource Read(string trackRoot, string layout, Func<Vector3, Vector3, Vector3> placeOrigin, bool visuals = false)
    {
        var source = new AcSource();
        string Input(string relative)
        {
            string path = Files.Inside(trackRoot, relative); source.Inputs[path] = Files.Hash(path); return path;
        }
        var models = Ini(File.ReadAllText(Input($"models_{layout}.ini")));
        _ = Input($"{layout}/data/surfaces.ini");
        Vector3[] points; (float L, float R)[] sides;
        using (var r = new BinaryReader(File.OpenRead(Input($"{layout}/ai/fast_lane.ai"))))
        {
            if (r.ReadInt32() != 7) throw new InvalidDataException("Unsupported driving spline version.");
            int n = r.ReadInt32(); r.ReadInt32(); r.ReadInt32();
            if (n is < 10 or > 100000) throw new InvalidDataException("Invalid driving spline point count.");
            points = new Vector3[n];
            for (int i = 0; i < n; i++)
            {
                points[i] = new(r.ReadSingle(), r.ReadSingle(), r.ReadSingle()); r.ReadSingle(); r.ReadInt32();
                if (!Kn5.Finite(points[i])) throw new InvalidDataException("Nonfinite spline point.");
            }
            if (r.ReadInt32() != n) throw new InvalidDataException("Missing driving spline side widths.");
            sides = new (float, float)[n];
            for (int i = 0; i < n; i++)
            {
                var values = new float[18]; for (int j = 0; j < 18; j++) values[j] = r.ReadSingle();
                sides[i] = (values[5], values[6]);
                if (!float.IsFinite(sides[i].L) || !float.IsFinite(sides[i].R) || sides[i].L <= 0 || sides[i].R <= 0)
                    throw new InvalidDataException("Invalid spline side widths.");
            }
        }
        // Optional and outside the pinned track files: hashed into the build's inputs by the caller.
        if (visuals) source.Ambient = VertexAmbient.TryLoad(trackRoot, Console.WriteLine);
        var min = points.Aggregate(Vector3.Min); var max = points.Aggregate(Vector3.Max);
        source.Origin = placeOrigin(min, max);
        var gates = new List<Gate>(); float distance = 0;
        for (int i = 0; i < points.Length; i++)
        {
            if (i > 0) distance += Vector3.Distance(points[i - 1], points[i]);
            if (i > 0 && distance - gates[^1].Distance < 5) continue;
            var tangent = Vector3.Normalize(points[(i + 1) % points.Length] - points[(i + points.Length - 1) % points.Length]);
            if (!Kn5.Finite(tangent)) throw new InvalidDataException("Invalid driving spline tangent.");
            gates.Add(new(points[i] - source.Origin, tangent, sides[i].L, sides[i].R, distance));
        }
        source.Gates = gates.ToArray();
        source.Length = distance + Vector3.Distance(points[^1], points[0]);

        foreach (var (section, values) in models.Where(x => x.Key.StartsWith("MODEL_", StringComparison.Ordinal)))
        {
            if (Vector(values["ROTATION"]) != Vector3.Zero) throw new InvalidDataException("Unsupported rotated model placement: " + section);
            var position = Vector(values["POSITION"]);
            string path = Input(values["FILE"]); Console.WriteLine("Reading " + Path.GetFileName(path));
            // Without visuals, textures are read past and released per file.
            var textures = visuals ? source.Textures : new Dictionary<string, Texture>(StringComparer.OrdinalIgnoreCase);
            Kn5.Read(path, Matrix4x4.CreateTranslation(position - source.Origin), textures, mesh =>
            {
                // Numeric (physical) source meshes can also be the visible asphalt and kerbs.
                if (visuals && mesh.Visible && !IsGameMarker(mesh.Name)) source.Visuals.Add(source.Ambient?.Apply(mesh) ?? mesh);
                if (!mesh.Active || !mesh.Physical) return;
                string surface = Surface(mesh.Name);
                source.CollisionMappings[mesh.Name] = surface;
                for (int i = 0; i < mesh.Indices.Length; i += 3)
                {
                    source.Collision.Add(new(mesh.Positions[mesh.Indices[i]], mesh.Positions[mesh.Indices[i + 1]], mesh.Positions[mesh.Indices[i + 2]], surface));
                    source.Surfaces[surface] = source.Surfaces.GetValueOrDefault(surface) + 1;
                }
            });
        }
        if (source.Collision.Count == 0) throw new InvalidDataException("No source collision selected.");
        if (visuals && source.Visuals.Count == 0) throw new InvalidDataException("No source visuals selected.");
        return source;
    }

    internal static Mesh Compact(Mesh mesh, IReadOnlyList<ushort> selected)
    {
        var used = selected.Distinct().Order().ToArray(); var map = used.Select((old, index) => (old, index)).ToDictionary(x => x.old, x => checked((ushort)x.index));
        return mesh with
        {
            Positions = used.Select(i => mesh.Positions[i]).ToArray(), Normals = used.Select(i => mesh.Normals[i]).ToArray(), UV = used.Select(i => mesh.UV[i]).ToArray(),
            Tangents = used.Select(i => mesh.Tangents[i]).ToArray(), Indices = selected.Select(i => map[i]).ToArray(),
            Colors = mesh.Colors is null ? null : used.Select(i => mesh.Colors[i]).ToArray(),
            TexCoords = mesh.TexCoords?.Select(set => used.Select(i => set[i]).ToArray()).ToArray(),
            Binormals = mesh.Binormals is null ? null : used.Select(i => mesh.Binormals[i]).ToArray(),
            Ambient = mesh.Ambient is null ? null : used.Select(i => mesh.Ambient[i]).ToArray(),
            Sun = mesh.Sun is null ? null : used.Select(i => mesh.Sun[i]).ToArray()
        };
    }

    // AC physical mesh names start with a digit and a surface key from surfaces.ini.
    // '*' is GRID 2's usual drivable variant; '+' is used for the stock vertical walls.
    internal static string Surface(string name)
    {
        string key = Regex.Replace(name, "^[0-9]+", "").ToUpperInvariant();
        if (key.StartsWith("TRM") || key.StartsWith("ROAD") || key.StartsWith("ASPH") || key.StartsWith("PITS") || key.StartsWith("PTRMBL") || key.StartsWith("PAINT")) return Road;
        if (key.StartsWith("CURB") || key.StartsWith("CRB") || key.StartsWith("CUTKRB")) return "RBL*";
        if (key.StartsWith("CNC") || key.StartsWith("CONCRETE")) return "CON*";
        if (key.StartsWith("WALL") || key.StartsWith("3DIMP") || key.StartsWith("3DMISC")) return "CON+";
        if (key.StartsWith("GRASS") || key.StartsWith("CUTGRA") || key.StartsWith("CARPET") || key.StartsWith("OUT")) return "GRS*";
        if (key.StartsWith("GRAVEL")) return "GTP*";
        if (key.StartsWith("SAND")) return "SDT*";
        throw new InvalidDataException("Unmapped collision surface: " + name);
    }

    internal float RoadHeight(Vector3 position) => SurfaceHeight(position, RoadSurfaces.Contains)
        ?? throw new InvalidDataException($"No road collision under {position} (surface there: {SurfaceAt(position) ?? "none"}).");

    internal string? SurfaceAt(Vector3 position)
    {
        string? found = null;
        SurfaceHeight(position, material => { found ??= material; return false; });
        return found;
    }

    float? SurfaceHeight(Vector3 position, Func<string, bool> accept)
    {
        float? closest = null;
        foreach (var t in Collision)
        {
            var a = t.Position0; var b = t.Position1; var c = t.Position2;
            float divisor = (b.Z - c.Z) * (a.X - c.X) + (c.X - b.X) * (a.Z - c.Z);
            if (MathF.Abs(divisor) < .000001f) continue;
            float u = ((b.Z - c.Z) * (position.X - c.X) + (c.X - b.X) * (position.Z - c.Z)) / divisor;
            float v = ((c.Z - a.Z) * (position.X - c.X) + (a.X - c.X) * (position.Z - c.Z)) / divisor;
            float w = 1 - u - v;
            if (MathF.Min(u, MathF.Min(v, w)) < -.00001f) continue;
            float height = u * a.Y + v * b.Y + w * c.Y;
            if (MathF.Abs(height - position.Y) > 4) continue;
            if (!accept(t.Material)) continue;
            if (closest is null || MathF.Abs(height - position.Y) < MathF.Abs(closest.Value - position.Y)) closest = height;
        }
        return closest;
    }

    // Position along the closed loop, metres before the start line.
    internal Gate BeforeStart(float metres)
    {
        float distance = Length - metres;
        int index = Array.FindLastIndex(Gates, g => g.Distance <= distance);
        if (index < 0) throw new InvalidDataException("Missing starting approach.");
        var a = Gates[index];
        var b = index + 1 < Gates.Length ? Gates[index + 1] : Gates[0] with { Distance = Length };
        float at = (distance - a.Distance) / (b.Distance - a.Distance);
        return new(Vector3.Lerp(a.Position, b.Position, at), Vector3.Normalize(Vector3.Lerp(a.Tangent, b.Tangent, at)),
            a.Left + (b.Left - a.Left) * at, a.Right + (b.Right - a.Right) * at, -metres);
    }

    internal static Dictionary<string, Dictionary<string, string>> Ini(string text)
    {
        var result = new Dictionary<string, Dictionary<string, string>>(StringComparer.Ordinal); Dictionary<string, string>? current = null;
        foreach (var raw in text.Split('\n'))
        {
            string line = raw.Trim(); if (line.Length == 0 || line.StartsWith(';') || line.StartsWith('#')) continue;
            if (line.StartsWith('[') && line.EndsWith(']')) { current = new(StringComparer.Ordinal); result.Add(line[1..^1], current); continue; }
            int index = line.IndexOf('='); if (index > 0 && current is not null) current.Add(line[..index].Trim(), line[(index + 1)..].Trim());
        }
        return result;
    }

    static Vector3 Vector(string text)
    {
        var p = text.Split(',').Select(x => float.Parse(x, CultureInfo.InvariantCulture)).ToArray();
        if (p.Length != 3 || p.Any(x => !float.IsFinite(x))) throw new InvalidDataException("Invalid model placement."); return new(p[0], p[1], p[2]);
    }
}
