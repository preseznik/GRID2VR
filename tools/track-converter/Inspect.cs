using System.Text.Json;
using System.Xml.Linq;
using EgoEngineLibrary.Archive.Jpk;
using EgoEngineLibrary.Formats.TrackQuadTree;
using EgoEngineLibrary.Formats.TrackQuadTree.Static;
using EgoEngineLibrary.Xml;

namespace GRID2VR.Tracks;

// Read-only decoding of a stock GRID 2 venue/route into a new folder, so the
// converter can be written against the real GRID 2 schemas and limits.
internal static class Inspect
{
    internal static void Run(string gameRoot, string trackId, string output)
    {
        var parts = trackId.Split('/');
        if (parts.Length != 3) throw new ArgumentException("Track id must be category/location/route_N.");
        string venue = Files.Inside(gameRoot, $"tracks/locations/{parts[0]}/{parts[1]}");
        string route = Files.Inside(venue, parts[2]);
        Files.NewOutput(output, gameRoot);
        var report = new Dictionary<string, object>(StringComparer.Ordinal);
        var files = Directory.EnumerateFiles(venue, "*", SearchOption.TopDirectoryOnly)
            .Concat(Directory.EnumerateFiles(route, "*", SearchOption.AllDirectories));
        foreach (var path in files.Order(StringComparer.OrdinalIgnoreCase))
        {
            Files.NoLinks(path);
            string relative = Path.GetRelativePath(venue, path).Replace('\\', '/');
            try { report[relative] = Describe(path, Path.Combine(output, "decoded", relative)); }
            catch (Exception error) { report[relative] = new { Error = error.GetType().Name + ": " + error.Message }; }
        }
        Files.Json(Path.Combine(output, "inspect.json"), report);
        Console.WriteLine($"Inspected {report.Count} files: {output}");
    }

    // Scene-graph outline of a (large) PSSG: libraries, then nodes to a depth, with
    // render-instance counts and shader groups; bulk data is never printed.
    internal static void Tree(string input, string output, int depth)
    {
        Files.NoLinks(input);
        if (File.Exists(output)) throw new IOException("Output must be new: " + output);
        var document = Files.Document(Files.Pssg(input));
        var shaderOf = document.Descendants().Where(e => e.Name.LocalName == "SHADERINSTANCE")
            .ToDictionary(e => "#" + (string?)e.Attribute("id"), e => (string?)e.Attribute("shaderGroup") ?? "");
        using var writer = new StreamWriter(new FileStream(output, FileMode.CreateNew));
        foreach (var library in document.Descendants().Where(e => e.Name.LocalName == "LIBRARY"))
        {
            writer.WriteLine($"LIBRARY {(string?)library.Attribute("type")}: {library.Elements().Count()} children; " +
                string.Join(", ", library.Elements().GroupBy(e => e.Name.LocalName).Select(g => g.Key + "x" + g.Count())));
            if ((string?)library.Attribute("type") is "NODE" or "RENDERNODE")
                foreach (var child in library.Elements()) Node(child, 1);
            if ((string?)library.Attribute("type") == "SHADERGROUP")
                foreach (var group in library.Elements())
                    writer.WriteLine($"  SHADERGROUP {(string?)group.Attribute("id")}: " + string.Join(", ", group.Elements("SHADERINPUTDEFINITION")
                        .Select(d => $"{(string?)d.Attribute("name")}:{(string?)d.Attribute("type")}{((string?)d.Attribute("format") is { } f ? "/" + f : "")}")));
        }
        void Node(XElement node, int level)
        {
            var instances = node.Elements().Where(e => e.Name.LocalName is "RENDERSTREAMINSTANCE" or "MODIFIERNETWORKINSTANCE").ToArray();
            var nodes = node.Elements().Where(e => e.Name.LocalName.EndsWith("NODE", StringComparison.Ordinal)).ToArray();
            var shaders = instances.Select(i => shaderOf.GetValueOrDefault((string?)i.Attribute("shader") ?? "", "?")).Distinct().Take(4);
            var attributes = string.Join(" ", node.Attributes().Where(a => a.Name.LocalName is not ("id" or "nickname")).Select(a => $"{a.Name.LocalName}={a.Value}"));
            writer.WriteLine($"{new string(' ', level * 2)}{node.Name.LocalName} {(string?)node.Attribute("id")} [{attributes}] nodes={nodes.Length} draws={instances.Length} {string.Join(",", shaders)}");
            if (level >= depth) return;
            // Long sibling runs: print the first few of each name prefix.
            foreach (var group in nodes.GroupBy(n => System.Text.RegularExpressions.Regex.Replace((string?)n.Attribute("id") ?? "", "[0-9]+", "#")))
            {
                foreach (var child in group.Take(3)) Node(child, level + 1);
                if (group.Count() > 3) writer.WriteLine($"{new string(' ', (level + 1) * 2)}... {group.Count() - 3} more {group.Key}");
            }
        }
    }

