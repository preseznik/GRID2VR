using System.Xml.Linq;

namespace GRID2VR.Tracks;

// Alpha-tested foliage for tracksplit tiles. No stock tracksplit shader both writes depth
// and alpha-tests: the decal groups blend without depth write (trees overlapped in draw
// order, 0.2.23 test build), and the object cutout Object_2SidedCov.fx drew untextured
// white cards from a tracksplit. This adds one group to an overlay copy of the shaderpack:
// decal_ao_vc_flat's own programs and inputs, with the stock object_2sidedcov cutout
// states (alpha test, alpha-to-coverage, depth write, no culling). The copy is used only
// by custom-track sessions (-mod); every stock group, program and state stays identical.
internal static class CutoutShader
{
    internal const string Group = "g2vr_cutout_vc_flat";
    // Must start with "terrain_": GRID 2 gives terrain treatment (the per-tile terrainXFade
    // that is the colour pass's output alpha) by that name prefix (the only such literal in
    // grid2_avx.exe). As "g2vr_terrain_vc" the colour pass drew at alpha 0: black ground over
    // the depth pre-pass and stale-frame ghosts (xr-sim run 20261112-084821-485).
    internal const string TerrainGroup = "terrain_g2vr_vc";
    internal const string FoliageShadowGroup = "g2vr_foliage_shadow", FoliageShadowProgram = "g2vr_foliage_shadow.vp";
    internal const string CasterGroup = "g2vr_cutout_caster";
    internal const string Relative = "shaderpack/dx11/shaderpack.pssg";
    // Shadow near the car. Stock terrain and decal day passes use only the dynamic shadow map
    // within ~33 m (shadowBlend lerp to the baked AO-alpha sun), and converted foliage casts no
    // dynamic shadow (casters cost 16 fps in VR), so baked tree shadow washed out in a radius
    // around the car (iztok4090, 0.2.23). Every converted ground/decal group therefore takes
    // min(dynamic, baked) at all distances (DxbcPatch.ShadowMin): the car's own shadow and the
    // baked one both show. Plain opaque ground and decals use these copies; stock groups (also
    // drawn by cars and stock objects in custom-track sessions) stay unchanged.
    internal const string GroundGroup = "terrain_g2vr_simple", PlainDecalGroup = "g2vr_decal_plain";
    const string ShadowPrefix = "g2vr_sm.";
    // Layer variants with a depth offset: the blended decal and the cutout. Only triangles
    // lying on an opaque surface use them (TrackVisuals.SplitLayers): on free-standing cards
    // the slope term pulled hedges and fences metres forward through opaque armco in the first
    // offset build (iztok4090, 0.2.23 174852: "trackside barriers are translucent").
    internal const string DecalGroup = "g2vr_decal_vc_flat", LayerCutoutGroup = "g2vr_cutout_layer";
    // SHADERGROUPPASS polyOffsetEnable/Factor/Units (EGO schema; GL convention: negative is
    // toward the viewer for GRID 2's LEQUAL depth, confirmed by an exaggerated test offset
    // drawing the track-edge line over the car, run 20261112-172915-021): factor scales with
    // the surface's depth slope, units are depth-buffer steps.
    // Strength: the VR cockpit camera keeps a 2 cm near plane (roof clearance; stock 0.2 m), so
    // D24 steps are 10x coarser than on Desktop. At iztok's graffiti wall (graffiti_clean 3-8 cm
    // in front of barriers, ~40-80 m away) the layer still lost with (-1, -4) and held with a
    // 5x finer near plane (xr-sim runs 20261112-222352-318 / 223129-906): the fight spans ~10-20
    // steps there. Both layer groups therefore pull 32 steps plus twice the slope.
    internal static readonly (float Factor, float Units) DecalOffset = (-2, -32), CutoutOffset = (-2, -32);
    static void Offset(XElement pass, (float Factor, float Units) offset)
    {
        // Schema order (EGO SHADERGROUPPASS: ... polyFillType, polyOffset*, colorMask*, stencil*,
        // ... passConfigMask*, coupleVertexAndPixelProgram), so the read-back compares attribute
        // for attribute: before the first attribute that follows them (not every pass has polyFillType).
        if (pass.Attribute("polyOffsetEnable") is not null) throw new InvalidDataException("Stock pass already has a polygon offset.");
        var attributes = pass.Attributes().ToList();
        static bool After(string name) => name.StartsWith("colorMask", StringComparison.Ordinal) || name.StartsWith("stencil", StringComparison.Ordinal) ||
            name.StartsWith("2Sided", StringComparison.Ordinal) || name.StartsWith("passConfigMask", StringComparison.Ordinal) || name == "coupleVertexAndPixelProgram";
        int at = attributes.FindIndex(x => After(x.Name.LocalName)); if (at < 0) at = attributes.Count;
        attributes.InsertRange(at, [new XAttribute("polyOffsetEnable", 1),
            new XAttribute("polyOffsetFactor", offset.Factor.ToString("0.000000000e+000", System.Globalization.CultureInfo.InvariantCulture)),
            new XAttribute("polyOffsetUnits", offset.Units.ToString("0.000000000e+000", System.Globalization.CultureInfo.InvariantCulture))]);
        pass.ReplaceAttributes(attributes);
    }

