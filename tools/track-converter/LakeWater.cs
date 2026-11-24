// A track's lake as GRID 2 native non-interactive water (niwater.pssg + niwater.xml +
// the location's waterdefs), with its own timed waves, reflections and fresnel, as
// DiRT2VR does for Misty Loch (tools/misty-converter/MistyWater.cs, same author).
// GRID 2's water_v2 group takes positions only (stock rivers: Indianapolis, Paris);
// its normal and noise textures come with the donor file. The source lake meshes are
// taken out of the terrain so they are not drawn twice.
using System.Numerics;
using System.Xml.Linq;
using EgoEngineLibrary.Formats.Pssg;
using EgoEngineLibrary.Formats.TrackQuadTree;
using EgoEngineLibrary.Graphics.Pssg.Elements;

namespace GRID2VR.Tracks;

internal static class LakeWater
{
    // Stock river patch (water_v2.fx, two render nodes) and a mountain-lake water definition.
    internal const string Donor = "tracks/locations/circuits/indianapolis/route_0/niwater.pssg";
    internal const string DonorDefinitions = "tracks/locations/p2p/okutama/waterdefs_day.xml";
    const string Group = "water_v2.fx";
    sealed class State : PssgModelWriterState { }

    // Planar water: GRID 2 renders each patch's reflection (ReflectionMap/ReflectionMap2, 2D
    // targets) by mirroring the camera about that patch's plane. Misty's lake1 mesh also covers
    // its streams and cascades (14-87 m), so one patch reflected about a wrong height (iztok,
    // 2026-11-23: "lake reflections seem wrong"). Only flat, near-horizontal triangles become
    // native water; sloped ones (streams, cascades) go back to the scenery with their material.
    internal const float FlatNormalY = .995f, FlatHeightRange = .25f, PatchHeightRange = .5f;
    static bool Flat(Vector3 a, Vector3 b, Vector3 c)
    {
        var n = Vector3.Cross(b - a, c - a); float length = n.Length();
        return length > 1e-9f && MathF.Abs(n.Y) / length >= FlatNormalY && MathF.Max(a.Y, MathF.Max(b.Y, c.Y)) - MathF.Min(a.Y, MathF.Min(b.Y, c.Y)) <= FlatHeightRange;
    }

    internal static (List<Mesh> Water, object Report) Detach(AcSource source, IReadOnlyCollection<string> materials)
    {
        var lake = source.Visuals.Where(m => materials.Contains(m.Material.Name)).ToList();
        source.Visuals.RemoveAll(m => materials.Contains(m.Material.Name));
        // Flat triangles, then their connected bodies (shared positions). GRID 2's water shader has
        // two reflection maps and stock venues use a few patches, so only the largest flat body
        // (the loch) stays native water; small pools at other heights return to the scenery too.
        var flatTriangles = new List<(int Mesh, int At)>(); int sloped = 0;
        for (int m = 0; m < lake.Count; m++)
            for (int i = 0; i + 2 < lake[m].Indices.Length; i += 3)
                if (Flat(lake[m].Positions[lake[m].Indices[i]], lake[m].Positions[lake[m].Indices[i + 1]], lake[m].Positions[lake[m].Indices[i + 2]])) flatTriangles.Add((m, i)); else sloped++;
        var parent = Enumerable.Range(0, flatTriangles.Count).ToArray();
        int Find(int x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; }
        var owner = new Dictionary<Vector3, int>();
        for (int t = 0; t < flatTriangles.Count; t++)
        {
            var (m, at) = flatTriangles[t];
            for (int k = 0; k < 3; k++) { var p = lake[m].Positions[lake[m].Indices[at + k]]; if (owner.TryGetValue(p, out var other)) parent[Find(t)] = Find(other); else owner[p] = t; }
        }
        float Area(int t) { var (m, at) = flatTriangles[t]; var p = lake[m].Positions; var ix = lake[m].Indices; return Vector3.Cross(p[ix[at + 1]] - p[ix[at]], p[ix[at + 2]] - p[ix[at]]).Length() / 2; }
        var bodies = Enumerable.Range(0, flatTriangles.Count).GroupBy(Find).OrderByDescending(g => g.Sum(Area)).ToArray();
        var native = bodies.Length > 0 ? bodies[0].ToHashSet() : [];
        var water = new List<Mesh>(); int pools = 0;
        for (int m = 0; m < lake.Count; m++)
        {
            var keep = new List<ushort>(); var back = new List<ushort>();
            var nativeAt = flatTriangles.Select((f, t) => (f, t)).Where(x => x.f.Mesh == m && native.Contains(x.t)).Select(x => x.f.At).ToHashSet();
            for (int i = 0; i + 2 < lake[m].Indices.Length; i += 3)
                (nativeAt.Contains(i) ? keep : back).AddRange([lake[m].Indices[i], lake[m].Indices[i + 1], lake[m].Indices[i + 2]]);
            if (keep.Count > 0) water.Add(AcSource.Compact(lake[m], keep));
            if (back.Count > 0) source.Visuals.Add(AcSource.Compact(lake[m], back));
        }
        pools = flatTriangles.Count - native.Count;
        return (water, new { NativeWaterTriangles = native.Count, NativeWaterAreaSquareMetres = bodies.Length > 0 ? bodies[0].Sum(Area) : 0, SmallPoolTrianglesToScenery = pools, PoolBodies = Math.Max(0, bodies.Length - 1),
            SlopedTrianglesToScenery = sloped, FlatNormalY, FlatHeightRange });
    }