    // Vertex streams each shader group reads, as the PSSG writer will need them.
    internal static void Layouts(string input)
    {
        var pssg = Files.Pssg(input);
        foreach (var layout in EgoEngineLibrary.Formats.Pssg.ShaderInputInfo.CreateFromPssg(pssg).OrderBy(l => l.ShaderGroupId, StringComparer.Ordinal))
            Console.WriteLine($"{layout.ShaderGroupId}: " + string.Join(" | ", layout.BlockInputs.Select(b =>
                string.Join(",", b.VertexInputs.Select(v => $"{v.Name}:{v.DataType}")))));
        // How instances reference textures (container prefix), per shader group.
        var document = Files.Document(pssg);
        var groups = document.Descendants().Where(e => e.Name.LocalName == "SHADERGROUP")
            .ToDictionary(g => "#" + (string?)g.Attribute("id"), g => g.Elements("SHADERINPUTDEFINITION").Select(d => (string?)d.Attribute("name") ?? "").ToArray());
        foreach (var instance in document.Descendants().Where(e => e.Name.LocalName == "SHADERINSTANCE")
                     .GroupBy(i => (string?)i.Attribute("shaderGroup") ?? "").Select(g => g.First()))
        {
            var names = groups.GetValueOrDefault((string?)instance.Attribute("shaderGroup") ?? "", []);
            Console.WriteLine($"  {(string?)instance.Attribute("shaderGroup")} e.g. {(string?)instance.Attribute("id")}: " + string.Join(", ", instance.Elements("SHADERINPUT")
                .Where(i => (string?)i.Attribute("texture") is not null)
                .Select(i => $"{names.ElementAtOrDefault((int?)i.Attribute("parameterID") ?? -1)}={(string?)i.Attribute("texture")}")));
        }
    }

    internal static void Decode(string input, string output)
    {
        Files.NoLinks(input);
        if (File.Exists(output)) throw new IOException("Output must be new: " + output);
        Console.WriteLine(JsonSerializer.Serialize(Describe(input, output)));
        if (!File.Exists(output + ".txt")) throw new InvalidDataException("No text form for: " + input);
    }