    // Rewrites each pass's pixel shader that carries the shadowBlend lerp. Programs already added
    // by this writer are patched in place; stock ones are copied under ShadowPrefix (shared by
    // every group that uses them). Passes without the lerp (cheap, depth, night) stay as they are.
    static List<object> ShadowMin(XDocument candidate, XElement group, List<XElement> newPrograms)
    {
        var report = new List<object>();
        foreach (var pass in group.Elements("SHADERGROUPPASS"))
        {
            string programId = ((string)pass.Attribute("fragmentProgram")!).TrimStart('#');
            var added = newPrograms.SingleOrDefault(p => (string?)p.Attribute("id") == programId);
            if (added is null && newPrograms.Any(p => (string?)p.Attribute("id") == ShadowPrefix + programId))
            { pass.SetAttributeValue("fragmentProgram", "#" + ShadowPrefix + programId); report.Add(new { Pass = (string?)pass.Attribute("passConfigMaskLow"), Program = programId, Patched = true, Shared = true }); continue; }
            var program = added ?? candidate.Descendants("SHADERPROGRAM").Single(p => (string?)p.Attribute("id") == programId);
            var code = program.Descendants("SHADERPROGRAMCODEBLOCK").SingleOrDefault();
            if (code is null) continue;
            var bytes = Convert.FromHexString(string.Concat(code.Value.Where(char.IsAsciiHexDigit)));
            DxbcPatch.Result result;
            try { result = DxbcPatch.ShadowMin(bytes); }
            catch (InvalidDataException e) when (e.Message.StartsWith("Expected one shadowBlend lerp", StringComparison.Ordinal)) { report.Add(new { Pass = (string?)pass.Attribute("passConfigMaskLow"), Program = programId, Patched = false, Reason = e.Message }); continue; }
            var target = added ?? new XElement(program);
            if (added is null) { target.SetAttributeValue("id", ShadowPrefix + programId); newPrograms.Add(target); pass.SetAttributeValue("fragmentProgram", "#" + ShadowPrefix + programId); }
            target.Descendants("SHADERPROGRAMCODE").Single().SetAttributeValue("codeSize", result.Code.Length);
            target.Descendants("SHADERPROGRAMCODEBLOCK").Single().Value = string.Join(" ", result.Code.Select(b => b.ToString("X2")));
            report.Add(new { Pass = (string?)pass.Attribute("passConfigMaskLow"), Program = programId, Patched = true, Bytes = result.Code.Length });
        }
        return report;
    }

