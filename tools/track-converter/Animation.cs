// Animated scenery for converted tracks, reconstructed from Custom Shaders Patch settings
// as DiRT2VR does for Misty Loch (tools/misty-converter: MistyFlowShader, MistyNativeMaterials
// Flow/Boat, same author). GRID 2 has no stock tracksplit shader that scrolls or floats, and
// its object_running_water/object_ripple groups have no stock instance to copy, so these are
// copies of the converter's own groups whose vertex shaders take a prologue
// (DxbcPatch.PrependVertex) driven by GRID 2's running clock (cb1[4].w, the stock water's):
//  - flow: blended decal (g2vr_decal_plain) with TEXCOORD0.y += frac(t x speed), one group
//    per speed (CSP stPerPixelNM_UVflow offsetDSpeed V);
//  - float: plain opaque ground (terrain_g2vr_simple; GRID 2 needs the terrain_ prefix) with
//    POSITION.y += Amplitude x sin(t x 2pi / Period), the same for every vertex (DiRT2VR's
//    10 cm rigid float). Floating meshes stay out of the static depth batches.
//  - spin: blended decal with POSITION rotated about each rotor disc's own hub and axis
//    (Rodrigues; the disc's normal is the axis, so normals stay valid), one group per disc.
//    CSP spins Misty's tail rotor as a U scroll on polar UVs: rate = -scroll / (dU/dangle).
using System.Globalization;
using System.Numerics;
using System.Xml.Linq;

namespace GRID2VR.Tracks;

// One animated group: its id, the converter group it copies ("decal" or "ground"), and its prologue.
internal sealed record AnimatedGroup(string Id, string Base, string Semantic, string Body, object Report);
// A rotor disc's spin, measured at build time.
internal sealed record Spin(Vector3 Hub, Vector3 Axis, float RadiansPerSecond, string Group, float Radius);
// Source: AC material name; CSP U scroll (UV/s) for polar-mapped discs, else a rate in rad/s
// (right-handed about the disc axis oriented upward).
internal sealed record SpinSource(string Material, float CspUPerSecond, float RadiansPerSecond);

internal static class Animation
{
    internal const string FloatGroup = "terrain_g2vr_float";
    internal const float Amplitude = .10f, Period = 5f;
    const string FlowPrefix = "g2vr_flow_", SpinPrefix = "g2vr_spin_";

    internal static string FlowGroup(float speed) => FlowPrefix + (speed < 0 ? "m" : "") + MathF.Abs(MathF.Round(speed * 1000)).ToString("0", CultureInfo.InvariantCulture);
    internal static bool IsFlow(string target) => target.StartsWith(FlowPrefix, StringComparison.Ordinal);
    internal static bool IsSpin(string target) => target.StartsWith(SpinPrefix, StringComparison.Ordinal);
    static string F(float v) => v.ToString("R", CultureInfo.InvariantCulture);
    static string F3(Vector3 v) => $"float3({F(v.X)}, {F(v.Y)}, {F(v.Z)})";

