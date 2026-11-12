// GRID 2 track visuals. Pattern adapted from DiRT2VR tools/nordschleife-converter/
// Visuals.cs (a617f7a), same author; targets GRID 2's venue tracksplit.pssg and
// GRID 2 object shaders instead of DiRT 2's routesplit/object_simple.
using System.Numerics;
using System.Xml.Linq;
using EgoEngineLibrary.Formats.Pssg;
using EgoEngineLibrary.Graphics;
using EgoEngineLibrary.Graphics.Dds;
using EgoEngineLibrary.Graphics.Pssg;
using EgoEngineLibrary.Graphics.Pssg.Elements;

namespace GRID2VR.Tracks;

internal static class TrackVisuals
{
    sealed class State : PssgModelWriterState { }

    internal const string Opaque = "terrain_simple.fx";
    // Alpha-tested cutouts (trees, *_AT fences/guardrail): the decal below with the stock
    // object cutout states, added to the overlay shaderpack (CutoutShader). The plain decal
    // never writes depth, so foliage overlapped in draw order (0.2.23 test build), and the
    // stock Object_2SidedCov.fx drew untextured white cards from a tracksplit (run
    // 20261112-005451-060).
    internal const string Cutout = CutoutShader.Group + ".fx";
    // Translucent (blended) materials: GRID 2's decal shader takes alpha from
    // TDiffuseAlphaMap.a x vertex alpha, discards below 0.01, blends without depth write.
    internal const string Decal = "decal_ao_vc_flat.fx";
    // 32-bit game budget: AC maps above this lose whole top mip levels.
    internal const uint MaxTextureSize = 1024;
    const string Depth = "batched_track.fx";
    const string Lod = "terrain_lod.fx";
    static readonly HashSet<string> Supported = new(StringComparer.Ordinal)
    {
        "ksPerPixel", "ksTree", "ksGrass", "ksPerPixelMultiMap", "ksPerPixelSimpleRefl", "ksPerPixelReflection",
        "ksPerPixelNM_UVMult", "ksPerPixelNM", "ksPerPixelMultiMap_NMDetail", "ksMultilayer_objsp",
        "ksMultilayer_fresnel_nm", "ksPerPixelMultiMap_AT", "ksPerPixelAT", "ksPerPixelAlpha",
        "ksPerPixelMultiMap_AT_NMDetail", "ksPerPixelMultiMapSimpleRefl", "ksFlags"
    };

    // Opaque surfaces: terrain_simple; alpha-tested cutouts: two-sided coverage object
    // shader; blended materials: decal. Road layering is not reproduced yet.
    internal static string Target(Material material)
    {
        if (material.StockGroup is not null) return material.StockGroup;
        if (!Supported.Contains(material.Shader)) throw new InvalidDataException("Unmapped AC shader: " + material.Shader);
        return IsOpaque(material) ? Opaque : IsAlphaTested(material) ? Cutout : Decal;
    }
    internal static bool IsAlphaTested(Material material) => material.Blend == 0 && material.Shader != "ksPerPixelAlpha" &&
        (material.AlphaTest || material.Shader is "ksTree" or "ksPerPixelAT" || material.Shader.Contains("_AT", StringComparison.Ordinal));
    internal static bool IsOpaque(Material material) => material.Blend == 0 && !material.AlphaTest &&
        material.Shader is not ("ksTree" or "ksGrass" or "ksPerPixelAT" or "ksPerPixelAlpha") && !material.Shader.Contains("_AT", StringComparison.Ordinal);

    // Terrain shaders read vertex colour as weights, not colour. Stock terrain_simple.fx
    // vertices (Brands Hatch) have X = Z = W = 0 and Y in 0..1 (mean 0.76); all-ones
    // rendered converted terrain pure white. Z is 0 in every stock terrain shader.
    static readonly Vector4 TerrainWeights = new(0, 1, 0, 0);

    static XElement Element(PssgElement element) { var root = new XElement("root"); element.WriteXml(root); return root.Elements().Single(); }