    internal static object Write(IReadOnlyList<Mesh> lake, string gameRoot, string venueOut, string routeOut, IDictionary<string, string> inputs)
    {
        if (lake.Count == 0) throw new InvalidDataException("Source lake missing.");
        string donor = Files.Inside(gameRoot, Donor), definitions = Files.Inside(gameRoot, DonorDefinitions);
        foreach (var path in new[] { donor, definitions }) { Files.NoLinks(path); inputs[path] = Files.Hash(path); }
        var original = Files.Pssg(donor);
        var layout = ShaderInputInfo.CreateFromPssg(original).Single(s => s.ShaderGroupId == Group);
        if (layout.BlockInputs.SelectMany(b => b.VertexInputs).Select(v => v.Name).SingleOrDefault() != "Vertex")
            throw new InvalidDataException("Unexpected donor water layout.");

        // Every triangle, then one patch per connected water body (shared vertex positions),
        // as stock venues hold one render node per lake or river (Okutama: five).
        var expected = new List<QuadTreeDataTriangle>();
        foreach (var mesh in lake)
            for (int i = 0; i < mesh.Indices.Length; i += 3)
                expected.Add(new(mesh.Positions[mesh.Indices[i]], mesh.Positions[mesh.Indices[i + 1]], mesh.Positions[mesh.Indices[i + 2]], "lake"));
        var parent = Enumerable.Range(0, expected.Count).ToArray();
        int Find(int x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; }
        var owner = new Dictionary<Vector3, int>();
        for (int t = 0; t < expected.Count; t++)
            foreach (var p in new[] { expected[t].Position0, expected[t].Position1, expected[t].Position2 })
                if (owner.TryGetValue(p, out var other)) parent[Find(t)] = Find(other); else owner[p] = t;
        var bodies = Enumerable.Range(0, expected.Count).GroupBy(Find).Select(g => g.Select(t => expected[t]).ToArray())
            .OrderByDescending(b => b.Length).ToArray();
        // Each patch is one reflection plane: flat to PatchHeightRange.
        foreach (var body in bodies)
        {
            var ys = body.SelectMany(t => new[] { t.Position0.Y, t.Position1.Y, t.Position2.Y }).ToArray();
            if (ys.Max() - ys.Min() > PatchHeightRange) throw new InvalidDataException($"A water patch spans {ys.Max() - ys.Min():0.00} m of height; it would reflect about a wrong plane.");
        }
        // One stock render node per body (its TRANSFORM and BOUNDINGBOX kept), no stock draws or geometry.
        var doc = Files.Document(original);
        var stockNodes = doc.Descendants("RENDERNODE").ToArray();
        var nodeTemplate = new XElement(stockNodes[0]); nodeTemplate.Elements().Where(e => e.Name != "TRANSFORM" && e.Name != "BOUNDINGBOX").Remove();
        var sceneRoot = stockNodes[0].Parent!;
        foreach (var stock in stockNodes) stock.Remove();
        for (int b = 0; b < bodies.Length; b++)
        {
            var copy = new XElement(nodeTemplate); copy.SetAttributeValue("id", $"water_g2vr_lake{b}"); copy.SetAttributeValue("nickname", $"water_g2vr_lake{b}");
            sceneRoot.Add(copy);
        }
        doc.Descendants("LIBRARY").Single(l => (string?)l.Attribute("type") == "SEGMENTSET").RemoveNodes();
        doc.Descendants("LIBRARY").Single(l => (string?)l.Attribute("type") == "RENDERINTERFACEBOUND").Elements().Where(e => e.Name != "TEXTURE").Remove();
        var file = Files.FromDocument(doc, original);
        var shader = file.Elements<PssgShaderInstance>().Single(i => i.ShaderGroup == "#" + Group);
        var segmentLib = file.Elements<PssgLibrary>().Single(l => l.Type == "SEGMENTSET");
        var dataLib = file.Elements<PssgLibrary>().Single(l => l.Type == "RENDERINTERFACEBOUND");
        if (dataLib.ChildElements.OfType<PssgTexture>().Count() < 2 || file.Elements<PssgRenderInstance>().Any()) throw new InvalidDataException("Unexpected donor water patch.");
        var state = new State(); var patches = new List<object>(); var names = new List<string>();
        for (int b = 0; b < bodies.Length; b++)
        {
            string name = $"water_g2vr_lake{b}", data = $"g2vr_lake{b}_RDS";
            var node = file.Elements<PssgRenderNode>().Single(n => n.Id == name);
            var writer = new RenderDataSourceWriter(data); var index = new Dictionary<Vector3, uint>();
            foreach (var t in bodies[b])
                foreach (var p in new[] { t.Position0, t.Position1, t.Position2 })
                {
                    if (!index.TryGetValue(p, out var i)) { index[p] = i = (uint)writer.Positions.Count; writer.Positions.Add(p); }
                    writer.Indices.Add(i);
                }
            if (writer.Positions.Count > ushort.MaxValue) throw new InvalidDataException("A water body exceeds one patch.");
            var segment = new PssgSegmentSet(file, segmentLib) { Id = $"g2vr_lake{b}_set", SegmentCount = 1 }; segmentLib.AppendChild(segment);
            writer.Write(layout, segment, dataLib, state);
            var drawn = new PssgRenderStreamInstance(file, node) { Id = $"g2vr_lake{b}_draw", SourceCount = 1, Indices = "#" + data, StreamCount = 0, Shader = "#" + shader.Id };
            node.AppendChild(drawn); drawn.AppendChild(new PssgRenderInstanceSource(file, drawn) { Source = "#" + data });
            // GRID 2 culls render nodes by their box: each body's real bounds.
            var min = writer.Positions.Aggregate(Vector3.Min) - new Vector3(.3f); var max = writer.Positions.Aggregate(Vector3.Max) + new Vector3(.3f);
            node.Transform.Transform = Matrix4x4.Identity; node.BoundingBox.BoundsMin = min; node.BoundingBox.BoundsMax = max;
            names.Add(name);
            patches.Add(new { Node = name, Triangles = bodies[b].Length, Vertices = writer.Positions.Count, MinY = min.Y + .3f, MaxY = max.Y - .3f, Size = new[] { max.X - min.X, max.Z - min.Z } });
        }
        var all = expected.SelectMany(t => new[] { t.Position0, t.Position1, t.Position2 }).ToArray();
        var rootNode = file.Elements<EgoEngineLibrary.Graphics.Pssg.PssgElement>().Single(e => e.Name == "ROOTNODE");
        if (rootNode.ChildElements.OfType<PssgTransform>().SingleOrDefault() is { } rootTransform) rootTransform.Transform = Matrix4x4.Identity;
        if (rootNode.ChildElements.OfType<PssgBoundingBox>().SingleOrDefault() is { } rootBox)
        { rootBox.BoundsMin = all.Aggregate(Vector3.Min) - new Vector3(.3f); rootBox.BoundsMax = all.Aggregate(Vector3.Max) + new Vector3(.3f); }
        string output = Path.Combine(routeOut, "niwater.pssg");
        Files.NoLinks(output); File.Delete(output);
        Files.Save(file, output);

