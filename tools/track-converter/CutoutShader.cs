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
    internal const string Relative = "shaderpack/dx11/shaderpack.pssg";

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
        string path = Path.Combine(overlay, Relative); Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        Files.Save(Files.FromDocument(candidate, originalFile), path);
        // Read back: the new group as written, and without it the stock pack unchanged.
        var saved = Files.Document(Files.Pssg(path));
        var savedCutout = GroupIn(saved, Group);
        if (!XNode.DeepEquals(cutout, savedCutout)) throw new InvalidDataException("Cutout shader state changed in serialization.");
        savedCutout.Remove();
        if (!XNode.DeepEquals(original, saved)) throw new InvalidDataException("Stock shader programs or states changed.");
        if (Files.Hash(source) != inputs[source]) throw new InvalidDataException("Shaderpack changed while converting.");
        return new { Group, Base = "decal_ao_vc_flat", States = "object_2sidedcov cutout (alpha test, coverage, depth write), decal back-face culling", StockUnchanged = true, OriginalHash = inputs[source] };
    }
}