    internal static object Write(string gameRoot, string overlay, IDictionary<string, string> inputs)
    {
        string source = Files.Inside(gameRoot, Relative); Files.NoLinks(source); inputs[source] = Files.Hash(source);
        var originalFile = Files.Pssg(source);
        var original = Files.Document(originalFile); var candidate = new XDocument(original);
        XElement GroupIn(XDocument doc, string id) => doc.Descendants("SHADERGROUP").Single(g => (string?)g.Attribute("id") == id);
        if (candidate.Descendants("SHADERGROUP").Any(g => (string?)g.Attribute("id") == Group)) throw new InvalidDataException("Shaderpack already has " + Group);
        var decal = GroupIn(candidate, "decal_ao_vc_flat");
        var masks = decal.Elements("SHADERGROUPPASS").Select(p => (string?)p.Attribute("passConfigMaskLow")).ToArray();
        if ((int?)decal.Attribute("passCount") != 3 || !masks.SequenceEqual(["1", "67108864", "32"])) throw new InvalidDataException("Unexpected stock decal_ao_vc_flat passes.");
        var cutout = new XElement(decal); cutout.SetAttributeValue("id", Group);
        foreach (var pass in cutout.Elements("SHADERGROUPPASS"))
        {
            // object_2sidedcov: main passes (1, 67108864) ONE/ZERO, test GREATER 0, coverage level 2;
            // cheap pass (32) no blend, test GREATER 0.25. All write depth. Culling stays the
            // decal's CW: AC foliage is two-sided by reversed duplicate triangles (trees1: 167,169
            // of 447,408; `ac-cutouts`), and with cull NONE both copies drew at one depth with
            // opposite normals and fought (flickering trees, run 20261112-010757).
            bool main = (string?)pass.Attribute("passConfigMaskLow") != "32";
            pass.SetAttributeValue("blendEnable", main ? 1 : 0);
            pass.SetAttributeValue("blendSource", main ? "ONE" : "SRC_ALPHA"); pass.SetAttributeValue("blendDest", main ? "ZERO" : "ONE_MINUS_SRC_ALPHA");
            pass.SetAttributeValue("alphaTestEnable", 1); pass.SetAttributeValue("alphaTestFunc", "GREATER");
            pass.SetAttributeValue("alphaTestRef", main ? "0.000000000e+000" : "2.500000000e-001");
            pass.SetAttributeValue("alphaToCoverageEnable", main ? 1 : 0); pass.SetAttributeValue("alphaToCoverageLevel", main ? 2 : 0);
            pass.SetAttributeValue("depthMaskEnable", 1);
            if ((string?)pass.Attribute("cullFaceType") != "CW") throw new InvalidDataException("Unexpected decal culling.");
        }
        decal.AddAfterSelf(cutout);
        var layerCutout = new XElement(cutout); layerCutout.SetAttributeValue("id", LayerCutoutGroup);
        foreach (var pass in layerCutout.Elements("SHADERGROUPPASS")) Offset(pass, CutoutOffset);
        cutout.AddAfterSelf(layerCutout);
        // Blended decals (road paint, graffiti) with a depth offset toward the viewer. The VR
        // cockpit camera's near plane is 2 cm (far 3572 m, D24): depth steps reach ~0.75 cm at
        // 50 m and ~3 cm at 100 m, so AC's decals 1-2 cm above the road z-fought in VR on
        // PC4090 (session 20261112-161850-400, near 0x3ca3d70a) and not in the 0.2 m chase
        // view. Stock decal_ao_vc_flat carries no offset and stays unchanged.
        var offsetDecal = new XElement(decal); offsetDecal.SetAttributeValue("id", DecalGroup);
        foreach (var pass in offsetDecal.Elements("SHADERGROUPPASS")) Offset(pass, DecalOffset);
        cutout.AddAfterSelf(offsetDecal);
        var plainDecal = new XElement(decal); plainDecal.SetAttributeValue("id", PlainDecalGroup);
        offsetDecal.AddAfterSelf(plainDecal);