    static object Describe(string path, string decoded)
    {
        var bytes = File.ReadAllBytes(path);
        string extension = Path.GetExtension(path).ToLowerInvariant();
        var info = new Dictionary<string, object> { ["Bytes"] = bytes.Length, ["Sha256"] = Files.Hash(path) };
        if (extension == ".jpk")
        {
            using var stream = new MemoryStream(bytes);
            var jpk = new JpkFile(); jpk.Read(stream);
            info["Entries"] = jpk.Entries.Select(e => new { e.Name, Bytes = e.Data.Length }).ToArray();
            var type = TrackGround.Identify(jpk);
            info["CollisionType"] = type.Type.ToString();
            var ground = TrackGround.Load(jpk, type);
            var triangles = ground.TraverseGrid().SelectMany(c => c.QuadTree.GetTriangles()).ToArray();
            info["Triangles"] = triangles.Length;
            info["Materials"] = triangles.GroupBy(t => t.Material).OrderBy(g => g.Key, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.Count());
            // Mean |normal.y| per surface: 1 = flat ground, 0 = vertical wall.
            info["MaterialUpness"] = triangles.GroupBy(t => t.Material).OrderBy(g => g.Key, StringComparer.Ordinal).ToDictionary(g => g.Key, g => MathF.Round(g.Average(t =>
            {
                var n = System.Numerics.Vector3.Cross(t.Position1 - t.Position0, t.Position2 - t.Position0);
                return n.LengthSquared() > 0 ? MathF.Abs(System.Numerics.Vector3.Normalize(n).Y) : 0;
            }), 2));
            info["BoundsMin"] = Vec(triangles.Select(t => System.Numerics.Vector3.Min(t.Position0, System.Numerics.Vector3.Min(t.Position1, t.Position2))).Aggregate(System.Numerics.Vector3.Min));
            info["BoundsMax"] = Vec(triangles.Select(t => System.Numerics.Vector3.Max(t.Position0, System.Numerics.Vector3.Max(t.Position1, t.Position2))).Aggregate(System.Numerics.Vector3.Max));
        }
        else if (extension == ".cqtc")
        {
            var tree = new CQuadTreeFile(bytes);
            var triangles = tree.GetTriangles();
            info["Triangles"] = triangles.Length;
            info["Materials"] = triangles.GroupBy(t => t.Material).ToDictionary(g => g.Key, g => g.Count());
            info["BoundsMin"] = Vec(tree.BoundsMin); info["BoundsMax"] = Vec(tree.BoundsMax);
        }
        else if (extension is ".xml" or ".ens" || (extension == ".pssg" && bytes.Length > 5 && bytes[0] == (byte)'<'))
        {
            using var stream = new MemoryStream(bytes);
            var xml = new XmlFile(stream);
            info["XmlType"] = xml.Type.ToString();
            info["Root"] = xml.Document.DocumentElement?.Name ?? "";
            if (bytes.Length <= 4 * 1024 * 1024) Save(decoded + ".txt", writer => xml.WriteXml(writer));
        }
        else if (extension == ".pssg")
        {
            var pssg = Files.Pssg(path);
            var document = Files.Document(pssg);
            var elements = document.Descendants().ToArray();
            info["ElementCounts"] = elements.GroupBy(e => e.Name.LocalName).OrderByDescending(g => g.Count()).ToDictionary(g => g.Key, g => g.Count());
            info["ShaderGroups"] = elements.Where(e => e.Name.LocalName == "SHADERINSTANCE")
                .GroupBy(e => (string?)e.Attribute("shaderGroup") ?? "").ToDictionary(g => g.Key, g => g.Count());
            info["TextureFormats"] = elements.Where(e => e.Name.LocalName == "TEXTURE")
                .GroupBy(e => (string?)e.Attribute("texelFormat") ?? "").ToDictionary(g => g.Key, g => g.Count());
            if (bytes.Length <= 3 * 1024 * 1024)
            {
                // Keep the structure readable: drop bulk payloads.
                foreach (var data in document.Descendants().Where(e => e.Name.LocalName is "DATABLOCKDATA" or "INDEXSOURCEDATA" or "TEXTUREIMAGEBLOCKDATA" or "SHADERPROGRAMCODEBLOCK").ToArray())
                    data.Value = $"[{data.Value.Length} chars omitted]";
                Save(decoded + ".txt", writer => document.Save(writer));
            }
        }
        return info;
    }

    static float[] Vec(System.Numerics.Vector3 v) => [v.X, v.Y, v.Z];
    static void Save(string path, Action<TextWriter> write)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var writer = new StreamWriter(new FileStream(path, FileMode.CreateNew));
        write(writer);
    }
}
