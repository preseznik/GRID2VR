using System.Numerics;
namespace GRID2VR.Tracks;

// Entry point for the launcher, which runs the converter in a child process of itself
// (GRID2VR.exe --convert-track), as DiRT2VR does with its converters.
public static class TrackConverterCli
{
    public static int Run(string[] args) => Program.Main(args);
}

internal static class Program
{
    internal static int Main(string[] args)
    {
        try
        {
            switch (args.FirstOrDefault())
            {
                case "inspect" when args.Length == 4:
                    Inspect.Run(args[1], args[2], args[3]); return 0;
                case "route-probe" when args.Length == 5:
                    RouteProbe.Run(args[1], args[2], args[3], args[4]); return 0;
                case "visual-probe" when args.Length == 5:
                    RouteProbe.Run(args[1], args[2], args[3], args[4], visuals: true); return 0;
                case "visual-probe" when args.Length == 6 && args[5] == "--reverse-tile-ids":
                    // Diagnostic control: identical geometry, tile identities in reverse order.
                    RouteProbe.Run(args[1], args[2], args[3], args[4], visuals: true, reverseTileIds: true); return 0;
                case "tree" when args.Length == 4:
                    Inspect.Tree(args[1], args[2], int.Parse(args[3], System.Globalization.CultureInfo.InvariantCulture)); return 0;
                case "layouts" when args.Length == 2:
                    Inspect.Layouts(args[1]); return 0;
                case "node-boxes" when args.Length == 2:
                    {
                        // TRANSFORM and BOUNDINGBOX of each terrain tile and its render nodes.
                        var doc = Files.Document(Files.Pssg(args[1]));
                        var surface = doc.Descendants().First(e => (string?)e.Attribute("id") == "surface");
                        foreach (var tile in surface.Elements().Take(int.MaxValue))
                            foreach (var node in new[] { tile }.Concat(tile.Elements().Where(e => e.Name.LocalName == "RENDERNODE")))
                            {
                                string Text(string name) => string.Join(" ", node.Elements(name).Select(e => e.Value.Trim()));
                                Console.WriteLine($"{(string?)node.Attribute("id"),-18} box [{Text("BOUNDINGBOX")}] transform [{Text("TRANSFORM")}]");
                            }
                        return 0;
                    }
                case "groups" when args.Length == 2:
                    {
                        // Shader groups with their sorting attributes and instance count.
                        var doc = Files.Document(Files.Pssg(args[1]));
                        var instances = doc.Descendants().Where(e => e.Name.LocalName == "SHADERINSTANCE").ToArray();
                        foreach (var group in doc.Descendants().Where(e => e.Name.LocalName == "SHADERGROUP"))
                        {
                            var id = (string?)group.Attribute("id");
                            var mine = instances.Where(i => (string?)i.Attribute("shaderGroup") == "#" + id).ToArray();
                            var priorities = string.Join(",", mine.Select(i => (string?)i.Attribute("renderSortPriority") ?? "-").Distinct().Take(5));
                            Console.WriteLine($"{id,-44} sorting={(string?)group.Attribute("instancesRequireSorting")} defaultPriority={(string?)group.Attribute("defaultRenderSortPriority")} instances={mine.Length} priorities={priorities}");
                        }
                        return 0;
                    }
                case "pass-states" when args.Length == 2:
                    {
                        // Per shader group: main pass depth/blend/coverage states and whether it samples TDiffuseAlphaMap.
                        var doc = Files.Document(Files.Pssg(args[1]));
                        foreach (var group in doc.Descendants().Where(e => e.Name.LocalName == "SHADERGROUP"))
                        {
                            var pass = group.Elements("SHADERGROUPPASS").FirstOrDefault();
                            if (pass is null) continue;
                            string A(string n) => (string?)pass.Attribute(n) ?? "-";
                            var streams = string.Join(",", group.Elements("SHADERSTREAMDEFINITION").Select(s => (string?)s.Attribute("renderTypeName")));
                            var alphaMap = group.Elements("SHADERINPUTDEFINITION").Any(d => (string?)d.Attribute("name") == "TDiffuseAlphaMap");
                            Console.WriteLine($"{(string?)group.Attribute("id"),-46} depth={A("depthMaskEnable")} blend={A("blendEnable")} a2c={A("alphaToCoverageEnable")} test={A("alphaTestEnable")} cull={A("cullFaceType")} alphaMap={alphaMap} streams={streams} passes={group.Elements("SHADERGROUPPASS").Count()}");
                        }
                        return 0;
                    }
                case "group-xml" when args.Length == 3:
                    {
                        // A shader group's full element tree (passes, render states), without data blobs.
                        var doc = Files.Document(Files.Pssg(args[1]));
                        var group = doc.Descendants().First(e => e.Name.LocalName == "SHADERGROUP" && (string?)e.Attribute("id") == args[2]);
                        void Print(System.Xml.Linq.XElement e, int depth)
                        {
                            if (e.Name.LocalName == "SHADERINPUTDEFINITION") return;
                            var text = e.HasElements ? "" : " = " + (e.Value.Length > 80 ? e.Value[..80] + "…" : e.Value);
                            Console.WriteLine(new string(' ', depth * 2) + e.Name.LocalName + " " + string.Join(" ", e.Attributes().Select(a => $"{a.Name}={a.Value}")) + text);
                            foreach (var child in e.Elements()) Print(child, depth + 1);
                        }
                        Print(group, 0);
                        return 0;
                    }
                case "check-node-boxes" when args.Length == 2:
                    {
                        // Render nodes with draws: stored BOUNDINGBOX vs the bounds of their geometry.
                        var (checkedNodes, mismatched, sample) = TrackVisuals.CheckNodeBoxes(Files.Pssg(args[1]));
                        Console.WriteLine($"{checkedNodes} render nodes with draws, {mismatched} whose BOUNDINGBOX differs from their geometry bounds (> 0.05 m){sample}");
                        return mismatched == 0 ? 0 : 3;
                    }
                case "tiles" when args.Length == 2:
                    foreach (var tile in Visibility.TerrainTiles(Files.Pssg(args[1])))
                    {
                        var b = Visibility.Bounds(tile);
                        Console.WriteLine($"{tile.Id,-12} x {b.Min.X,8:0} .. {b.Max.X,8:0}   z {b.Min.Z,8:0} .. {b.Max.Z,8:0}");
                    }
                    return 0;
                case "color-stats" when args.Length == 2:
                    {
                        // Vertex colour channel statistics per terrain shader group (stock tracksplit).
                        var pssg = Files.Pssg(args[1]);
                        var stats = new Dictionary<string, (long N, long[] Sum, int[] Min, int[] Max)>();
                        foreach (var tile in Visibility.TerrainTiles(pssg))
                        foreach (var draw in tile.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>())
                        {
                            string group = draw.GetShaderInstance().ShaderGroup;
                            var reader = new EgoEngineLibrary.Formats.Pssg.RenderDataSourceReader(draw.GetRenderDataSource());
                            if (!stats.TryGetValue(group, out var s)) s = (0, new long[4], [255, 255, 255, 255], [0, 0, 0, 0]);
                            for (uint v = 0; v < reader.VertexCount; v += 7)
                            {
                                Vector4 c;
                                try { c = reader.GetColor(v); } catch (Exception) { break; }
                                var ch = new[] { (int)MathF.Round(c.X * 255), (int)MathF.Round(c.Y * 255), (int)MathF.Round(c.Z * 255), (int)MathF.Round(c.W * 255) };
                                for (int k = 0; k < 4; k++) { s.Sum[k] += ch[k]; s.Min[k] = Math.Min(s.Min[k], ch[k]); s.Max[k] = Math.Max(s.Max[k], ch[k]); }
                                s.N++;
                            }
                            stats[group] = s;
                        }
                        Console.WriteLine("GetColor() channels X,Y,Z,W: mean [min..max]");
                        foreach (var (group, s) in stats.OrderBy(x => x.Key))
                            Console.WriteLine($"{group,-36} n={s.N,7} " + string.Join("  ", Enumerable.Range(0, 4).Select(k => $"{(s.N > 0 ? s.Sum[k] / s.N : 0),3} [{s.Min[k]}..{s.Max[k]}]")));
                        return 0;
                    }
                case "vertices" when args.Length == 4:
                    {
                        // Raw vertex streams of a render node's first draw (big-endian PSSG data).
                        var pssg = Files.Pssg(args[1]);
                        var node = pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderNode>().First(n => n.Id == args[2]);
                        var draw = node.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>().First();
                        var doc = Files.Document(pssg);
                        string rds = draw.Indices.TrimStart('#');
                        var source = doc.Descendants().First(e => e.Name.LocalName == "RENDERDATASOURCE" && (string?)e.Attribute("id") == rds);
                        string blockId = ((string)source.Descendants().First(e => e.Name.LocalName == "RENDERSTREAM").Attribute("dataBlock")!).TrimStart('#');
                        var block = doc.Descendants().First(e => e.Name.LocalName == "DATABLOCK" && (string?)e.Attribute("id") == blockId);
                        var data = Convert.FromHexString(string.Concat(block.Element("DATABLOCKDATA")!.Value.Where(char.IsAsciiHexDigit)));
                        var streams = block.Elements("DATABLOCKSTREAM").Select(s => (Type: (string)s.Attribute("renderType")!, Data: (string)s.Attribute("dataType")!, Offset: (int)s.Attribute("offset")!, Stride: (int)s.Attribute("stride")!)).ToArray();
                        Console.WriteLine($"{rds} block {blockId}: {data.Length} bytes, shader {draw.Shader}");
                        for (int v = 0; v < int.Parse(args[3]); v++)
                        {
                            var parts = new List<string>();
                            foreach (var s in streams)
                            {
                                int p = v * s.Stride + s.Offset;
                                float H(int at) => (float)BitConverter.UInt16BitsToHalf(System.Buffers.Binary.BinaryPrimitives.ReadUInt16BigEndian(data.AsSpan(at, 2)));
                                float F(int at) => System.Buffers.Binary.BinaryPrimitives.ReadSingleBigEndian(data.AsSpan(at, 4));
                                parts.Add(s.Type + ":" + s.Data switch
                                {
                                    "float3" => $"{F(p):0.##},{F(p + 4):0.##},{F(p + 8):0.##}",
                                    "half2" => $"{H(p):0.###},{H(p + 2):0.###}",
                                    "half4" => $"{H(p):0.###},{H(p + 2):0.###},{H(p + 4):0.###},{H(p + 6):0.###}",
                                    "uint_color_argb" => Convert.ToHexString(data.AsSpan(p, 4)),
                                    _ => "?"
                                });
                            }
                            Console.WriteLine("  " + string.Join("  ", parts));
                        }
                        return 0;
                    }
                case "rewrite-check" when args.Length == 3:
                    {
                        // Write a stock draw back through RenderDataSourceWriter (original vertex order)
                        // and compare vertex and index bytes with the stock data.
                        var pssg = Files.Pssg(args[1]);
                        var node = pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderNode>().First(n => n.Id == args[2]);
                        var draw = node.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>().First();
                        var stockRds = draw.GetRenderDataSource();
                        var group = draw.GetShaderInstance().ShaderGroup.TrimStart('#');
                        var layout = EgoEngineLibrary.Formats.Pssg.ShaderInputInfo.CreateFromPssg(pssg).First(l => l.ShaderGroupId == group);
                        var reader = new EgoEngineLibrary.Formats.Pssg.RenderDataSourceReader(stockRds);
                        var writer = new EgoEngineLibrary.Formats.Pssg.RenderDataSourceWriter("check");
                        for (uint v = 0; v < reader.VertexCount; v++)
                        {
                            writer.Positions.Add(reader.GetPosition(v));
                            if (reader.HasNormals) writer.Normals.Add(reader.GetNormal(v));
                            writer.Colors.Add(reader.GetColor(v));
                            writer.Tangents.Add(Vector4.UnitX);
                            Vector2 T(int s) => s < reader.TexCoordSetCount ? reader.GetTexCoord(v, s) : Vector2.Zero;
                            writer.TexCoords0.Add(T(0)); writer.TexCoords1.Add(T(1)); writer.TexCoords2.Add(T(2)); writer.TexCoords3.Add(T(3));
                        }
                        for (int i = 0; i < reader.IndexCount; i++) writer.Indices.Add(reader.GetIndex(i));
                        // Write into the loaded stock file in memory (never saved), as TrackVisuals does.
                        var segmentLib = pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgLibrary>().Single(l => l.Type == "SEGMENTSET");
                        var dataLib = pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgLibrary>().Single(l => l.Type == "RENDERINTERFACEBOUND");
                        var set = new EgoEngineLibrary.Graphics.Pssg.Elements.PssgSegmentSet(pssg, segmentLib) { Id = "g2vr_check_set", SegmentCount = 1 }; segmentLib.AppendChild(set);
                        writer.Write(layout, set, dataLib, new CheckState());
                        var stockDoc = Files.Document(pssg);
                        var ours = stockDoc.Descendants("SEGMENTSET").Single(e => (string?)e.Attribute("id") == "g2vr_check_set");
                        var oursSourceStream = ((string)ours.Descendants("RENDERSTREAM").First().Attribute("dataBlock")!).TrimStart('#');
                        var oursBlocks = stockDoc.Descendants("DATABLOCK").Where(e => (string?)e.Attribute("id") == oursSourceStream).ToArray();
                        var stockSource = stockDoc.Descendants("RENDERDATASOURCE").Single(e => (string?)e.Attribute("id") == stockRds.Id);
                        string Hex(System.Xml.Linq.XElement? e) => string.Concat((e?.Value ?? "").Where(char.IsAsciiHexDigit));
                        string stockIndex = Hex(stockSource.Descendants("INDEXSOURCEDATA").Single()), ourIndex = Hex(ours.Descendants("INDEXSOURCEDATA").Single());
                        Console.WriteLine($"{group}: vertices {reader.VertexCount}, indices {reader.IndexCount}; index bytes equal: {stockIndex == ourIndex} ({stockIndex.Length / 2} vs {ourIndex.Length / 2})");
                        Console.WriteLine("stock index attrs: " + string.Join(" ", stockSource.Element("RENDERINDEXSOURCE")!.Attributes().Select(a => $"{a.Name}={a.Value}")));
                        Console.WriteLine("ours  index attrs: " + string.Join(" ", ours.Descendants("RENDERINDEXSOURCE").Single().Attributes().Select(a => $"{a.Name}={a.Value}")));
                        var stockBlockId = ((string)stockSource.Elements("RENDERSTREAM").First().Attribute("dataBlock")!).TrimStart('#');
                        var stockBlock = stockDoc.Descendants("DATABLOCK").First(e => (string?)e.Attribute("id") == stockBlockId);
                        Console.WriteLine("stock block: " + string.Join(" ", stockBlock.Attributes().Select(a => $"{a.Name}={a.Value}")) + " | " + string.Join(";", stockBlock.Elements("DATABLOCKSTREAM").Select(s => $"{s.Attribute("renderType")!.Value}:{s.Attribute("dataType")!.Value}@{s.Attribute("offset")!.Value}/{s.Attribute("stride")!.Value}")));
                        foreach (var block in oursBlocks)
                            Console.WriteLine("ours  block: " + string.Join(" ", block.Attributes().Select(a => $"{a.Name}={a.Value}")) + " | " + string.Join(";", block.Elements("DATABLOCKSTREAM").Select(s => $"{s.Attribute("renderType")!.Value}:{s.Attribute("dataType")!.Value}@{s.Attribute("offset")!.Value}/{s.Attribute("stride")!.Value}")));
                        string a = Hex(stockBlock.Element("DATABLOCKDATA")), b = Hex(oursBlocks[0].Element("DATABLOCKDATA"));
                        int diff = Enumerable.Range(0, Math.Min(a.Length, b.Length) / 2).Count(k => a.Substring(k * 2, 2) != b.Substring(k * 2, 2));
                        int first = Enumerable.Range(0, Math.Min(a.Length, b.Length) / 2).FirstOrDefault(k => a.Substring(k * 2, 2) != b.Substring(k * 2, 2), -1);
                        Console.WriteLine($"vertex bytes: stock {a.Length / 2}, ours {b.Length / 2}, differing bytes {diff}, first at {first}");
                        if (first >= 0) Console.WriteLine($"  stock {a.Substring(Math.Max(0, first * 2 - 8), 48)}\n  ours  {b.Substring(Math.Max(0, first * 2 - 8), 48)}");
                        return 0;
                    }
                case "textures" when args.Length == 3:
                    {
                        // Texture elements whose id starts with a prefix, attributes and child structure.
                        var doc = Files.Document(Files.Pssg(args[1]));
                        foreach (var t in doc.Descendants().Where(e => e.Name.LocalName == "TEXTURE" && ((string?)e.Attribute("id") ?? "").StartsWith(args[2], StringComparison.Ordinal)).Take(3))
                        {
                            Console.WriteLine(string.Join(" ", t.Attributes().Select(a => $"{a.Name}={a.Value}")));
                            foreach (var c in t.Descendants()) Console.WriteLine($"   {c.Name.LocalName} " + string.Join(" ", c.Attributes().Select(a => $"{a.Name}={a.Value}")) + (c.HasElements ? "" : $" [{c.Value.Length} chars]"));
                        }
                        return 0;
                    }
                case "disasm" when args.Length == 3:
                    if (File.Exists(args[2])) throw new IOException("Output must be new: " + args[2]);
                    File.WriteAllText(args[2], Disassemble.Run(File.ReadAllBytes(args[1]))); return 0;
                case "shader-code" when args.Length == 4:
                    {
                        // Outline a shaderpack group's passes and dump each referenced program's code
                        // blocks (DXBC) for disassembly.
                        var doc = Files.Document(Files.Pssg(args[1]));
                        var group = doc.Descendants().First(e => e.Name.LocalName == "SHADERGROUP" && (string?)e.Attribute("id") == args[2]);
                        Console.WriteLine(string.Join(" ", group.Attributes().Select(a => $"{a.Name}={a.Value}")));
                        foreach (var e in group.Elements().Where(e => e.Name.LocalName != "SHADERINPUTDEFINITION"))
                            Console.WriteLine($"  {e.Name.LocalName} " + string.Join(" ", e.Attributes().Select(a => $"{a.Name}={a.Value}")) +
                                string.Concat(e.Elements().Select(c => $"\n    {c.Name.LocalName} " + string.Join(" ", c.Attributes().Select(a => $"{a.Name}={a.Value}")))));
                        Directory.CreateDirectory(args[3]);
                        var programs = group.Descendants().SelectMany(e => e.Attributes()).Where(a => a.Value.StartsWith('#')).Select(a => a.Value[1..]).ToHashSet();
                        foreach (var program in doc.Descendants().Where(e => e.Name.LocalName == "SHADERPROGRAM" && programs.Contains((string?)e.Attribute("id") ?? "")))
                        {
                            int k = 0;
                            foreach (var code in program.Descendants().Where(e => e.Name.LocalName == "SHADERPROGRAMCODE"))
                            {
                                var block = code.Descendants().FirstOrDefault(e => e.Name.LocalName == "SHADERPROGRAMCODEBLOCK");
                                if (block is null) continue;
                                var bytes = Convert.FromHexString(string.Concat(block.Value.Where(char.IsAsciiHexDigit)));
                                string name = $"{(string)program.Attribute("id")!}_{k++}".Replace('#', '_').Replace('!', '_') + ".dxbc";
                                File.WriteAllBytes(Path.Combine(args[3], name), bytes);
                                Console.WriteLine($"  program {(string?)program.Attribute("id")}: {string.Join(" ", code.Attributes().Select(a => $"{a.Name}={a.Value}"))} -> {name} ({bytes.Length} bytes, {System.Text.Encoding.ASCII.GetString(bytes, 0, Math.Min(4, bytes.Length))})");
                            }
                        }
                        return 0;
                    }
                case "ac-materials" when args.Length == 4:
                    {
                        // Assetto Corsa materials by visible triangle count: shader, texture slots, key properties.
                        var source = AcSource.Read(args[1], args[2], (min, max) => min, visuals: true);
                        var rows = source.Visuals.GroupBy(m => m.Material.Id).Select(g => (Material: g.First().Material, Triangles: g.Sum(m => m.Indices.Length / 3)))
                            .OrderByDescending(r => r.Triangles).Take(int.Parse(args[3])).ToArray();
                        foreach (var (m, n) in rows)
                            Console.WriteLine($"{n,9:N0} {m.Shader,-28} blend={m.Blend} at={m.AlphaTest} {m.Name}\n            " +
                                string.Join(" ", m.Textures.Select(t => $"{t.Key}={t.Value}")) + "\n            " +
                                string.Join(" ", m.Properties.Where(p => p.Key is not ("ksAmbient" or "ksDiffuse" or "ksSpecular" or "ksSpecularEXP" or "ksEmissive" or "ksAlphaRef"))
                                    .Select(p => $"{p.Key}={string.Join(",", p.Value.Take(p.Key.StartsWith("mult") || p.Key.Contains("Mult") ? 1 : 4).Select(v => v.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture)))}")));
                        return 0;
                    }
                case "ac-cutouts" when args.Length == 3:
                    {
                        // Alpha-tested source materials: triangles by mesh LOD range, and triangles whose
                        // exact reversed copy (same positions, opposite winding) also exists.
                        var source = AcSource.Read(args[1], args[2], (min, max) => min, visuals: true);
                        foreach (var g in source.Visuals.Where(m => m.Material.StockGroup is null && TrackVisuals.IsAlphaTested(m.Material)).GroupBy(m => m.Material.Name).OrderBy(g => g.Key))
                        {
                            var faces = new HashSet<(System.Numerics.Vector3, System.Numerics.Vector3, System.Numerics.Vector3)>(); int total = 0, reversed = 0, same = 0;
                            (System.Numerics.Vector3, System.Numerics.Vector3, System.Numerics.Vector3) Key(System.Numerics.Vector3 a, System.Numerics.Vector3 b, System.Numerics.Vector3 c)
                            { // rotation-invariant key for one winding
                                var r = new[] { (a, b, c), (b, c, a), (c, a, b) };
                                return r.OrderBy(t => t.Item1.X).ThenBy(t => t.Item1.Y).ThenBy(t => t.Item1.Z).First();
                            }
                            foreach (var m in g) for (int i = 0; i + 2 < m.Indices.Length; i += 3)
                            {
                                var (a, b, c) = (m.Positions[m.Indices[i]], m.Positions[m.Indices[i + 1]], m.Positions[m.Indices[i + 2]]); total++;
                                if (faces.Contains(Key(a, c, b))) reversed++;
                                if (!faces.Add(Key(a, b, c))) same++;
                            }
                            var lods = string.Join(" ", g.GroupBy(m => (m.LodIn, m.LodOut)).Select(l => $"[{l.Key.LodIn:0}-{l.Key.LodOut:0}]x{l.Count()}:{l.Sum(m => m.Indices.Length / 3)}"));
                            Console.WriteLine($"{g.Key,-22} {g.First().Material.Shader,-26} tris={total,8:N0} reversedDup={reversed,7:N0} sameDup={same,6:N0} lods {lods}");
                        }
                        return 0;
                    }
                case "roundtrip-diff" when args.Length == 2:
                    {
                        // Does Files.Document -> Files.FromDocument preserve every element, attribute
                        // value and attribute value type? Reports the first differences.
                        var a = Files.Pssg(args[1]); var b = Files.FromDocument(Files.Document(a));
                        int shown = 0, elements = 0, attributes = 0;
                        var diffs = new Dictionary<string, int>();
                        void Walk(EgoEngineLibrary.Graphics.Pssg.PssgElement x, EgoEngineLibrary.Graphics.Pssg.PssgElement y, string path)
                        {
                            elements++;
                            if (x.Name != y.Name) { diffs[$"element {x.Name}->{y.Name}"] = diffs.GetValueOrDefault($"element {x.Name}->{y.Name}") + 1; return; }
                            foreach (var ax in x.Attributes)
                            {
                                attributes++;
                                var ay = y.Attributes.FirstOrDefault(t => t.Name == ax.Name);
                                string? key = ay is null ? $"{x.Name}@{ax.Name} missing" :
                                    ax.Value?.GetType() != ay.Value?.GetType() ? $"{x.Name}@{ax.Name} type {ax.Value?.GetType().Name}->{ay.Value?.GetType().Name}" :
                                    ax.DisplayValue != ay.DisplayValue ? $"{x.Name}@{ax.Name} value" : null;
                                if (key is null) continue;
                                diffs[key] = diffs.GetValueOrDefault(key) + 1;
                                if (shown++ < 12) Console.WriteLine($"{path}/{x.Name} {ax.Name}: {ax.Value?.GetType().Name} '{ax.DisplayValue}' -> {ay?.Value?.GetType().Name} '{ay?.DisplayValue}'");
                            }
                            if (y.Attributes.Count() != x.Attributes.Count()) diffs[$"{x.Name} attribute count"] = diffs.GetValueOrDefault($"{x.Name} attribute count") + 1;
                            var cx = x.ChildElements.ToArray(); var cy = y.ChildElements.ToArray();
                            if (cx.Length != cy.Length) { diffs[$"{x.Name} child count"] = diffs.GetValueOrDefault($"{x.Name} child count") + 1; return; }
                            if (cx.Length == 0 && !(x.Value is byte[] bx && y.Value is byte[] by ? bx.AsSpan().SequenceEqual(by) : Equals(x.Value, y.Value) || (x.Value?.ToString() == y.Value?.ToString() && x.Value?.GetType() == y.Value?.GetType())))
                                diffs[$"{x.Name} data"] = diffs.GetValueOrDefault($"{x.Name} data") + 1;
                            for (int i = 0; i < cx.Length; i++) Walk(cx[i], cy[i], path + "/" + x.Name);
                        }
                        Walk(a.RootElement!, b.RootElement!, "");
                        Console.WriteLine($"{elements} elements, {attributes} attributes compared");
                        foreach (var (k, v) in diffs.OrderByDescending(d => d.Value)) Console.WriteLine($"  {v,6} x {k}");
                        return 0;
                    }
                case "set-xml" when args.Length == 3:
                    {
                        var doc = Files.Document(Files.Pssg(args[1]));
                        var lib = doc.Descendants("LIBRARY").Single(l => (string?)l.Attribute("type") == "SEGMENTSET");
                        Console.WriteLine("library attrs: " + string.Join(" ", lib.Attributes().Select(a => $"{a.Name}={a.Value}")) + $" children={lib.Elements().Count()}");
                        foreach (var set in lib.Elements().Where(s => ((string?)s.Attribute("id") ?? "").StartsWith(args[2], StringComparison.Ordinal)).Take(2))
                        {
                            Console.WriteLine("SET " + string.Join(" ", set.Attributes().Select(a => $"{a.Name}={a.Value}")));
                            foreach (var s in set.Elements().Take(2)) Console.WriteLine("   " + s.Name.LocalName + " " + string.Join(" ", s.Attributes().Select(a => $"{a.Name}={a.Value}")) + " children: " + string.Join(",", s.Elements().Select(c => c.Name.LocalName)));
                        }
                        return 0;
                    }
                case "node-shaders" when args.Length == 2:
                    {
                        var pssg = Files.Pssg(args[1]);
                        foreach (var g in pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderNode>().SelectMany(n => n.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>().Select(d => (Kind: n.Id[..n.Id.IndexOf('_')], d.Shader))).GroupBy(x => x.Kind))
                            Console.WriteLine($"{g.Key,-12} " + string.Join(" ", g.GroupBy(x => x.Shader).OrderByDescending(s => s.Count()).Take(6).Select(s => $"{s.Key}x{s.Count()}")));
                        foreach (var s in pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgShaderInstance>().Where(s => s.ShaderGroup == "#batched_track.fx"))
                            Console.WriteLine($"  batch instance {s.Id}: " + string.Join(" ", s.ChildElements.Select(c => c.Name + ":" + string.Join(",", c.Attributes.Select(a => a.Name + "=" + a.Value)))));
                        return 0;
                    }
                case "batch-check" when args.Length == 3:
                    {
                        // Is <LEVEL>BATCH the concatenation of <LEVEL> geometry, and in which order?
                        var pssg = Files.Pssg(args[1]);
                        EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderNode N(string id) => pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderNode>().Single(n => n.Id == id);
                        var colour = N(args[2]); var batch = N(args[2].Insert(args[2].IndexOf('_'), "BATCH"));
                        var b = new EgoEngineLibrary.Formats.Pssg.RenderDataSourceReader(batch.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>().Single().GetRenderDataSource());
                        var draws = colour.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>().Select(d => new EgoEngineLibrary.Formats.Pssg.RenderDataSourceReader(d.GetRenderDataSource())).ToArray();
                        Console.WriteLine($"batch: {b.VertexCount} verts {b.IndexCount} idx; colour draws: {draws.Length}, {draws.Sum(d => (long)d.VertexCount)} verts {draws.Sum(d => (long)d.IndexCount)} idx");
                        // Where does each draw's first triangle appear in the batch (triangle index)?
                        var batchTris = Enumerable.Range(0, (int)b.IndexCount / 3).Select(i => (b.GetPosition(b.GetIndex(i * 3)), b.GetPosition(b.GetIndex(i * 3 + 1)), b.GetPosition(b.GetIndex(i * 3 + 2)))).ToList();
                        var first = new Dictionary<(Vector3, Vector3, Vector3), int>(); for (int i = 0; i < batchTris.Count; i++) first.TryAdd(batchTris[i], i);
                        Console.WriteLine("draw -> first triangle position in batch: " + string.Join(" ", draws.Select((d, k) => $"{k}:{(d.IndexCount >= 3 && first.TryGetValue((d.GetPosition(d.GetIndex(0)), d.GetPosition(d.GetIndex(1)), d.GetPosition(d.GetIndex(2))), out var at) ? at : -1)}")));
                        return 0;
                    }
                case "set-content-order" when args.Length == 2:
                    {
                        var pssg = Files.Pssg(args[1]);
                        var drawOf = pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>().GroupBy(d => d.Indices.TrimStart('#')).ToDictionary(g => g.Key, g => g.First());
                        foreach (var set in pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgSegmentSet>().Where(s => s.Segments.Count() >= 6).Take(6))
                        {
                            var node = (EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderNode)drawOf[set.Segments.First().Id].ParentElement!;
                            var drawIndex = node.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>().Select((d, i) => (d.Indices.TrimStart('#'), i)).ToDictionary(x => x.Item1, x => x.i);
                            Console.WriteLine($"{set.Id} ({node.Id}): " + string.Join(" ", set.Segments.Select(r => $"{drawIndex.GetValueOrDefault(r.Id, -1)}:{drawOf[r.Id].GetShaderInstance().ShaderGroup.TrimStart('#').Replace(".fx", "").Replace("terrain_", "")}/{r.StreamCount}/{new EgoEngineLibrary.Formats.Pssg.RenderDataSourceReader(r).VertexCount}")));
                        }
                        return 0;
                    }
                case "set-order" when args.Length == 2:
                    {
                        var pssg = Files.Pssg(args[1]);
                        var sets = pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgSegmentSet>().ToArray();
                        var setOf = sets.SelectMany((s, i) => s.Segments.Select((r, j) => (r.Id, Set: i, Pos: j))).ToDictionary(x => x.Id, x => (x.Set, x.Pos));
                        int same = 0, diff = 0; var nodeOrder = new List<int>();
                        foreach (var node in pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderNode>())
                        {
                            var draws = node.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>().Select(d => setOf[d.Indices.TrimStart('#')]).ToArray();
                            if (draws.Length == 0) continue; nodeOrder.Add(draws[0].Set);
                            if (draws.Select(d => d.Pos).SequenceEqual(Enumerable.Range(0, draws.Length))) same++; else diff++;
                        }
                        Console.WriteLine($"nodes whose draws follow set order 0..n: {same}, other: {diff}");
                        Console.WriteLine("set index of nodes in document order: " + string.Join(",", nodeOrder.Take(40)));
                        return 0;
                    }
                case "set-sizes" when args.Length == 2:
                    {
                        var pssg = Files.Pssg(args[1]);
                        var setOf = pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgSegmentSet>().SelectMany(s => s.Segments.Select(r => (r.Id, Set: s))).ToDictionary(x => x.Id, x => x.Set);
                        long Verts(EgoEngineLibrary.Graphics.Pssg.Elements.PssgSegmentSet s) => s.Segments.Sum(r => (long)new EgoEngineLibrary.Formats.Pssg.RenderDataSourceReader(r).VertexCount);
                        foreach (var tile in Visibility.TerrainTiles(pssg))
                            foreach (var node in tile.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderNode>().Where(n => !n.Id.Contains("BATCH")))
                            {
                                var sets = node.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>().Select(d => setOf[d.Indices.TrimStart('#')]).Distinct().ToArray();
                                if (sets.Length > 0) Console.WriteLine($"{node.Id,-16} sets={sets.Length} " + string.Join(" ", sets.Select(s => $"[{s.Segments.Count()} src, {Verts(s)} verts]")));
                            }
                        return 0;
                    }
                case "set-rules" when args.Length == 2:
                    {
                        // How stock SEGMENTSETs group data sources: by node, tile, shader group, layout.
                        var pssg = Files.Pssg(args[1]);
                        var users = new Dictionary<string, List<(string Node, string Tile, string Group)>>();
                        foreach (var tile in Visibility.TerrainTiles(pssg))
                            foreach (var node in tile.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderNode>())
                                foreach (var d in node.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>())
                                {
                                    var key = d.Indices.TrimStart('#');
                                    if (!users.TryGetValue(key, out var list)) users[key] = list = [];
                                    list.Add((node.Id[..node.Id.IndexOf('_')], tile.Id, d.GetShaderInstance().ShaderGroup.TrimStart('#')));
                                }
                        foreach (var set in pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgSegmentSet>())
                        {
                            var u = set.Segments.SelectMany(s => users.GetValueOrDefault(s.Id, [("UNUSED", "-", "-")])).ToArray();
                            var blocks = set.Segments.SelectMany(s => s.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStream>().Select(r => r.DataBlock)).Distinct().Count();
                            Console.WriteLine($"{set.Id,-6} n={set.Segments.Count(),3} blocks={blocks,3} nodes={string.Join("/", u.Select(x => x.Node).Distinct())} tiles={u.Select(x => x.Tile).Distinct().Count()} groups={string.Join("/", u.Select(x => x.Group).Distinct().Take(4))}");
                        }
                        return 0;
                    }
                case "id-refs" when args.Length == 3:
                    {
                        // Are tracksplit SEGMENTSET / tile ids referenced from other venue files?
                        var pssg = Files.Pssg(args[1]);
                        var ids = pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgSegmentSet>().Select(s => s.Id).Where(i => i.Length >= 3).ToHashSet();
                        Console.WriteLine($"{ids.Count} segment set ids of length >= 3, e.g. {string.Join(" ", ids.Take(5))}");
                        foreach (var file in Directory.EnumerateFiles(args[2], "*", SearchOption.AllDirectories).Where(f => !f.EndsWith("tracksplit.pssg", StringComparison.OrdinalIgnoreCase)))
                        {
                            var text = System.Text.Encoding.Latin1.GetString(File.ReadAllBytes(file));
                            var hits = ids.Where(i => text.Contains(i, StringComparison.Ordinal)).ToArray();
                            if (hits.Length > ids.Count / 10) Console.WriteLine($"  {Path.GetRelativePath(args[2], file)}: {hits.Length} of {ids.Count} ids present, e.g. {string.Join(" ", hits.Take(5))}");
                        }
                        return 0;
                    }
                case "segments" when args.Length == 2:
                    {
                        // SEGMENTSET grouping vs. tiles and render nodes.
                        var pssg = Files.Pssg(args[1]);
                        var setOf = new Dictionary<string, string>();
                        foreach (var set in pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgSegmentSet>())
                            foreach (var rds in set.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderDataSource>()) setOf[rds.Id] = set.Id;
                        var sets = pssg.Elements<EgoEngineLibrary.Graphics.Pssg.Elements.PssgSegmentSet>().ToArray();
                        Console.WriteLine($"{sets.Length} segment sets, {setOf.Count} data sources inside them; sizes: " + string.Join(",", sets.Take(12).Select(s => s.ChildElements.Count)));
                        foreach (var tile in Visibility.TerrainTiles(pssg).Take(4))
                            foreach (var node in tile.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderNode>())
                                Console.WriteLine($"{tile.Id}/{node.Id}: " + string.Join(" ", node.ChildElements.OfType<EgoEngineLibrary.Graphics.Pssg.Elements.PssgRenderStreamInstance>()
                                    .Select(d => setOf.GetValueOrDefault(d.Indices.TrimStart('#'), "none")).GroupBy(s => s).Select(g => $"{g.Key}x{g.Count()}")));
                        return 0;
                    }
                case "shader-xml" when args.Length is 3 or 4:
                    {
                        // First instance of a shader group, with input names.
                        var doc = Files.Document(Files.Pssg(args[1]));
                        var group = doc.Descendants().First(e => e.Name.LocalName == "SHADERGROUP" && (string?)e.Attribute("id") == args[2]);
                        var names = group.Elements("SHADERINPUTDEFINITION").Select(d => (string?)d.Attribute("name") ?? "").ToArray();
                        var instances = doc.Descendants().Where(e => e.Name.LocalName == "SHADERINSTANCE" && (string?)e.Attribute("shaderGroup") == "#" + args[2]).ToArray();
                        Console.WriteLine($"{instances.Length} instances of {args[2]}");
                        var instance = args.Length > 3 ? instances.FirstOrDefault(i => (string?)i.Attribute("id") == args[3]) : instances.FirstOrDefault();
                        if (instance is null) return 1;
                        Console.WriteLine(string.Join(" ", instance.Attributes().Select(a => $"{a.Name}={a.Value}")));
                        foreach (var input in instance.Elements("SHADERINPUT"))
                        {
                            int id = (int?)input.Attribute("parameterID") ?? -1;
                            Console.WriteLine($"  {names.ElementAtOrDefault(id),-22} {(string?)input.Attribute("type")} {(string?)input.Attribute("texture")}{input.Value.Trim()}");
                        }
                        return 0;
                    }
                case "draw-xml" when args.Length == 3:
                    {
                        // One render node's first draw, its data source and shader instance, without bulk data.
                        var doc = Files.Document(Files.Pssg(args[1]));
                        var node = doc.Descendants().First(e => (string?)e.Attribute("id") == args[2]);
                        var draw = node.Elements().First(e => e.Name.LocalName == "RENDERSTREAMINSTANCE");
                        string Strip(System.Xml.Linq.XElement e)
                        {
                            var copy = new System.Xml.Linq.XElement(e);
                            foreach (var d in copy.DescendantsAndSelf().Where(x => !x.HasElements && x.Value.Length > 80)) d.Value = $"[{d.Value.Length} chars]";
                            return copy.ToString();
                        }
                        string Ref(string? r) => r is null ? "" : r.TrimStart('#');
                        Console.WriteLine("RENDERNODE attrs: " + string.Join(" ", node.Attributes().Select(a => $"{a.Name}={a.Value}")) + $" children: {string.Join(",", node.Elements().Select(e => e.Name.LocalName).Distinct())}");
                        Console.WriteLine(Strip(draw));
                        var source = doc.Descendants().First(e => (string?)e.Attribute("id") == Ref((string?)draw.Attribute("indices")) && e.Name.LocalName == "RENDERDATASOURCE");
                        Console.WriteLine(Strip(source));
                        foreach (var stream in source.Descendants().Where(e => e.Name.LocalName == "RENDERSTREAM"))
                        {
                            var block = doc.Descendants().FirstOrDefault(e => (string?)e.Attribute("id") == Ref((string?)stream.Attribute("dataBlock")));
                            if (block is not null) Console.WriteLine(Strip(block).Split('\n').Take(14).Aggregate((a, b) => a + "\n" + b));
                        }
                        var shader = doc.Descendants().First(e => e.Name.LocalName == "SHADERINSTANCE" && (string?)e.Attribute("id") == Ref((string?)draw.Attribute("shader")));
                        Console.WriteLine(Strip(shader).Split('\n').Take(6).Aggregate((a, b) => a + "\n" + b));
                        return 0;
                    }
                case "inplace-control" when args.Length == 4 && args[3] == "new-instance-ids":
                    ReExport.InPlace(args[1], args[2], newInstanceIds: true); return 0;
                case "inplace-control" when args.Length == 4 && args[3] == "xml-roundtrip":
                    ReExport.InPlace(args[1], args[2], xmlRoundTrip: true); return 0;
                case "inplace-control" when args.Length == 4 && args[3] == "own-batches":
                    ReExport.InPlace(args[1], args[2], ownBatches: true); return 0;
                case "inplace-control" when args.Length == 4 && args[3] == "empty-low":
                    ReExport.InPlace(args[1], args[2], emptyLow: true); return 0;
                case "inplace-control" when args.Length == 4 && args[3] == "rename-sets":
                    ReExport.InPlace(args[1], args[2], renameSets: true); return 0;
                case "inplace-control" when args.Length == 4 && args[3] == "regroup-sets":
                    ReExport.InPlace(args[1], args[2], regroupSets: true); return 0;
                case "inplace-control" when args.Length == 4 && args[3] == "shift-names":
                    ReExport.InPlace(args[1], args[2], shiftNames: true); return 0;
                case "inplace-control" when args.Length == 4 && args[3] == "converted-lod":
                    ReExport.InPlace(args[1], args[2], convertedLod: true); return 0;
                case "inplace-control" when args.Length == 4 && args[3] == "keep-ids":
                    ReExport.InPlace(args[1], args[2], keepIds: true); return 0;
                case "inplace-control" when args.Length == 3:
                    ReExport.InPlace(args[1], args[2]); return 0;
                case "reexport-control" when args.Length == 3:
                    ReExport.Run(args[1], args[2]); return 0;
                case "reexport-control" when args.Length == 4 && args[3] == "stock-shaders-nomerge":
                    ReExport.Run(args[1], args[2], stockShaders: true, mergeDraws: false); return 0;
                case "reexport-control" when args.Length == 4 && args[3] == "stock-shaders-36":
                    ReExport.Run(args[1], args[2], stockShaders: true, fixedTiles: 36); return 0;
                case "reexport-control" when args.Length == 4 && args[3] == "stock-shaders-keep-land":
                    ReExport.Run(args[1], args[2], stockShaders: true, stockLod: true, keepLand: true); return 0;
                case "reexport-control" when args.Length == 4 && args[3] == "stock-shaders-stock-lod":
                    ReExport.Run(args[1], args[2], stockShaders: true, stockLod: true); return 0;
                case "reexport-control" when args.Length == 4 && args[3] == "weight-test":
                    ReExport.Run(args[1], args[2], weightTest: true); return 0;
                case "reexport-control" when args.Length == 4 && args[3] == "stock-shaders":
                    ReExport.Run(args[1], args[2], stockShaders: true); return 0;
                case "vis-control" when args.Length == 3:
                    Visibility.Control(args[1], args[2]); return 0;
                case "check-vis" when args.Length == 3:
                    Console.WriteLine(Visibility.CheckDonor(args[1], args[2])); return 0;
                case "decode" when args.Length == 3:
                    Inspect.Decode(args[1], args[2]); return 0;
                default:
                    Console.Error.WriteLine("Usage: inspect <GRID 2 root> <category/location/route_N> <new output folder>\n       decode <EGO xml/pssg file> <new text file>");
                    return 2;
            }
        }
        catch (Exception error) when (error is IOException or InvalidDataException or ArgumentException or UnauthorizedAccessException)
        {
            Console.Error.WriteLine(error.Message); return 1;
        }
    }
}

internal sealed class CheckState : EgoEngineLibrary.Formats.Pssg.PssgModelWriterState { }