        // Vertex-colour lit terrain for multilayer ground: terrain_simple with its day colour
        // passes' TAmbientOcclusion sample replaced by COLOR0, which terrain_simple's vertex
        // shader already passes through and those pixel shaders ignored. Multilayer meshes
        // keep their AC shading map on the AO/colour-map UV set (all stock terrain shaders
        // share one UV for both) and carry baked ambient + sun visibility per vertex.
        var terrain = GroupIn(candidate, "terrain_simple");
        var vc = new XElement(terrain); vc.SetAttributeValue("id", TerrainGroup);
        var patched = new List<object>(); var newPrograms = new List<XElement>();
        foreach (var pass in vc.Elements("SHADERGROUPPASS"))
        {
            string programId = ((string)pass.Attribute("fragmentProgram")!).TrimStart('#');
            var program = candidate.Descendants("SHADERPROGRAM").Single(p => (string?)p.Attribute("id") == programId);
            var code = program.Descendants("SHADERPROGRAMCODEBLOCK").SingleOrDefault();
            if (code is null) continue;
            if (!program.Descendants("SHADERINPUTDEFINITION").Any(d => (string?)d.Attribute("name") == "TAmbientOcclusion")) continue;
            var bytes = Convert.FromHexString(string.Concat(code.Value.Where(char.IsAsciiHexDigit)));
            DxbcPatch.Result result;
            // Variants that cannot take it (already read COLOR0, no COLOR0 input, no AO
            // sample: night/light, cheap and depth passes) keep the stock program.
            try { result = DxbcPatch.SampleToInput(bytes, "TAmbientOcclusion", "COLOR", 0); }
            catch (InvalidDataException e) when (!e.Message.Contains("checksum") && !e.Message.Contains("rejected")) { patched.Add(new { Pass = (string?)pass.Attribute("passConfigMaskLow"), Program = programId, Patched = false, Reason = e.Message }); continue; }
            var copy = new XElement(program); copy.SetAttributeValue("id", "g2vr_vc." + programId);
            var copyCode = copy.Descendants("SHADERPROGRAMCODE").Single(); copyCode.SetAttributeValue("codeSize", result.Code.Length);
            copy.Descendants("SHADERPROGRAMCODEBLOCK").Single().Value = string.Join(" ", result.Code.Select(b => b.ToString("X2")));
            newPrograms.Add(copy); pass.SetAttributeValue("fragmentProgram", "#g2vr_vc." + programId);
            patched.Add(new { Pass = (string?)pass.Attribute("passConfigMaskLow"), Program = programId, Patched = true, Bytes = result.Code.Length, Input = "v" + result.InputRegister });
        }
        // The day colour pass the game uses (4359E7BF, read live in run 20261112-082338-345) must be patched.
        if (!newPrograms.Any(p => ((string)p.Attribute("id")!).EndsWith("terrain_simple.cg.fp.4359E7BF", StringComparison.Ordinal)))
            throw new InvalidDataException("The terrain_simple day colour pass could not read vertex colour.");
        terrain.AddAfterSelf(vc);
        var ground = new XElement(terrain); ground.SetAttributeValue("id", GroundGroup);
        vc.AddAfterSelf(ground);