    // Builds the venue tracksplit.pssg: donor terrain removed, source visuals as tiles.
    // Returns the tile bounds in VIS identity order.
    internal static (Box[] Tiles, object Report) Build(AcSource source, string donorVenue, string venueOut, Vector4[]? weightVariants = null, bool lodOverrides = true, bool keepDonorLand = false, int? fixedTiles = null, bool mergeDraws = true, bool lowCopies = false, bool reverseTileIds = false)
    {
        var tracksplit = Files.Pssg(Path.Combine(donorVenue, "tracksplit.pssg"));
        var objects = Files.Pssg(Path.Combine(donorVenue, "objects.pssg"));
        var layouts = ShaderInputInfo.CreateFromPssg(objects).Concat(ShaderInputInfo.CreateFromPssg(tracksplit))
            .GroupBy(l => l.ShaderGroupId).ToDictionary(g => g.Key, g => g.First());
        foreach (var required in new[] { Opaque, Decal, Depth, Lod })
            if (!layouts.ContainsKey(required)) throw new InvalidDataException("Donor lacks shader group " + required);
        var templates = new Dictionary<string, (XElement Group, XElement Instance)>();
        foreach (var (id, file) in new[] { (Opaque, tracksplit), (Decal, tracksplit) })
        {
            var group = file.GetObject<PssgShaderGroup>(id.AsMemory());
            var instance = file.Elements<PssgShaderInstance>().First(i => i.ShaderGroup == "#" + id);
            templates[id] = (Element(group), Element(instance));
        }
        // Cutout: the decal's tracksplit group and inputs under the overlay shaderpack's new id.
        {
            var (group, instance) = templates[Decal];
            var cutoutGroup = new XElement(group); cutoutGroup.SetAttributeValue("id", Cutout);
            var cutoutInstance = new XElement(instance); cutoutInstance.SetAttributeValue("shaderGroup", "#" + Cutout);
            templates[Cutout] = (cutoutGroup, cutoutInstance); layouts[Cutout] = layouts[Decal];
        }
        // ROOT tiles' depth batches use the donor's own HIGHBATCH material ("batchmaterial" on
        // Brands Hatch); "batchmaterial!0" is the LAND/NONLOD one. They differ only by id.
        string depthMaterial = tracksplit.Elements<PssgRenderNode>().Where(n => n.Id.StartsWith("HIGHBATCH_", StringComparison.Ordinal))
            .SelectMany(n => n.ChildElements.OfType<PssgRenderStreamInstance>()).Select(d => d.Shader.TrimStart('#')).GroupBy(s => s)
            .OrderByDescending(g => g.Count()).First().Key;

        // Remove every donor terrain draw and tile; keep resources (textures) and other nodes.
        // keepDonorLand (experiment): keep the donor's distant LAND tiles and their data.
        var keptLand = keepDonorLand ? Visibility.TerrainTiles(tracksplit).Where(t => t.Id.StartsWith("LAND_", StringComparison.Ordinal)).ToHashSet() : [];
        var keptDraws = keptLand.SelectMany(t => t.Elements<PssgRenderStreamInstance>()).ToHashSet();
        var keptData = keptDraws.Select(d => d.GetRenderDataSource()).SelectMany(s => new PssgElement[] { s, (PssgElement)s.ParentElement! }.Concat(s.ChildElements.OfType<PssgRenderStream>().Select(r => (PssgElement)tracksplit.Elements<PssgDataBlock>().Single(b => b.Id == r.DataBlock.TrimStart('#'))))).ToHashSet();
        foreach (var instance in tracksplit.Elements<PssgRenderInstance>().Where(i => !keptDraws.Contains(i)).ToArray()) instance.ParentElement!.RemoveChild(instance);
        var surface = tracksplit.Elements<PssgNode>().Single(n => n.Id == "surface");
        var donorTiles = Visibility.TerrainTiles(tracksplit);
        // Stock ROOT tiles: HIGH (detailed shader) within track_lod_dist of the tile, LOW
        // (terrain_lod.fx only) beyond it, each with a depth-only batch list; tiles beyond
        // track_cull_dist are not drawn (lod_overrides.xml, written by WriteLodOverrides).
        var template = Element(donorTiles.First(t => t.Id.StartsWith("ROOT_", StringComparison.Ordinal)));
        var renderTemplate = template.Elements("RENDERNODE").First();
        foreach (var tile in donorTiles.Where(t => !keptLand.Contains(t))) surface.RemoveChild(tile);
        // Donor geometry buffers are now unreferenced; drop them, keep textures.
        foreach (var data in tracksplit.Elements<PssgElement>().Where(e => e.Name is "DATABLOCK" or "SEGMENTSET" && !keptData.Contains(e)).ToArray()) data.ParentElement!.RemoveChild(data);
        // Kept LAND data sources stay inside their segment sets; drop other sources there.
        foreach (var set in keptData.OfType<PssgSegmentSet>()) { foreach (var s in set.Segments.Where(s => !keptData.Contains(s)).ToArray()) set.RemoveChild(s); set.SegmentCount = (uint)set.Segments.Count(); }

        // Grass tufts (ksGrass, 1.8 M triangles on the Nordschleife) wait for per-area visibility.
        var visuals = source.Visuals.Where(m => m.Material.StockGroup is not null || m.Material.Shader != "ksGrass")
            .Select(m => MultilayerMaterial.Read(m.Material) is { } layer ? layer.Apply(m, source.Origin) : m).ToList();
        int omittedCoverage = source.Visuals.Sum(m => m.Indices.Length / 3) - visuals.Sum(m => m.Indices.Length / 3);
        List<List<Mesh>> parts; int tileSize, farSize = 0;
        if (source.Gates.Length > 0)
        {
            // "Near" = within 250 m (horizontally) of the driving line.
            var corridor = new HashSet<Cell>();
            foreach (var g in source.Gates) for (int dx = -2; dx <= 2; dx++) for (int dz = -2; dz <= 2; dz++)
                corridor.Add(new((int)MathF.Floor(g.Position.X / 125) + dx, (int)MathF.Floor(g.Position.Z / 125) + dz));
            (parts, tileSize, farSize) = Tiles.PartitionNearFar(visuals, p => corridor.Contains(new((int)MathF.Floor(p.X / 125), (int)MathF.Floor(p.Z / 125))));
        }
        else (parts, tileSize) = Tiles.Partition(visuals, fixedTiles);
        if (reverseTileIds) parts.Reverse();
        int width = (int)MathF.Ceiling(MathF.Sqrt(parts.Count));
        var binaryTracksplit = tracksplit;
        var doc = Files.Document(tracksplit);
        XElement Library(string type) => doc.Descendants("LIBRARY").Single(e => (string?)e.Attribute("type") == type);
        var surfaceXml = doc.Descendants("NODE").Single(n => (string?)n.Attribute("id") == "surface");
        // Identity order is reverse file order, so write tile 0 last.
        string TileName(int i) => $"{i % width}_{i / width}";
        for (int i = parts.Count - 1; i >= 0; i--)
        {
            var root = new XElement(template); root.Elements("RENDERNODE").Remove();
            root.SetAttributeValue("id", "ROOT_" + TileName(i)); root.SetAttributeValue("nickname", "ROOT_" + TileName(i));
            foreach (string prefix in new[] { "LOWBATCH_", "LOW_", "HIGHBATCH_", "HIGH_", "DECAL_" })
            {
                var node = new XElement(renderTemplate); node.Elements().Where(e => e.Name != "TRANSFORM" && e.Name != "BOUNDINGBOX").Remove();
                node.SetAttributeValue("id", prefix + TileName(i)); node.SetAttributeValue("nickname", prefix + TileName(i)); root.Add(node);
            }
            surfaceXml.Add(root);
        }

        // Materials and textures.
        var materials = visuals.Select(m => m.Material).DistinctBy(m => m.Id).OrderBy(m => m.Id, StringComparer.Ordinal).ToArray();
        var ids = new Dictionary<string, string>(); var textureBytes = new Dictionary<string, byte[]>(); var report = new List<object>();
        var lowIds = new Dictionary<string, string>();
        var lodTemplate = doc.Descendants("SHADERINSTANCE").First(s => (string?)s.Attribute("shaderGroup") == "#" + Lod);
        int lodDiffuse = Array.FindIndex(doc.Descendants("SHADERGROUP").Single(g => (string?)g.Attribute("id") == Lod).Elements("SHADERINPUTDEFINITION").ToArray(),
            d => (string?)d.Attribute("name") == "TDiffuseAlphaMap");
        if (lodDiffuse < 0 || !lodTemplate.Elements("SHADERINPUT").Any(x => (int?)x.Attribute("parameterID") == lodDiffuse)) throw new InvalidDataException("Unexpected terrain_lod.fx template.");
        foreach (var target in materials.Where(m => m.StockGroup is null).Select(Target).Distinct())
            if (!Library("SHADERGROUP").Elements().Any(g => (string?)g.Attribute("id") == target)) Library("SHADERGROUP").Add(new XElement(templates[target].Group));
        string TextureId(string name, bool opaque)
        {
            if (!source.Textures.TryGetValue(name, out var texture)) throw new InvalidDataException("Missing embedded texture: " + name);
            string id = "g2vr_tex_" + texture.Hash[..20] + (opaque ? "_opaque" : ""); textureBytes.TryAdd(id, texture.Dds); return id;
        }
        for (int i = 0; i < materials.Length; i++)
        {
            var material = materials[i]; string target = Target(material);
            if (material.StockShader is not null)
            {
                if (!doc.Descendants("SHADERINSTANCE").Any(s => (string?)s.Attribute("id") == material.StockShader)) throw new InvalidDataException("Missing stock shader " + material.StockShader);
                ids[material.Id] = material.StockShader;
                report.Add(new { Source = material.Id, Target = target, StockShader = material.StockShader });
                continue;
            }
            var t = templates[target];
            string id = "g2vr_material_" + i; ids[material.Id] = id;
            var instance = new XElement(t.Instance); instance.SetAttributeValue("id", id);
            var definitions = t.Group.Elements("SHADERINPUTDEFINITION").ToArray();
            if (!material.Textures.TryGetValue("txDiffuse", out var diffuse)) throw new InvalidDataException("Material has no diffuse map: " + material.Id);
            var layer = MultilayerMaterial.Read(material);
            string diffuseId = TextureId(layer?.Detail ?? diffuse, IsOpaque(material));
            string colourId = layer is null ? "g2vr_grey" : TextureId(layer.Shading, true);
            for (int j = 0; j < definitions.Length; j++)
            {
                if ((string?)definitions[j].Attribute("type") != "texture" || (string?)definitions[j].Attribute("format") is not null) continue;
                string name = (string)definitions[j].Attribute("name")!;
                var input = instance.Elements("SHADERINPUT").SingleOrDefault(x => (int?)x.Attribute("parameterID") == j);
                if (input is null) { input = new XElement("SHADERINPUT", new XAttribute("parameterID", j)); instance.Add(input); }
                // The stock wet mask stays as authored (dry); everything else is neutral.
                if (name == "TWetMask" && (string?)input.Attribute("texture") is not null) continue;
                string texture = name switch
                {
                    "TDiffuseAlphaMap" or "TDiffuseSpecMap1" => diffuseId,
                    "TOcclusionMap" or "TAmbientOcclusion" or "TAmbientOcclusionMap" => "g2vr_white",
                    // Grey x ColourMapScale 2 = unchanged colour; multilayer: AC shading map x 1.
                    "TColourMap" => colourId,
                    "TNormalMap" => "g2vr_normal",
                    _ => "g2vr_black"
                };
                input.SetAttributeValue("type", "texture"); input.SetAttributeValue("texture", "#" + texture); input.RemoveNodes();
            }
            // Source UVs as authored; no terrain specular from AC's diffuse alpha.
            foreach (var (name, value) in new[] { ("Map1UVScaleAndOffset", "1 1 0 0"), ("PhongColour", "0 0 0"), ("ColourMapScale", layer is null ? "2" : "1"), ("TrackAOScale", "1"), ("SpecScale", "0") })
            {
                int parameter = Array.FindIndex(definitions, d => (string?)d.Attribute("name") == name);
                if (parameter < 0) continue;
                var input = instance.Elements("SHADERINPUT").SingleOrDefault(x => (int?)x.Attribute("parameterID") == parameter);
                if (input is null) { input = new XElement("SHADERINPUT", new XAttribute("parameterID", parameter)); instance.Add(input); }
                input.SetAttributeValue("type", "constant"); input.SetAttributeValue("format", definitions[parameter].Attribute("format")!.Value);
                input.Attributes("texture").Remove(); input.Value = value;
            }
            instance.SetAttributeValue("parameterSavedCount", instance.Elements("SHADERINPUT").Count());
            Library("SHADERINSTANCE").Add(instance);
            report.Add(new { Source = material.Id, material.Name, material.Shader, material.Blend, material.AlphaTest, Target = target, Diffuse = layer?.Detail ?? diffuse,
                ColourMap = layer?.Shading, DetailScale = layer?.Scale,
                Approximation = layer is not null ? "Multilayer: R/grass detail on source world XZ x shading map; other detail layers, normal maps and reflections not reproduced."
                    : target == Decal ? "Blend: diffuse alpha through decal_ao_vc_flat (no depth write); no depth batch, no distant LOD."
                    : target == Cutout ? "Cutout: decal_ao_vc_flat programs with object_2sidedcov states (alpha test + coverage, depth written, two-sided); no depth batch, no distant LOD."
                    : "Source diffuse and UVs; specular black, occlusion white. Normal maps and reflections are not reproduced." });
            // Cutouts have no distant (LOW) version.
            if (target != Opaque) continue;
            // Distant (LOW) version: terrain_lod.fx with the same diffuse.
            var low = new XElement(lodTemplate); low.SetAttributeValue("id", "g2vr_low_" + i); lowIds[material.Id] = "g2vr_low_" + i;
            foreach (var input in low.Elements("SHADERINPUT").Where(x => (int?)x.Attribute("parameterID") == lodDiffuse)) input.SetAttributeValue("texture", "#" + diffuseId);
            Library("SHADERINSTANCE").Add(low);
        }
        long textureBytesTrimmed = 0;
        var textureTemplate = doc.Descendants("TEXTURE").First();
        var textureLibrary = textureTemplate.Parent!;
        foreach (var id in textureBytes.Keys.Concat(["g2vr_white", "g2vr_black", "g2vr_grey", "g2vr_normal"]))
        {
            // Keep the image-block structure the DDS importer fills; drop the donor pixels.
            var texture = new XElement(textureTemplate); texture.SetAttributeValue("id", id);
            foreach (var data in texture.Descendants().Where(e => !e.HasElements && e.Name.LocalName.EndsWith("DATA", StringComparison.Ordinal))) data.Value = "";
            textureLibrary.Add(texture);
        }
        tracksplit = Files.FromDocument(doc, binaryTracksplit);
        foreach (var texture in tracksplit.Elements<PssgTexture>().Where(t => t.Id.StartsWith("g2vr_", StringComparison.Ordinal)))
        {
            DdsFile dds;
            if (textureBytes.TryGetValue(texture.Id, out var payload))
            {
                using var memory = new MemoryStream(payload, false); dds = new DdsFile(memory);
                if (dds.header.width == 0 || dds.header.height == 0 || dds.header.width > 8192 || dds.header.height > 8192) throw new InvalidDataException("Invalid DDS dimensions.");
                textureBytesTrimmed += DdsAlpha.Shrink(dds, MaxTextureSize);
                DdsAlpha.Prepare(dds, texture.Id.EndsWith("_opaque", StringComparison.Ordinal));
            }
            else
            {
                // Solid-colour 64x64 DXT1 (stock textures are all block-compressed; 1x1 RGBA
                // constants left converted terrain flat black/white). RGB565 colour, indices 0.
                ushort colour = texture.Id switch { "g2vr_white" => 0xFFFF, "g2vr_grey" => 0x8410, "g2vr_normal" => 0x841F, _ => 0x0000 };
                dds = new DdsFile(); dds.header.width = 64; dds.header.height = 64; dds.header.mipMapCount = 1;
                dds.header.flags |= DdsHeader.Flags.DDSD_LINEARSIZE; dds.header.pitchOrLinearSize = 16 * 16 * 8;
                dds.header.ddspf.flags = DdsPixelFormat.Flags.DDPF_FOURCC; dds.header.ddspf.fourCC = 0x31545844; // DXT1
                dds.bdata = new byte[16 * 16 * 8];
                for (int b = 0; b < dds.bdata.Length; b += 8)
                {
                    System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(dds.bdata.AsSpan(b), colour);
                    System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(dds.bdata.AsSpan(b + 2), colour);
                }
            }
            dds.ToPssgElement(texture); texture.AutoMipMap = false;
            if (texture.TexelFormat is not ("dxt1" or "dxt3" or "dxt5" or "ui8x4")) throw new InvalidDataException("Unsupported target texture format: " + texture.Id + " " + texture.TexelFormat);
        }

        // Geometry.
        var segmentLib = tracksplit.Elements<PssgLibrary>().Single(l => l.Type == "SEGMENTSET");
        var dataLib = tracksplit.Elements<PssgLibrary>().Single(l => l.Type == "RENDERINTERFACEBOUND");
        var tiles = Visibility.TerrainTiles(tracksplit);
        if (tiles.Length != parts.Count + keptLand.Count) throw new InvalidDataException("Tile inventory changed while writing.");
        var state = new State(); int meshId = 0, triangles = 0, depthTriangles = 0; var bounds = new Box[parts.Count];
        for (int t = 0; t < parts.Count; t++)
        {
            var root = tiles[t];
            if (root.Id != "ROOT_" + TileName(t)) throw new InvalidDataException("Unexpected tile order: " + root.Id);
            PssgRenderNode Node(string prefix) => root.ChildElements.OfType<PssgRenderNode>().Single(n => n.Id == prefix + TileName(t));
            var high = Node("HIGH_"); var lowNode = Node("LOW_"); var decal = Node("DECAL_"); var depth = new[] { Node("HIGHBATCH_"), Node("LOWBATCH_") };
            // Stock layout: each render node draws from exactly one SEGMENTSET holding all
            // of its data sources; nodes never share data (HIGHBATCH and LOWBATCH included).
            var sets = new Dictionary<PssgRenderNode, PssgSegmentSet>();
            PssgSegmentSet SetFor(PssgRenderNode node)
            {
                if (sets.TryGetValue(node, out var set)) return set;
                set = new PssgSegmentSet(tracksplit, segmentLib) { Id = "g2vr_set_" + node.Id }; segmentLib.AppendChild(set);
                return sets[node] = set;
            }
            void Draw(PssgRenderNode node, string data, string shader)
            {
                var instance = new PssgRenderStreamInstance(tracksplit, node) { Id = data + "_" + node.Id, SourceCount = 1, Indices = "#" + data, StreamCount = 0, Shader = "#" + shader };
                node.AppendChild(instance); instance.AppendChild(new PssgRenderInstanceSource(tracksplit, instance) { Source = "#" + data });
            }
            // Each render node's BOUNDINGBOX = bounds of its own geometry, as in every stock
            // tracksplit (GRID 2 culls render nodes by it; the template's donor box hid scenery).
            var nodeBoxes = new Dictionary<PssgRenderNode, Box>();
            void Grow(PssgRenderNode node, IReadOnlyList<Vector3> positions)
            {
                if (positions.Count == 0) return;
                var box = new Box(positions.Aggregate(Vector3.Min), positions.Aggregate(Vector3.Max));
                nodeBoxes[node] = nodeBoxes.TryGetValue(node, out var b) ? b.Union(box) : box;
            }
            Box? tileBox = null; var opaque = new List<Mesh>();
            foreach (var mesh in mergeDraws ? VisualBatches.Combine(parts[t].Select(RecentreUV)) : parts[t].Select(RecentreUV))
            {
                string id = "g2vr_RDS_" + meshId++; var writer = new RenderDataSourceWriter(id);
                // Non-opaque: decal_ao_vc_flat alpha = texture alpha x vertex colour w (white).
                bool cutout = mesh.Material.StockGroup is null && !IsOpaque(mesh.Material);
                for (int i = 0; i < mesh.Positions.Length; i++)
                {
                    var p = mesh.Positions[i]; tileBox = tileBox is { } b ? b.Union(new(p, p)) : new(p, p);
                    writer.Positions.Add(p); writer.Normals.Add(mesh.Normals[i]); writer.Tangents.Add(new(mesh.Tangents[i], 1));
                    writer.Colors.Add(mesh.Colors?[i] ?? (cutout ? Vector4.One : weightVariants is { Length: > 0 } v ? v[t % v.Length] : TerrainWeights));
                    // Explicit UV sets (stock controls) are written as given; otherwise the source UV fills all.
                    Vector2 Set(int k) => mesh.TexCoords is { } sets && sets.Length > 0 ? sets[Math.Min(k, sets.Length - 1)][i] : mesh.UV[i];
                    writer.TexCoords0.Add(Set(0)); writer.TexCoords1.Add(Set(1));
                    writer.TexCoords2.Add(Set(2)); writer.TexCoords3.Add(Set(3));
                }
                writer.Indices.AddRange(mesh.Indices.Select(i => (uint)i));
                // Blended materials draw as decals; opaque and alpha-tested ones in HIGH (depth written).
                var drawNode = cutout && Target(mesh.Material) == Decal ? decal : high;
                writer.Write(layouts[Target(mesh.Material)], SetFor(drawNode), dataLib, state);
                Draw(drawNode, id, ids[mesh.Material.Id]); Grow(drawNode, mesh.Positions);
                // LOW: the same triangles through terrain_lod.fx (position + UV).
                if (lowCopies && lowIds.TryGetValue(mesh.Material.Id, out var lowShader))
                {
                    string lowId = "g2vr_low_RDS_" + (meshId - 1); var lowWriter = new RenderDataSourceWriter(lowId);
                    lowWriter.Positions.AddRange(mesh.Positions); lowWriter.TexCoords0.AddRange(mesh.TexCoords is { Length: > 0 } uvSets ? uvSets[0] : mesh.UV); lowWriter.Indices.AddRange(writer.Indices);
                    lowWriter.Write(layouts[Lod], SetFor(lowNode), dataLib, state);
                    Draw(lowNode, lowId, lowShader); Grow(lowNode, mesh.Positions);
                }
                triangles += mesh.Indices.Length / 3;
                if (IsOpaque(mesh.Material)) { opaque.Add(mesh); depthTriangles += mesh.Indices.Length / 3; }
            }
            // Depth-only batch lists: stock tiles merge the whole level into ONE draw
            // (Brands HIGHBATCH_5_2 = all 26 HIGH_5_2 draws, 33,585 indices). Merge the
            // tile's opaque geometry the same way, splitting only at the 16-bit index limit.
            foreach (var batch in depth)
            {
                int part = 0; var merged = new RenderDataSourceWriter("");
                void Flush()
                {
                    if (merged.Positions.Count == 0) return;
                    string depthId = $"g2vr_depth_{TileName(t)}_{batch.Id[..batch.Id.IndexOf('_')]}_{part++}";
                    var w = new RenderDataSourceWriter(depthId); w.Positions.AddRange(merged.Positions); w.Indices.AddRange(merged.Indices);
                    w.Write(layouts[Depth], SetFor(batch), dataLib, state); Draw(batch, depthId, depthMaterial); Grow(batch, merged.Positions);
                    merged = new RenderDataSourceWriter("");
                }
                foreach (var mesh in opaque)
                {
                    if (merged.Positions.Count + mesh.Positions.Length > ushort.MaxValue) Flush();
                    uint offset = (uint)merged.Positions.Count;
                    merged.Positions.AddRange(mesh.Positions); merged.Indices.AddRange(mesh.Indices.Select(i => i + offset));
                }
                Flush();
            }
            foreach (var (node, set) in sets)
            {
                // Stock sets list their sources in REVERSE draw order (Brands HIGH_3_6: draws
                // 74, 73 … 0), and GRID 2 pairs draws with set entries by that position: forward
                // order drew nothing although every reference resolved.
                var order = node.ChildElements.OfType<PssgRenderStreamInstance>().Select(d => d.Indices.TrimStart('#')).Reverse().ToArray();
                var sources = set.Segments.ToDictionary(s => s.Id);
                if (order.Length != sources.Count || order.Any(id => !sources.ContainsKey(id))) throw new InvalidDataException("Segment set does not match its render node: " + node.Id);
                foreach (var entry in sources.Values) set.RemoveChild(entry);
                foreach (var id in order) set.AppendChild(sources[id]);
                set.SegmentCount = (uint)set.Segments.Count();
                foreach (var data in set.Segments) data.Primitive = "triangles";
            }
            // Not every stock tile has decals; drop the node rather than leave it drawless.
            if (!decal.ChildElements.OfType<PssgRenderStreamInstance>().Any()) root.RemoveChild(decal);
            bounds[t] = tileBox ?? throw new InvalidDataException("Empty tile " + root.Id);
            // Drawless nodes (empty LOW) get the tile's bounds; the tile node itself stays zero as stock.
            foreach (var node in root.ChildElements.OfType<PssgRenderNode>())
            {
                var box = nodeBoxes.TryGetValue(node, out var b) ? b : bounds[t];
                node.BoundingBox.BoundsMin = box.Min; node.BoundingBox.BoundsMax = box.Max;
            }
        }
        // Replaces the donor copy placed in the overlay.
        Console.WriteLine("Writing track tiles...");
        PssgLayout.StockStyle(tracksplit, "g2vr_");
        string savedPath = Path.Combine(venueOut, "tracksplit.pssg"); Files.NoLinks(savedPath); File.Delete(savedPath);
        Files.Save(tracksplit, savedPath);
        Files.VerifyUnschemed(Files.Pssg(Path.Combine(donorVenue, "tracksplit.pssg")), savedPath);
        // Read back: every draw resolves, tile identities and bounds are as written.
        var saved = Files.Pssg(Path.Combine(venueOut, "tracksplit.pssg"));
        foreach (var instance in saved.Elements<PssgRenderStreamInstance>()) { _ = instance.GetRenderDataSource(); _ = instance.GetShaderInstance(); }
        var savedTiles = Visibility.TerrainTiles(saved);
        if (savedTiles.Length != parts.Count + keptLand.Count || Enumerable.Range(0, parts.Count).Any(i => !bounds[i].Contains(Visibility.Bounds(savedTiles[i]), .05f)))
            throw new InvalidDataException("Saved tracksplit tiles do not match their bounds.");
        if (CheckNodeBoxes(saved) is { Mismatched: > 0 } boxes)
            throw new InvalidDataException($"{boxes.Mismatched} render node bounding boxes differ from their geometry{boxes.Sample}.");
        object lod = lodOverrides ? WriteLodOverrides(donorVenue, venueOut, tileSize, Math.Max(tileSize, farSize), highToCull: !lowCopies) : "stock";
        // VIS identities cover every terrain tile, kept LAND tiles included (after ours).
        bounds = [.. bounds, .. savedTiles.Skip(parts.Count).Select(Visibility.Bounds)];
        return (bounds, new { KeptDonorLandTiles = keptLand.Count, MaterialMappings = report, Tiles = parts.Count, NearTileSizeMetres = tileSize, FarTileSizeMetres = farSize, LodOverrides = lod, Meshes = meshId, Triangles = triangles, OpaqueDepthTriangles = depthTriangles,
            Materials = materials.Length, UniqueTextures = textureBytes.Count, TextureBytes = textureBytes.Values.Sum(b => (long)b.Length), TextureBytesTrimmed = textureBytesTrimmed, MaxTextureSize, LowCopies = lowCopies, ReverseTileIds = reverseTileIds, DonorTerrainRemoved = donorTiles.Length, OmittedCoverageTriangles = omittedCoverage });
    }

