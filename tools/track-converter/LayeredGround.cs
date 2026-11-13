using System.Globalization;
using System.Numerics;

namespace GRID2VR.Tracks;

// AC's layered ground (ksMultilayer_fresnel_nm grass): four detail maps in world XZ at
// multR/G/B/A, weighted per texel by txMask (on the mesh's own UVs), times the pale txDiffuse
// shading map and magicMult, plus a detail normal map (txDetailNM). Ported from DiRT2VR's
// Nordschleife HQ ground (tools/nordschleife-converter NordGround.hlsl and NordMaterials.cs,
// 45ce792, same author), including its reading of multA and detailNMMult as their vec2 values.
// The previous conversion drew only the grass layer (MultilayerMaterial), so the ground lacked
// the second grass, soil patches and relief (iztok, 0.2.23: "seems like it's lacking something").
internal sealed record LayeredGround(string[] Details, Vector2[] Scales, string Mask, string Normal, Vector2 NormalScale, float Magic)
{
    internal const string Shader = "ksMultilayer_fresnel_nm";

    internal static LayeredGround? Read(Material material)
    {
        if (material.Shader != Shader || material.Blend != 0 || material.AlphaTest) return null;        if (!material.Textures.TryGetValue("txDiffuse", out var shading) || !shading.StartsWith("grass", StringComparison.OrdinalIgnoreCase)) return null;
        var details = new[] { "R", "G", "B", "A" }.Select(c => material.Textures.GetValueOrDefault("txDetail" + c)).ToArray();
        if (details.Any(d => d is null) || !material.Textures.TryGetValue("txMask", out var mask) || !material.Textures.TryGetValue("txDetailNM", out var normal)) return null;
        float[] P(string name) => material.Properties.TryGetValue(name, out var v) && v.Length >= 3 && v.Take(3).All(float.IsFinite) ? v
            : throw new InvalidDataException($"Invalid {name} on layered ground {material.Id}");
        static void Check(float s, string what) { if (s < 0 || s > 100) throw new InvalidDataException("Layered ground scale out of range: " + what); }
        var scales = new[] { new Vector2(P("multR")[0]), new Vector2(P("multG")[0]), new Vector2(P("multB")[0]), new Vector2(P("multA")[1], P("multA")[2]) };
        foreach (var s in scales) { Check(s.X, material.Id); Check(s.Y, material.Id); }
        var nm = P("detailNMMult"); var normalScale = new Vector2(nm[1], nm[2]); Check(normalScale.X, material.Id); Check(normalScale.Y, material.Id);
        float magic = material.Properties.TryGetValue("magicMult", out var m) && m.Length > 0 && float.IsFinite(m[0]) && m[0] > 0 && m[0] <= 4 ? m[0] : 1;
        return new(details!, scales, mask, normal, normalScale, magic);
    }

    // Instance constants: layer mappings G/B/A (R stays the first UV set, TDiffuseSpecMap1), with
    // the source origin folded into the phase so world-space layers line up with the R layer.
    internal IEnumerable<(string Name, string Value)> Constants(Vector3 origin)
    {
        string F(float v) => v.ToString("0.#########", CultureInfo.InvariantCulture);
        string Map(Vector2 s) { float x = origin.X * s.X, z = origin.Z * s.Y; return $"{F(s.X)} {F(s.Y)} {F(x - MathF.Floor(x))} {F(z - MathF.Floor(z))}"; }
        yield return ("Map2UVScaleAndOffset", Map(Scales[1]));
        yield return ("Map3UVScaleAndOffset", Map(Scales[2]));
        yield return ("Map4UVScaleAndOffset", Map(Scales[3]));
        yield return ("DetailBlendScale", F(Magic));
        yield return ("BlendMaskSoftness", NormalScale == Vector2.Zero ? "0" : "1");
        yield return ("EdgeBlendMaskUScale", F(NormalScale.X));
        yield return ("EdgeBlendMaskVScale", F(NormalScale.Y));
    }

    // New texture inputs (texture and sampler slots past the stock terrain day passes' t0-t8/s0-s6).
    internal static readonly (string Name, int Texture, int Sampler)[] Textures =
        [("TDiffuseSpecMap2", 9, 7), ("TDiffuseSpecMap3", 10, 8), ("TDiffuseSpecMap4", 11, 9), ("TBlendMap2", 12, 10), ("TNormalMap2", 13, 11)];
    internal static readonly string[] ConstantNames = ["Map2UVScaleAndOffset", "Map3UVScaleAndOffset", "Map4UVScaleAndOffset", "DetailBlendScale", "BlendMaskSoftness", "EdgeBlendMaskUScale", "EdgeBlendMaskVScale"];