        // Foliage as dynamic shadow casters. GRID 2 renders its cascaded shadow map from the
        // tiles' HIGHBATCH lists (batched_track: position-only, mask 8). Near the camera the
        // terrain uses only that dynamic shadow (shadowBlend), so baked foliage shadow never
        // shows there. This group is batched_track's shadow pass made alpha-tested: the stock
        // object_2sidedcov mask-8 pass state and its pixel shader (csm_depth_opaque.psh.
        // mainAlphaTested: discard below 0.5 in TDiffuseAlphaMap.a) with a compiled
        // world-space vertex shader identical to batched_track's shadow transform plus UV.
        var batched = GroupIn(candidate, "batched_track"); var objectCov = GroupIn(candidate, "object_2sidedcov");
        var stockShadowPass = objectCov.Elements("SHADERGROUPPASS").Single(p => (string?)p.Attribute("passConfigMaskLow") == "8" && (string?)p.Attribute("passConfigMaskHigh") == "0");
        if ((string?)stockShadowPass.Attribute("fragmentProgram") != "#csm_depth_opaque.psh.mainAlphaTested.13E6F496") throw new InvalidDataException("Unexpected stock alpha-tested shadow pass.");
        var foliage = new XElement(batched); foliage.SetAttributeValue("id", FoliageShadowGroup);
        foliage.Elements("SHADERGROUPPASS").Remove();
        foliage.Elements("SHADERINPUTDEFINITION").Last().AddAfterSelf(new XElement(objectCov.Elements("SHADERINPUTDEFINITION").First(d => (string?)d.Attribute("name") == "TDiffuseAlphaMap")));
        foliage.Elements("SHADERSTREAMDEFINITION").Last().AddAfterSelf(new XElement(objectCov.Elements("SHADERSTREAMDEFINITION").First(s => (string?)s.Attribute("renderTypeName") == "ST" && (string?)s.Attribute("name") == "TEXCOORD")));
        var foliagePass = new XElement(stockShadowPass); foliagePass.SetAttributeValue("vertexProgram", "#" + FoliageShadowProgram); foliage.Add(foliagePass);
        foliage.SetAttributeValue("parameterCount", foliage.Elements("SHADERINPUTDEFINITION").Count());
        foliage.SetAttributeValue("parameterStreamCount", foliage.Elements("SHADERSTREAMDEFINITION").Count()); foliage.SetAttributeValue("passCount", 1);
        batched.AddAfterSelf(foliage);
        // Caster variant of the cutout: the same passes plus that shadow pass. Foliage in
        // HIGHBATCH lists rasterised (46 ms GPU per VR frame) but cast nothing, consistent
        // with batch lists binding no material textures (alpha always 0, every texel
        // discarded). Regular HIGH draws bind their textures.
        var caster = new XElement(cutout); caster.SetAttributeValue("id", CasterGroup);
        var casterPass = new XElement(foliagePass); caster.Elements("SHADERGROUPPASS").Last().AddAfterSelf(casterPass);
        caster.SetAttributeValue("passCount", caster.Elements("SHADERGROUPPASS").Count());
        cutout.AddAfterSelf(caster);
        var vsCode = ShaderCompile.Compile(ShaderCompile.FoliageShadowVertex, "main", "vs_5_0"); DxbcPatch.Validate(vsCode, vertex: true);
        var vs = new XElement(candidate.Descendants("SHADERPROGRAM").Single(p => (string?)p.Attribute("id") == "batched_track.batched_track.cg.vp_shadow.13E6F496"));
        vs.SetAttributeValue("id", FoliageShadowProgram);
        var vsProgramCode = vs.Elements("SHADERPROGRAMCODE").Single();
        vsProgramCode.SetAttributeValue("codeSize", vsCode.Length); vsProgramCode.SetAttributeValue("streamCount", 2);
        vsProgramCode.Element("SHADERPROGRAMCODEBLOCK")!.Value = string.Join(" ", vsCode.Select(b => b.ToString("X2")));
        var positionStream = vsProgramCode.Elements("CGSTREAM").Single();
        var uvStream = new XElement(positionStream); uvStream.SetAttributeValue("cgStreamName", "TEXCOORD"); uvStream.SetAttributeValue("cgStreamRenderType", "ST");
        positionStream.AddAfterSelf(uvStream);
        newPrograms.Add(vs);

        var shadow = new Dictionary<string, List<object>>();
        foreach (var g in new[] { vc, ground, plainDecal, offsetDecal, cutout, layerCutout, caster })
            shadow[(string)g.Attribute("id")!] = ShadowMin(candidate, g, newPrograms);
        // The day colour passes the game draws (terrain 4359E7BF, decal 6AC11FEB) must be patched.
        foreach (var (g, program) in new[] { (vc, "g2vr_vc.terrain_simple.cg.fp.4359E7BF"), (ground, ShadowPrefix + "terrain_simple.cg.fp.4359E7BF"), (plainDecal, ShadowPrefix + "decal.cg.decal_ao_fp.6AC11FEB"), (cutout, ShadowPrefix + "decal.cg.decal_ao_fp.6AC11FEB") })
            if (!g.Elements("SHADERGROUPPASS").Any(p => (string?)p.Attribute("fragmentProgram") == "#" + program))
                throw new InvalidDataException($"{g.Attribute("id")} day colour pass lacks the shadow min program {program}.");
        var programAnchor = candidate.Descendants("SHADERPROGRAM").Last();
        foreach (var p in newPrograms) { programAnchor.AddAfterSelf(p); programAnchor = p; }