    // Every stock terrain render node stores the bounds of its own geometry; GRID 2
    // culls render nodes by that box (a copied donor box hid all converted scenery
    // whenever the camera faced away from that donor spot).
    internal static (int Checked, int Mismatched, string Sample) CheckNodeBoxes(PssgFile pssg)
    {
        int count = 0, bad = 0; string sample = "";
        foreach (var node in Visibility.TerrainTiles(pssg).SelectMany(t => t.ChildElements.OfType<PssgRenderNode>()))
        {
            if (!node.ChildElements.OfType<PssgRenderStreamInstance>().Any()) continue;
            count++;
            var stored = new Box(node.BoundingBox.BoundsMin, node.BoundingBox.BoundsMax);
            var geometry = Visibility.Bounds(node);
            if (!stored.Contains(geometry) || !geometry.Contains(stored))
            {
                if (bad++ == 0) sample = $"; e.g. {node.Id}: stored {stored}, geometry {geometry}";
            }
        }
        return (count, bad, sample);
    }

    // Venue draw distances (lod_overrides.xml, text XML). Stock Brands Hatch: terrain HIGH
    // within 90 m, terrain culled beyond 530 m, objects beyond 730 m, world clip 1000 m,
    // tuned for ~200 m tiles. Converted tiles are larger and the course has long views.
    internal static object WriteLodOverrides(string donorVenue, string venueOut, int nearTile, int farTile, bool highToCull = false)
    {
        var doc = XDocument.Load(Path.Combine(donorVenue, "lod_overrides.xml"));
        // highToCull: LOW nodes are left empty (no second copy of every opaque triangle in a
        // 32-bit process), so tiles stay on HIGH until they are culled.
        float cull = MathF.Max(2500, farTile * 1.5f), lodDistance = highToCull ? cull : MathF.Max(90, nearTile * 1.5f), world = cull, clip = cull + 500;
        string F(float v) => v.ToString("0.000000", System.Globalization.CultureInfo.InvariantCulture);
        foreach (var name in new[] { "defaults", "replay" })
        {
            var e = doc.Root!.Element(name) ?? throw new InvalidDataException("Unexpected lod_overrides.xml");
            e.SetAttributeValue("track_lod_dist", F(lodDistance)); e.SetAttributeValue("track_cull_dist", F(cull)); e.SetAttributeValue("world_cull_dist", F(world));
        }
        doc.Root!.Elements("zone").Remove(); // Donor-specific zones.
        doc.Root.Element("lods")?.SetAttributeValue("world_clip_dist", F(clip));
        string path = Path.Combine(venueOut, "lod_overrides.xml"); Files.NoLinks(path); File.Delete(path); doc.Save(path);
        return new { TrackLodDistance = lodDistance, TrackCullDistance = cull, WorldCullDistance = world, WorldClipDistance = clip };
    }

    // GRID 2 object shaders read half-precision UVs; move each mesh's UVs near zero
    // by whole texture repeats so large AC UV values keep their precision.
    static Mesh RecentreUV(Mesh mesh)
    {
        if (mesh.UV.Length == 0 || mesh.TexCoords is not null) return mesh;
        var min = mesh.UV.Aggregate(Vector2.Min);
        var shift = new Vector2(MathF.Floor(min.X), MathF.Floor(min.Y));
        return shift == Vector2.Zero ? mesh : mesh with { UV = mesh.UV.Select(uv => uv - shift).ToArray() };
    }
}