    // Prologue for DxbcPatch.PrependLayers. Input signature and cb0 offsets are those of the stock
    // terrain_simple day passes (FBF1B90E, 4359E7BF): v1 world normal, v2 first UV set, v3 world
    // position, v4.w/v5.w the colour-map (source) UVs. SV_Target0 = blended detail albedo (the
    // stock code multiplies the colour map and ColourMapScale in), SV_Target1 = world normal.
    internal static byte[] Compile(int diffuseTexture, int diffuseSampler) => ShaderCompile.Compile(Prologue
        .Replace("{T0}", diffuseTexture.ToString(CultureInfo.InvariantCulture)).Replace("{S0}", diffuseSampler.ToString(CultureInfo.InvariantCulture)), "main", "ps_5_0");

    const string Prologue = """
        cbuffer TerrainGlobals : register(b0)
        {
            float4 Map2UVScaleAndOffset : packoffset(c13);
            float4 Map3UVScaleAndOffset : packoffset(c14);
            float4 Map4UVScaleAndOffset : packoffset(c15);
            float DetailBlendScale : packoffset(c17.x);
            float BlendMaskSoftness : packoffset(c17.y);
            float EdgeBlendMaskUScale : packoffset(c20.x);
            float EdgeBlendMaskVScale : packoffset(c20.y);
        };
        Texture2D LayerR : register(t{T0}); SamplerState SamplerR : register(s{S0});
        Texture2D LayerG : register(t9); SamplerState SamplerG : register(s7);
        Texture2D LayerB : register(t10); SamplerState SamplerB : register(s8);
        Texture2D LayerA : register(t11); SamplerState SamplerA : register(s9);
        Texture2D Mask : register(t12); SamplerState SamplerMask : register(s10);
        Texture2D DetailNormal : register(t13); SamplerState SamplerNormal : register(s11);
        struct Pixel
        {
            float4 position : SV_Position;
            float3 normal : TEXCOORD1;
            float4 uv0 : TEXCOORD2;
            float4 world : TEXCOORD3;
            float4 uvU : TEXCOORD4;
            float4 uvV : TEXCOORD5;
            float4 colour : COLOR0;
            float4 fog : COLOR1;
        };
        struct Result { float4 albedo : SV_Target0; float4 normal : SV_Target1; };
        Result main(Pixel i)
        {
            float2 uv = float2(i.uvU.w, i.uvV.w);
            float3 p = i.world.xyz, n = i.normal;
            float4 w = Mask.Sample(SamplerMask, uv);
            float3 albedo = LayerR.Sample(SamplerR, i.uv0.xy).rgb * w.r
                + LayerG.Sample(SamplerG, p.xz * Map2UVScaleAndOffset.xy + Map2UVScaleAndOffset.zw).rgb * w.g
                + LayerB.Sample(SamplerB, p.xz * Map3UVScaleAndOffset.xy + Map3UVScaleAndOffset.zw).rgb * w.b
                + LayerA.Sample(SamplerA, p.xz * Map4UVScaleAndOffset.xy + Map4UVScaleAndOffset.zw).rgb * w.a;
            // Detail normal in the source UV frame, rebuilt from world/UV derivatives (the stock
            // terrain vertex program carries no tangents). Sampled before the guard so its
            // derivatives stay valid; collapsed UVs keep the mesh normal.
            float3 detail = DetailNormal.Sample(SamplerNormal, uv * float2(EdgeBlendMaskUScale, EdgeBlendMaskVScale)).rgb * 2 - 1;
            float3 px = ddx(p), py = ddy(p); float2 ux = ddx(uv), uy = ddy(uv);
            float orientation = ux.x * uy.y - ux.y * uy.x >= 0 ? 1 : -1;
            float3 nn = normalize(n);
            float3 tangent = (px * uy.y - py * ux.y) * orientation, bitangent = (py * ux.x - px * uy.x) * orientation;
            tangent -= nn * dot(nn, tangent); bitangent -= nn * dot(nn, bitangent);
            float t2 = dot(tangent, tangent), b2 = dot(bitangent, bitangent);
            if (BlendMaskSoftness > 0 && min(t2, b2) > 0)
                n = normalize(tangent * rsqrt(t2) * detail.x + bitangent * rsqrt(b2) * detail.y + nn * detail.z);
            Result r;
            r.albedo = float4(albedo * DetailBlendScale, 1);
            r.normal = float4(n, 0);
            return r;
        }
        """;
}