        string path = Path.Combine(overlay, Relative); Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        Files.Save(Files.FromDocument(candidate, originalFile), path);
        // Read back: the new groups and programs as written, and without them the stock pack unchanged.
        var saved = Files.Document(Files.Pssg(path));
        var savedCutout = GroupIn(saved, Group);
        if (!XNode.DeepEquals(cutout, savedCutout)) throw new InvalidDataException("Cutout shader state changed in serialization.");
        var savedDecal = GroupIn(saved, DecalGroup);
        if (!XNode.DeepEquals(offsetDecal, savedDecal)) throw new InvalidDataException("Offset decal state changed in serialization.");
        savedDecal.Remove();
        var savedLayer = GroupIn(saved, LayerCutoutGroup);
        if (!XNode.DeepEquals(layerCutout, savedLayer)) throw new InvalidDataException("Layer cutout state changed in serialization.");
        savedLayer.Remove();
        var savedVc = GroupIn(saved, TerrainGroup);
        if (!XNode.DeepEquals(vc, savedVc)) throw new InvalidDataException("Vertex-colour terrain group changed in serialization.");
        var savedFoliage = GroupIn(saved, FoliageShadowGroup);
        if (!XNode.DeepEquals(foliage, savedFoliage)) throw new InvalidDataException("Foliage shadow group changed in serialization.");
        savedFoliage.Remove();
        var savedCaster = GroupIn(saved, CasterGroup);
        if (!XNode.DeepEquals(caster, savedCaster)) throw new InvalidDataException("Cutout caster group changed in serialization.");
        savedCaster.Remove();
        foreach (var p in newPrograms)
        {
            var s = saved.Descendants("SHADERPROGRAM").Single(x => (string?)x.Attribute("id") == (string?)p.Attribute("id"));
            var a = Convert.FromHexString(string.Concat(p.Descendants("SHADERPROGRAMCODEBLOCK").Single().Value.Where(char.IsAsciiHexDigit)));
            var b = Convert.FromHexString(string.Concat(s.Descendants("SHADERPROGRAMCODEBLOCK").Single().Value.Where(char.IsAsciiHexDigit)));
            if (!a.AsSpan().SequenceEqual(b)) throw new InvalidDataException("Added shader program changed in serialization.");
            s.Remove();
        }
        foreach (var (g, what) in new[] { (ground, "Shadow-min terrain group"), (plainDecal, "Shadow-min decal group") })
        {
            var s = GroupIn(saved, (string)g.Attribute("id")!);
            if (!XNode.DeepEquals(g, s)) throw new InvalidDataException(what + " changed in serialization.");
            s.Remove();
        }
        savedCutout.Remove(); savedVc.Remove();
        if (!XNode.DeepEquals(original, saved)) throw new InvalidDataException("Stock shader programs or states changed.");
        if (Files.Hash(source) != inputs[source]) throw new InvalidDataException("Shaderpack changed while converting.");
        return new { Group, Base = "decal_ao_vc_flat", States = "object_2sidedcov cutout (alpha test, coverage, depth write), decal back-face culling",
            DecalGroup, LayerCutoutGroup, DecalOffset = new { DecalOffset.Factor, DecalOffset.Units }, CutoutOffset = new { CutoutOffset.Factor, CutoutOffset.Units },
            TerrainGroup, TerrainPasses = patched, GroundGroup, PlainDecalGroup, ShadowMin = shadow, FoliageShadowGroup, FoliageShadowVertexBytes = vsCode.Length, StockUnchanged = true, OriginalHash = inputs[source] };
    }
}
