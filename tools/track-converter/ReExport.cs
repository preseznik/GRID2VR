using System.Numerics;
using System.Xml.Linq;
using EgoEngineLibrary.Formats.Pssg;
using EgoEngineLibrary.Graphics;
using EgoEngineLibrary.Graphics.Pssg;
using EgoEngineLibrary.Graphics.Pssg.Elements;

namespace GRID2VR.Tracks;

// Isolation control: a stock venue's own terrain is read back as plain source meshes
// (geometry + first diffuse texture) and written again through TrackVisuals and
// Visibility.Write, exactly as converted tracks are. Optionally the whole venue is
// shifted (geometry only) to test distance from the donor's lighting data.
// If this renders, the writer is sound and differences lie in the source track.
internal static class ReExport
{
    // Experiment: tiles cycle through vertex weight vectors to identify the right one in one lap.
    internal static readonly Vector4[] WeightVariants = [new(0, .76f, 0, 0), new(0, 0, 0, 0), new(1, 1, 1, 1), new(0, 1, 0, 0)];

    internal static void Run(string gameRoot, string output, bool stockShaders = false, bool weightTest = false, bool stockLod = false, bool keepLand = false, int? fixedTiles = null, bool mergeDraws = true)
    {
        GRID2VR.GameExecutable.Validate(Files.Inside(gameRoot, "grid2_avx.exe"), avx: true);
        Files.NewOutput(output, gameRoot);
        string donor = Files.Inside(gameRoot, RouteProbe.DonorVenue), route = Files.Inside(donor, RouteProbe.DonorRoute);
        const string location = "g2vr_reexport";
        string venueOut = Path.Combine(output, "overlay", "tracks", "locations", "circuits", location), routeOut = Path.Combine(venueOut, RouteProbe.DonorRoute);
        Directory.CreateDirectory(routeOut);
        foreach (var file in Directory.EnumerateFiles(donor)) { Files.NoLinks(file); File.Copy(file, Path.Combine(venueOut, Path.GetFileName(file))); }
        foreach (var file in Directory.EnumerateFiles(route)) { Files.NoLinks(file); File.Copy(file, Path.Combine(routeOut, Path.GetFileName(file))); }

        var stock = Files.Pssg(Path.Combine(donor, "tracksplit.pssg"));
        var source = new AcSource();
        int skipped = 0, draws = 0;
        var materials = new Dictionary<string, Material>(StringComparer.Ordinal);
        foreach (var tile in Visibility.TerrainTiles(stock))
        foreach (var node in tile.ChildElements.OfType<PssgRenderNode>().Where(n => n.Id.StartsWith("HIGH_", StringComparison.Ordinal) || n.Id.StartsWith("NONLOD_", StringComparison.Ordinal)))
        foreach (var draw in node.ChildElements.OfType<PssgRenderStreamInstance>())
        {
            var shader = draw.GetShaderInstance();
            string? texture = DiffuseOf(stock, shader);
            if (texture is null) { skipped++; continue; }
            Material? material;
            if (stockShaders)
            {
                if (!materials.TryGetValue(shader.Id, out material))
                    materials[shader.Id] = material = new Material("stock:" + shader.Id, shader.Id, "ksPerPixel", 0, false, new() { ["txDiffuse"] = texture })
                        { StockShader = shader.Id, StockGroup = shader.ShaderGroup.TrimStart('#') };
            }
            else if (!materials.TryGetValue(texture, out material))
            {
                var element = stock.Elements<PssgTexture>().Single(t => t.Id == texture);
                using var memory = new MemoryStream(); element.ToDdsFile().Write(memory);
                source.Textures[texture] = new Texture(memory.ToArray(), Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(memory.ToArray())));
                materials[texture] = material = new Material("stock:" + texture, texture, "ksPerPixel", 0, false, new() { ["txDiffuse"] = texture });
            }
            var reader = new RenderDataSourceReader(draw.GetRenderDataSource());
            var triangles = reader.GetTriangles().ToArray();
            // Split into 16-bit-indexed meshes.
            for (int start = 0; start < triangles.Length;)
            {
                var map = new Dictionary<uint, ushort>(); var indices = new List<ushort>();
                while (start < triangles.Length && map.Count + 3 <= ushort.MaxValue)
                {
                    foreach (var v in new[] { triangles[start].A, triangles[start].B, triangles[start].C })
                    {
                        if (!map.TryGetValue(v, out var i)) map[v] = i = (ushort)map.Count;
                        indices.Add(i);
                    }
                    start++;
                }
                var order = map.OrderBy(p => p.Value).Select(p => p.Key).ToArray();
                var mesh = new Mesh($"{node.Id}:{draw.Id}", material, false,
                    order.Select(reader.GetPosition).ToArray(),
                    order.Select(i => reader.HasNormals ? Vector3.Normalize(reader.GetNormal(i)) : Vector3.UnitY).ToArray(),
                    order.Select(i => reader.TexCoordSetCount > 0 ? reader.GetTexCoord(i, 0) : Vector2.Zero).ToArray(),
                    order.Select(_ => Vector3.UnitX).ToArray(), [.. indices]);
                // Stock-shader control: carry every stock vertex attribute unchanged.
                if (stockShaders)
                    mesh = mesh with
                    {
                        Colors = order.Select(reader.GetColor).ToArray(),
                        TexCoords = Enumerable.Range(0, reader.TexCoordSetCount).Select(set => order.Select(i => reader.GetTexCoord(i, set)).ToArray()).ToArray()
                    };
                source.Visuals.Add(mesh);
            }
            draws++;
        }
        Console.WriteLine($"Read {draws} stock terrain draws ({skipped} without a diffuse texture), {source.Visuals.Sum(m => m.Indices.Length / 3):N0} triangles, {source.Textures.Count} textures.");
        var (tiles, report) = TrackVisuals.Build(source, donor, venueOut, weightTest ? WeightVariants : null, lodOverrides: !stockLod, keepDonorLand: keepLand, fixedTiles: fixedTiles, mergeDraws: mergeDraws);
        File.Delete(Path.Combine(routeOut, "track.vis"));
        File.WriteAllBytes(Path.Combine(routeOut, "track.vis"), Visibility.Write(tiles, File.ReadAllBytes(Path.Combine(route, "track.vis"))));
        new XDocument(new XElement("config", new XAttribute("cycle_camera", "false"), new XAttribute("infinite_loop", "false"), new XAttribute("skipreplays", "true"),
            new XAttribute("outputfilename", "G2VRTrack"),
            new XElement("track", new XAttribute("country", "circuits"), new XAttribute("name", location), new XAttribute("route", RouteProbe.DonorRoute),
                new XAttribute("lighting", "day"), new XAttribute("weather", "dry"), new XAttribute("laps", 1), new XAttribute("number_cars", 1),
                new XElement("car", new XAttribute("name", "z28"), new XAttribute("livery", "user"), new XAttribute("ai_driver", "Cau_Demo_1_1"),
                    new XAttribute("ai_driver0", "Cau_Demo_1_0"), new XAttribute("grid", 0))))).Save(Path.Combine(output, "overlay", "G2VRTrack.xml"));
        new XDocument(new XElement("ModList", new XElement("Mod", new XAttribute("Path", Path.GetFullPath(Path.Combine(output, "overlay")))))).Save(Path.Combine(output, "modlist.xml"));
        Files.Json(Path.Combine(output, "custom-track.json"), new
        {
            schema = 1, tracks = new[] { new { id = $"circuits/{location}/{RouteProbe.DonorRoute}", label = "Brands Hatch re-export (control)" } }, kind = "reexport-control"
        });
        Files.Json(Path.Combine(output, "build.json"), new { Schema = 1, Kind = "reexport-control", Donor = $"{RouteProbe.DonorVenue}/{RouteProbe.DonorRoute}", StockDraws = draws, SkippedDraws = skipped, Tiles = tiles.Length, Visuals = report, RuntimeValidated = false });
        Console.WriteLine($"Re-export control built ({tiles.Length} tiles): {output}");
    }

    // Smallest step from a working file: stock tracksplit and stock track.vis unchanged
    // (tiles, names, shaders, segment-set grouping), only every draw's data rewritten by
    // RenderDataSourceWriter under new ids, in the segment set that held the original.
    internal static void InPlace(string gameRoot, string output, bool keepIds = false, bool convertedLod = false, bool shiftNames = false, bool newInstanceIds = false, bool regroupSets = false, bool renameSets = false, bool ownBatches = false, bool emptyLow = false, bool xmlRoundTrip = false)
    {
        GRID2VR.GameExecutable.Validate(Files.Inside(gameRoot, "grid2_avx.exe"), avx: true);
        Files.NewOutput(output, gameRoot);
        string donor = Files.Inside(gameRoot, RouteProbe.DonorVenue), route = Files.Inside(donor, RouteProbe.DonorRoute);
        const string location = "g2vr_inplace";
        string venueOut = Path.Combine(output, "overlay", "tracks", "locations", "circuits", location), routeOut = Path.Combine(venueOut, RouteProbe.DonorRoute);
        Directory.CreateDirectory(routeOut);
        foreach (var file in Directory.EnumerateFiles(donor)) { Files.NoLinks(file); File.Copy(file, Path.Combine(venueOut, Path.GetFileName(file))); }
        foreach (var file in Directory.EnumerateFiles(route)) { Files.NoLinks(file); File.Copy(file, Path.Combine(routeOut, Path.GetFileName(file))); }
        var pssg = Files.Pssg(Path.Combine(donor, "tracksplit.pssg"));
        var layouts = ShaderInputInfo.CreateFromPssg(pssg).ToDictionary(l => l.ShaderGroupId);
        var dataLib = pssg.Elements<PssgLibrary>().Single(l => l.Type == "RENDERINTERFACEBOUND");
        var state = new InPlaceState(); int n = 0;
        static string writer0(int i) => "g2vr_inplace_" + i;
        var originalIds = new Dictionary<string, (string Source, string Index, (string Stream, string Block)[] Streams)>();
        var oldSources = new HashSet<PssgRenderDataSource>();
        foreach (var draw in pssg.Elements<PssgRenderStreamInstance>().ToArray())
        {
            var stock = draw.GetRenderDataSource(); oldSources.Add(stock);
            originalIds[writer0(n)] = (stock.Id, stock.ChildElements.OfType<PssgRenderIndexSource>().Single().Id,
                stock.ChildElements.OfType<PssgRenderStream>().Select(s => (s.Id, s.DataBlock.TrimStart('#'))).ToArray());
            var reader = new RenderDataSourceReader(stock);
            var writer = new RenderDataSourceWriter("g2vr_inplace_" + n++);
            for (uint v = 0; v < reader.VertexCount; v++)
            {
                writer.Positions.Add(reader.GetPosition(v)); if (reader.HasNormals) writer.Normals.Add(reader.GetNormal(v));
                writer.Colors.Add(reader.GetColor(v)); writer.Tangents.Add(Vector4.UnitX);
                Vector2 T(int s) => s < reader.TexCoordSetCount ? reader.GetTexCoord(v, s) : Vector2.Zero;
                writer.TexCoords0.Add(T(0)); writer.TexCoords1.Add(T(1)); writer.TexCoords2.Add(T(2)); writer.TexCoords3.Add(T(3));
            }
            for (int i = 0; i < reader.IndexCount; i++) writer.Indices.Add(reader.GetIndex(i));
            var set = (PssgSegmentSet)stock.ParentElement!;
            writer.Write(layouts[draw.GetShaderInstance().ShaderGroup.TrimStart('#')], set, dataLib, state);
            draw.Indices = "#" + writer.Name;
            foreach (var source in draw.ChildElements.OfType<PssgRenderInstanceSource>()) source.Source = "#" + writer.Name;
        }
        // Drop the replaced sources and their data blocks.
        var oldBlocks = oldSources.SelectMany(s => s.ChildElements.OfType<PssgRenderStream>().Select(r => r.DataBlock.TrimStart('#'))).ToHashSet();
        foreach (var source in oldSources) source.ParentElement!.RemoveChild(source);
        foreach (var block in dataLib.ChildElements.OfType<PssgDataBlock>().Where(b => oldBlocks.Contains(b.Id)).ToArray()) dataLib.RemoveChild(block);
        foreach (var set in pssg.Elements<PssgSegmentSet>()) { set.SegmentCount = (uint)set.Segments.Count(); foreach (var s in set.Segments) s.Primitive = "triangles"; }
        var styled = PssgLayout.StockStyle(pssg, "g2vr_");
        if (newInstanceIds)
        {
            // Step A: new ids for every draw instance, as the converter creates them.
            int k = 0;
            foreach (var draw in pssg.Elements<PssgRenderStreamInstance>().ToArray()) draw.Id = $"g2vr_draw_{k++}";
        }
        if (ownBatches)
        {
            // Step D: each HIGHBATCH/LOWBATCH draw replaced by the converter's merged copy of its
            // sibling colour level (unwelded), with the stock batch shader instance.
            var dataLib2 = pssg.Elements<PssgLibrary>().Single(l => l.Type == "RENDERINTERFACEBOUND");
            var batchLayout = layouts["batched_track.fx"]; int b = 0;
            foreach (var batch in pssg.Elements<PssgRenderNode>().Where(n => n.Id.StartsWith("HIGHBATCH_", StringComparison.Ordinal) || n.Id.StartsWith("LOWBATCH_", StringComparison.Ordinal)).ToArray())
            {
                var draw = batch.ChildElements.OfType<PssgRenderStreamInstance>().Single();
                var colour = ((PssgElement)batch.ParentElement!).ChildElements.OfType<PssgRenderNode>().Single(n => n.Id == batch.Id.Replace("BATCH", ""));
                var merged = new RenderDataSourceWriter($"g2vr_batch_{b++}");
                foreach (var d in colour.ChildElements.OfType<PssgRenderStreamInstance>())
                {
                    var r = new RenderDataSourceReader(d.GetRenderDataSource()); uint offset = (uint)merged.Positions.Count;
                    for (uint v = 0; v < r.VertexCount; v++) merged.Positions.Add(r.GetPosition(v));
                    for (int i = 0; i < r.IndexCount; i++) merged.Indices.Add(r.GetIndex(i) + offset);
                }
                if (merged.Positions.Count > ushort.MaxValue) continue; // keep stock where a split would be needed
                var old = draw.GetRenderDataSource(); var set = (PssgSegmentSet)old.ParentElement!;
                merged.Write(batchLayout, set, dataLib2, state);
                set.RemoveChild(old);
                draw.Indices = "#" + merged.Name; foreach (var s in draw.ChildElements.OfType<PssgRenderInstanceSource>()) s.Source = "#" + merged.Name;
                set.SegmentCount = (uint)set.Segments.Count(); foreach (var s in set.Segments) s.Primitive = "triangles";
            }
            PssgLayout.StockStyle(pssg, "g2vr_batch_");
        }
        if (emptyLow)
        {
            // Step E: LOW render nodes without draws (their segment sets removed).
            foreach (var low in pssg.Elements<PssgRenderNode>().Where(n => n.Id.StartsWith("LOW_", StringComparison.Ordinal)).ToArray())
                foreach (var d in low.ChildElements.OfType<PssgRenderStreamInstance>().ToArray())
                {
                    var source = d.GetRenderDataSource(); var set = (PssgSegmentSet)source.ParentElement!;
                    low.RemoveChild(d); set.RemoveChild(source);
                    if (!set.Segments.Any()) set.ParentElement!.RemoveChild(set); else set.SegmentCount = (uint)set.Segments.Count();
                }
        }
        if (renameSets)
        {
            // Step C: same sets, same order and contents; only new ids.
            int k = 0;
            foreach (var set in pssg.Elements<PssgSegmentSet>().ToArray()) set.Id = $"g2vr_set_{k++}";
        }
        if (regroupSets)
        {
            // Step B: one new SEGMENTSET per render node holding that node's sources.
            var segmentLib = pssg.Elements<PssgLibrary>().Single(l => l.Type == "SEGMENTSET");
            foreach (var node in pssg.Elements<PssgRenderNode>().ToArray())
            {
                var draws = node.ChildElements.OfType<PssgRenderStreamInstance>().ToArray();
                if (draws.Length == 0) continue;
                var set = new PssgSegmentSet(pssg, segmentLib) { Id = "g2vr_set_" + node.Id }; segmentLib.AppendChild(set);
                foreach (var source in draws.Select(d => d.GetRenderDataSource()).Distinct().Reverse().ToArray()) // stock: reverse draw order
                {
                    var parent = source.ParentElement!; parent.RemoveChild(source); set.AppendChild(source);
                }
            }
            foreach (var set in pssg.Elements<PssgSegmentSet>().ToArray())
            {
                if (!set.Segments.Any()) set.ParentElement!.RemoveChild(set);
                else { set.SegmentCount = (uint)set.Segments.Count(); }
            }
        }
        if (shiftNames)
            // Test whether ROOT_x_z names are read as grid coordinates: shift both by +10.
            foreach (var tile in Visibility.TerrainTiles(pssg).Where(t => t.Id.StartsWith("ROOT_", StringComparison.Ordinal)))
            {
                var xz = tile.Id[5..].Split('_'); string suffix = $"{int.Parse(xz[0]) + 10}_{int.Parse(xz[1]) + 10}";
                foreach (var node in tile.ChildElements.OfType<PssgRenderNode>()) { string prefix = node.Id[..(node.Id.IndexOf('_') + 1)]; node.Id = prefix + suffix; node.AddAttribute("nickname", node.Id); }
                tile.Id = "ROOT_" + suffix; tile.AddAttribute("nickname", tile.Id);
            }
        if (convertedLod) TrackVisuals.WriteLodOverrides(donor, venueOut, 128, 128); // as the converter writes for Brands-sized tiles
        if (keepIds)
        {
            // Give the rewritten data the exact stock ids (sources, index sources, streams, blocks).
            var blocks = dataLib.ChildElements.OfType<PssgDataBlock>().ToDictionary(b => b.Id);
            foreach (var source in pssg.Elements<PssgRenderDataSource>().Where(s => originalIds.ContainsKey(s.Id)).ToArray())
            {
                var ids = originalIds[source.Id]; string newId = source.Id;
                source.ChildElements.OfType<PssgRenderIndexSource>().Single().Id = ids.Index;
                var streams = source.ChildElements.OfType<PssgRenderStream>().ToArray();
                if (streams.Length != ids.Streams.Length) throw new InvalidDataException("Stream count changed: " + newId);
                for (int k = 0; k < streams.Length; k++)
                {
                    string current = streams[k].DataBlock.TrimStart('#');
                    if (blocks.Remove(current, out var block)) { block.Id = ids.Streams[k].Block; blocks[block.Id] = block; }
                    streams[k].Id = ids.Streams[k].Stream; streams[k].DataBlock = "#" + ids.Streams[k].Block;
                }
                source.Id = ids.Source;
                foreach (var draw in pssg.Elements<PssgRenderStreamInstance>().Where(d => d.Indices == "#" + newId))
                {
                    draw.Indices = "#" + ids.Source;
                    foreach (var s in draw.ChildElements.OfType<PssgRenderInstanceSource>()) s.Source = "#" + ids.Source;
                }
            }
        }
        // Step F: the converter edits tracksplit through an XML round trip; test that alone.
        if (xmlRoundTrip) pssg = Files.FromDocument(Files.Document(pssg), pssg);
        string target = Path.Combine(venueOut, "tracksplit.pssg"); File.Delete(target); Files.Save(pssg, target);
        foreach (var draw in Files.Pssg(target).Elements<PssgRenderStreamInstance>()) { _ = draw.GetRenderDataSource(); _ = draw.GetShaderInstance(); }
        new XDocument(new XElement("config", new XAttribute("cycle_camera", "false"), new XAttribute("infinite_loop", "false"), new XAttribute("skipreplays", "true"),
            new XAttribute("outputfilename", "G2VRTrack"),
            new XElement("track", new XAttribute("country", "circuits"), new XAttribute("name", location), new XAttribute("route", RouteProbe.DonorRoute),
                new XAttribute("lighting", "day"), new XAttribute("weather", "dry"), new XAttribute("laps", 1), new XAttribute("number_cars", 1),
                new XElement("car", new XAttribute("name", "z28"), new XAttribute("livery", "user"), new XAttribute("ai_driver", "Cau_Demo_1_1"),
                    new XAttribute("ai_driver0", "Cau_Demo_1_0"), new XAttribute("grid", 0))))).Save(Path.Combine(output, "overlay", "G2VRTrack.xml"));
        new XDocument(new XElement("ModList", new XElement("Mod", new XAttribute("Path", Path.GetFullPath(Path.Combine(output, "overlay")))))).Save(Path.Combine(output, "modlist.xml"));
        Files.Json(Path.Combine(output, "build.json"), new { Schema = 1, Kind = "inplace-control", KeepStockIds = keepIds, RewrittenDraws = n, RemovedSources = oldSources.Count, RemovedBlocks = oldBlocks.Count, StockVisRetained = true, RuntimeValidated = false });
        Console.WriteLine($"In-place control built ({n} draws rewritten, stock tiles and track.vis kept): {output}");
    }
    sealed class InPlaceState : PssgModelWriterState { }

    // First texture input that carries colour, by stock input name.
    static string? DiffuseOf(PssgFile file, PssgShaderInstance shader)
    {
        var group = file.GetObject<PssgShaderGroup>(shader.ShaderGroup.AsMemory()[1..]);
        var names = group.ChildElements.Where(e => e.Name == "SHADERINPUTDEFINITION").Select(e => e.GetAttributeValue<string>("name")).ToArray();
        foreach (var wanted in new[] { "TDiffuseSpecMap1", "TDiffuseSpecMap2", "TDiffuseAlphaMap", "TDiffuseMap" })
        {
            int parameter = Array.IndexOf(names, wanted);
            if (parameter < 0) continue;
            var input = shader.ChildElements.FirstOrDefault(e => e.Name == "SHADERINPUT" && e.GetAttributeValue<uint>("parameterID") == parameter);
            string? reference = input?.GetAttributeValue<string>("texture");
            if (reference is not null && reference.StartsWith('#')) return reference[1..];
        }
        return null;
    }
}
