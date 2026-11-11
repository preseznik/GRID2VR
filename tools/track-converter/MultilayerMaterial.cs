// AC multilayer terrain. Detail/scale choice adapted from DiRT2VR tools/nordschleife-
// converter GrassMaterial.cs and RoadMaterial.cs (a617f7a), same author.
using System.Numerics;

namespace GRID2VR.Tracks;

// AC's ksMultilayer diffuse is a pale shading map; the surface colour and grain come
// from detail layers sampled in source world XZ at the material's multR/multG scale.
// GRID 2's terrain_simple has one diffuse (first UV set, Map1UVScaleAndOffset) and a
// colour map on the second UV set, so: diffuse = detail on world-XZ UVs, colour map =
// shading map on the original UVs (multiplied in by the shader).
internal sealed record MultilayerMaterial(string Detail, string Shading, float Scale)
{
    internal static MultilayerMaterial? Read(Material material)
    {
        if (!material.Shader.StartsWith("ksMultilayer", StringComparison.Ordinal) || material.Blend != 0 || material.AlphaTest ||
            !material.Textures.TryGetValue("txDiffuse", out var shading)) return null;
        // Grass bases prefer a grass detail layer; everything else uses the R layer (asphalt grain).
        bool grass = shading.StartsWith("grass", StringComparison.OrdinalIgnoreCase);
        foreach (var channel in grass ? new[] { "R", "G" } : new[] { "R" })
        {
            if (!material.Textures.TryGetValue("txDetail" + channel, out var detail)) continue;
            if (grass && !detail.Contains("grass", StringComparison.OrdinalIgnoreCase)) continue;
            if (!material.Properties.TryGetValue("mult" + channel, out var property) || !float.IsFinite(property[0]) || property[0] < 0 || property[0] > 10)
                throw new InvalidDataException("Invalid multilayer detail scale: " + material.Id);
            // mult 0 (all ksMultilayer_objsp buildings, curb R = White1) or a flat white
            // layer: the source diffuse already carries the colour, keep it as is.
            if (property[0] == 0 || detail.StartsWith("White", StringComparison.OrdinalIgnoreCase)) return null;
            return new(detail, shading, property[0]);
        }
        return null;
    }

    // UV sets: [0] detail on world XZ (one integer phase removed per mesh, keeping
    // interpolation intact and half-float precision high), [1] same, [2] original UVs.
    internal Mesh Apply(Mesh mesh, Vector3 origin)
    {
        var anchor = mesh.Positions.Length > 0 ? mesh.Positions[0] : Vector3.Zero;
        var phase = new Vector2(anchor.X + origin.X, anchor.Z + origin.Z) * Scale;
        phase -= new Vector2(MathF.Floor(phase.X), MathF.Floor(phase.Y));
        var world = mesh.Positions.Select(p => new Vector2(p.X - anchor.X, p.Z - anchor.Z) * Scale + phase).ToArray();
        var minimum = mesh.UV.Length > 0 ? mesh.UV.Aggregate(Vector2.Min) : Vector2.Zero;
        var shift = new Vector2(MathF.Floor(minimum.X), MathF.Floor(minimum.Y));
        var original = mesh.UV.Select(uv => uv - shift).ToArray();
        return mesh with { TexCoords = [world, world, original] };
    }
}