        // Read back: the same triangles, same winding, nothing else.
        var actual = new List<QuadTreeDataTriangle>();
        foreach (var data in Files.Pssg(output).Elements<PssgRenderDataSource>())
        {
            var r = new RenderDataSourceReader(data);
            for (int i = 0; i < r.IndexCount; i += 3) actual.Add(new(r.GetPosition(r.GetIndex(i)), r.GetPosition(r.GetIndex(i + 1)), r.GetPosition(r.GetIndex(i + 2)), "lake"));
        }
        if (actual.Count != expected.Count || GeometryCoverage.Unmatched(expected, actual) != 0 || GeometryCoverage.Unmatched(actual, expected) != 0)
            throw new InvalidDataException("Lake geometry changed in the water patch.");

        string xml = Path.Combine(routeOut, "niwater.xml"); Files.NoLinks(xml); File.Delete(xml);
        // Plain text, as every stock niwater.xml.
        using (var stream = new FileStream(xml, FileMode.CreateNew))
        using (var text = System.Xml.XmlWriter.Create(stream, new System.Xml.XmlWriterSettings { OmitXmlDeclaration = true, Indent = true, IndentChars = "  ", Encoding = new System.Text.UTF8Encoding(false) }))
            new XElement("interactiveWater", names.Select((name, index) => new XElement("interactiveWaterPatch", new XAttribute("index", index),
                new XAttribute("uri", "niwater.pssg#" + name), new XAttribute("type", "water"), new XAttribute("prioritise_envmap", 1)))).Save(text);
        // The location's water definitions (type "water" = waterDef "Water"); a new file in the converted venue.
        string defs = Path.Combine(venueOut, "waterdefs_day.xml");
        if (File.Exists(defs)) throw new InvalidDataException("Donor venue already has water definitions.");
        File.Copy(definitions, defs);
        if (!XDocument.Load(defs).Root!.Elements("waterDef").Any(e => (string?)e.Attribute("type") == "Water")) throw new InvalidDataException("Water definition missing.");
        return new
        {
            Donor, Definitions = DonorDefinitions, Group, SourceMeshes = lake.Count, Triangles = expected.Count, GeometryExact = true, Patches = patches
        };
    }
}