    // A rotor disc: hub (bounding-box centre), axis (area-weighted face normal, sign-folded so
    // two-sided copies add up), how flat it is along that axis, its radii, and how U and V
    // change with the angle around the hub (edge-wise least squares; R2 near 1 = polar mapping).
    internal sealed record Disc(float[] Hub, float[] Axis, float Flatness, float RadiusMin, float RadiusMax, float RimSpread, float UPerRadian, float UFit, float VPerRadian, float VFit, int Vertices, int Triangles)
    {
        internal Vector3 HubVector => new(Hub[0], Hub[1], Hub[2]);
        internal Vector3 AxisVector => new(Axis[0], Axis[1], Axis[2]);
    }
    internal static Disc Measure(Mesh mesh)
    {
        var p = mesh.Positions; var min = p.Aggregate(System.Numerics.Vector3.Min); var max = p.Aggregate(System.Numerics.Vector3.Max); var hub = (min + max) / 2;
        System.Numerics.Vector3 sum = default, reference = default;
        for (int i = 0; i + 2 < mesh.Indices.Length; i += 3)
        {
            var n = System.Numerics.Vector3.Cross(p[mesh.Indices[i + 1]] - p[mesh.Indices[i]], p[mesh.Indices[i + 2]] - p[mesh.Indices[i]]);
            if (reference == default && n.LengthSquared() > 1e-12f) reference = n;
            sum += System.Numerics.Vector3.Dot(n, reference) < 0 ? -n : n;
        }
        var axis = System.Numerics.Vector3.Normalize(sum);
        if (axis.Y < 0 || (axis.Y == 0 && axis.X + axis.Z < 0)) axis = -axis;
        var e1 = System.Numerics.Vector3.Normalize(System.Numerics.Vector3.Cross(axis, MathF.Abs(axis.Y) < .9f ? System.Numerics.Vector3.UnitY : System.Numerics.Vector3.UnitX)); var e2 = System.Numerics.Vector3.Cross(axis, e1);
        float Angle(System.Numerics.Vector3 q) { var r = q - hub; return MathF.Atan2(System.Numerics.Vector3.Dot(r, e2), System.Numerics.Vector3.Dot(r, e1)); }
        float flat = p.Max(q => MathF.Abs(System.Numerics.Vector3.Dot(q - hub, axis)));
        var radii = p.Select(q => { var r = q - hub; return (r - axis * System.Numerics.Vector3.Dot(r, axis)).Length(); }).ToArray();
        (float Slope, float Fit) Polar(Func<int, float> coordinate)
        {
            double dd = 0, dt = 0, tt = 0;
            var pairs = new List<(float D, float T)>();
            for (int i = 0; i + 2 < mesh.Indices.Length; i += 3)
                for (int k = 0; k < 3; k++)
                {
                    int a = mesh.Indices[i + k], b = mesh.Indices[i + (k + 1) % 3];
                    float t = Angle(p[b]) - Angle(p[a]); t = MathF.IEEERemainder(t, 2 * MathF.PI);
                    float d = coordinate(b) - coordinate(a);
                    if (MathF.Abs(t) < 1e-3f || radii[a] < 1e-3f || radii[b] < 1e-3f) continue;
                    pairs.Add((d, t)); dt += d * t; tt += t * t;
                }
            if (tt == 0) return (0, 0);
            float slope = (float)(dt / tt); double residual = 0;
            foreach (var (d, t) in pairs) { residual += (d - slope * t) * (d - slope * t); dd += d * d; }
            return (slope, dd == 0 ? 0 : (float)(1 - residual / dd));
        }
        var (u, uFit) = Polar(i => mesh.UV[i].X); var (v, vFit) = Polar(i => mesh.UV[i].Y);
        // Rim: relative spread of the outer vertices' radii (small = the hub is the disc centre).
        float rMax = radii.Max(); var rim = radii.Where(r => r > .9f * rMax).ToArray();
        return new([hub.X, hub.Y, hub.Z], [axis.X, axis.Y, axis.Z], flat, radii.Min(), rMax, (rim.Max() - rim.Min()) / rMax, u, uFit, v, vFit, p.Length, mesh.Indices.Length / 3);
    }

    // Marks source materials by definition (flows by AC material name, floats by source file,
    // spins per mesh of a rotor material) and returns the groups the shaderpack needs.
    internal static (List<AnimatedGroup> Groups, object Report) Mark(AcSource source, TrackDefinition definition)
    {
        int flow = 0, floating = 0; var spins = new List<object>(); var groups = new List<AnimatedGroup>();
        for (int i = 0; i < source.Visuals.Count; i++)
        {
            var mesh = source.Visuals[i]; var material = mesh.Material;
            if (definition.Flows.TryGetValue(material.Name, out var speed)) { source.Visuals[i] = mesh with { Material = material with { FlowV = speed } }; flow += mesh.Indices.Length / 3; }
            else if (definition.Spins.SingleOrDefault(s => s.Material == material.Name) is { } spinSource)
            {
                var disc = Measure(mesh);
                // A rotor must be a disc about its hub: flat to 25 cm, outer rim within 5% of its radius.
                if (disc.Flatness > .25f || disc.RimSpread > .05f || disc.RadiusMax is < .2f or > 15) throw new InvalidDataException($"{material.Name} is not a rotor disc: " + System.Text.Json.JsonSerializer.Serialize(disc));
                // Polar UVs: CSP's U scroll turns the pattern by -scroll/(dU/dangle) rad/s; the
                // geometry turns the same way. Otherwise the given rate.
                bool polar = disc.UFit > .99f && MathF.Abs(disc.UPerRadian) > 1e-3f;
                float rate = polar ? -spinSource.CspUPerSecond / disc.UPerRadian : spinSource.RadiansPerSecond;
                string group = SpinPrefix + groups.Count(g => IsSpin(g.Id));
                var spin = new Spin(disc.HubVector, Vector3.Normalize(disc.AxisVector), rate, group, MathF.Sqrt(disc.RadiusMax * disc.RadiusMax + disc.Flatness * disc.Flatness));
                source.Visuals[i] = mesh with { Material = material with { Id = material.Id + "~" + group, Spin = spin } };
                // Rodrigues about the hub; frac keeps the angle precise over long sessions.
                string body = $"float a = frac(t * {F(rate / (2 * MathF.PI))}) * 6.28318548; float s, c; sincos(a, s, c); float3 k = {F3(spin.Axis)}; " +
                    $"float3 r = v.xyz - {F3(spin.Hub)}; v.xyz = {F3(spin.Hub)} + r * c + cross(k, r) * s + k * dot(k, r) * (1 - c);";
                var report = new { Group = group, material.Name, Disc = disc, Polar = polar, CspUPerSecond = polar ? spinSource.CspUPerSecond : (float?)null, RadiansPerSecond = rate, RevolutionsPerSecond = rate / (2 * MathF.PI) };
                groups.Add(new(group, "decal", "POSITION", body, report)); spins.Add(report);
            }
            else if (definition.FloatingFiles.Any(f => material.Id.StartsWith(f + ":", StringComparison.OrdinalIgnoreCase)))
            { source.Visuals[i] = mesh with { Material = material with { Floating = true } }; floating += mesh.Indices.Length / 3; }
        }
        foreach (var name in definition.Flows.Keys.Concat(definition.Spins.Select(s => s.Material)))
            if (!source.Visuals.Any(m => m.Material.Name == name)) throw new InvalidDataException("Animated material missing: " + name);
        if (definition.FloatingFiles.Length > 0 && floating == 0) throw new InvalidDataException("Floating source meshes missing.");
        foreach (var speed in definition.Flows.Values.Distinct().Order())
            groups.Add(new(FlowGroup(speed), "decal", "TEXCOORD", $"v.y += frac(t * {F(speed)});", new { Group = FlowGroup(speed), SpeedV = speed }));
        if (floating > 0)
            groups.Add(new(FloatGroup, "ground", "POSITION", $"v.y += {F(Amplitude)} * sin(t * {F(2 * MathF.PI / Period)});", new { Group = FloatGroup, Amplitude, Period }));
        return (groups, new { FlowTriangles = flow, FloatingTriangles = floating, definition.Flows, definition.FloatingFiles, Spins = spins });
    }

    // Adds the animated groups and their vertex programs. `decal` and `ground` are the finished
    // converter groups (shadow min applied); their fragment programs are shared, not copied.
    internal static (List<XElement> Groups, object Report) AddGroups(XDocument candidate, XElement decal, XElement ground, IReadOnlyList<AnimatedGroup> specs, List<XElement> newPrograms)
    {
        var groups = new List<XElement>(); var report = new List<object>();
        foreach (var spec in specs)
        {
            var (group, passes) = Animate(candidate, spec.Base == "ground" ? ground : decal, spec.Id, spec.Semantic, spec.Body, newPrograms);
            groups.Add(group); report.Add(new { spec.Id, Source = spec.Report, Passes = passes });
        }
        var anchor = ground;
        foreach (var g in groups) { anchor.AddAfterSelf(g); anchor = g; }
        return (groups, report);
    }

    static (XElement Group, List<object> Passes) Animate(XDocument candidate, XElement template, string id, string semantic, string body, List<XElement> newPrograms)
    {
        var group = new XElement(template); group.SetAttributeValue("id", id);
        var passes = new List<object>();
        foreach (var pass in group.Elements("SHADERGROUPPASS"))
        {
            string mask = (string?)pass.Attribute("passConfigMaskLow") ?? "?";
            string programId = ((string)pass.Attribute("vertexProgram")!).TrimStart('#'), newId = id + "." + programId;
            if (newPrograms.Any(p => (string?)p.Attribute("id") == newId)) { pass.SetAttributeValue("vertexProgram", "#" + newId); passes.Add(new { Pass = mask, Program = programId, Patched = true, Shared = true }); continue; }
            var program = newPrograms.SingleOrDefault(p => (string?)p.Attribute("id") == programId) ?? candidate.Descendants("SHADERPROGRAM").Single(p => (string?)p.Attribute("id") == programId);
            var code = program.Descendants("SHADERPROGRAMCODEBLOCK").SingleOrDefault();
            if (code is null) { passes.Add(new { Pass = mask, Program = programId, Patched = false, Reason = "no bytecode" }); continue; }
            var bytes = Convert.FromHexString(string.Concat(code.Value.Where(char.IsAsciiHexDigit)));
            DxbcPatch.Result result;
            try { result = DxbcPatch.PrependVertex(bytes, semantic, 0, body); }
            catch (InvalidDataException e) when (e.Message.Contains("never reads") || e.Message.Contains("has no input"))
            { passes.Add(new { Pass = mask, Program = programId, Patched = false, Reason = e.Message }); continue; }
            var copy = new XElement(program); copy.SetAttributeValue("id", newId);
            copy.Descendants("SHADERPROGRAMCODE").Single().SetAttributeValue("codeSize", result.Code.Length);
            copy.Descendants("SHADERPROGRAMCODEBLOCK").Single().Value = string.Join(" ", result.Code.Select(b => b.ToString("X2")));
            newPrograms.Add(copy); pass.SetAttributeValue("vertexProgram", "#" + newId);
            passes.Add(new { Pass = mask, Program = programId, Patched = true, Bytes = result.Code.Length, Reads = result.InputRegister });
        }
        // The day colour passes (1 and 67108864) must move, or nothing visibly animates.
        foreach (var main in new[] { "1", "67108864" })
            if (!group.Elements("SHADERGROUPPASS").Any(p => (string?)p.Attribute("passConfigMaskLow") == main && ((string)p.Attribute("vertexProgram")!).StartsWith("#" + id + ".", StringComparison.Ordinal)))
                throw new InvalidDataException($"{id}: day pass {main} could not be animated.");
        return (group, passes);
    }
}
