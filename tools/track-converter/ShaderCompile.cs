using System.Runtime.InteropServices;
using System.Text;

namespace GRID2VR.Tracks;

// Build-time HLSL compilation (Windows d3dcompiler_47, as DiRT2VR's converters do) for
// the few small programs converted tracks add to their overlay shaderpack. Results are
// disassembled and must create on a WARP D3D11 device before they are written.
internal static partial class ShaderCompile
{
    [LibraryImport("d3dcompiler_47.dll", StringMarshalling = StringMarshalling.Utf8)]
    private static partial int D3DCompile(nint source, nuint size, string name, nint defines, nint include, string entry, string target, uint flags1, uint flags2, out nint code, out nint errors);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate nint GetPointer(nint self);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate nuint GetSize(nint self);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate uint Release(nint self);

    internal static byte[] Compile(string hlsl, string entry, string target)
    {
        var bytes = Encoding.UTF8.GetBytes(hlsl); var handle = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        try
        {
            // D3DCOMPILE_OPTIMIZATION_LEVEL3 (1 << 15).
            int hr = D3DCompile(handle.AddrOfPinnedObject(), (nuint)bytes.Length, "g2vr", 0, 0, entry, target, 1u << 15, 0, out var code, out var errors);
            string message = errors != 0 ? Read(errors, out _) : "";
            if (errors != 0) Free(errors);
            if (hr < 0 || code == 0) throw new InvalidDataException($"Shader compile failed (0x{hr:X8}): {message}");
            var text = Read(code, out var data); Free(code);
            return data;
        }
        finally { handle.Free(); }
    }

    static T Slot<T>(nint obj, int i) => Marshal.GetDelegateForFunctionPointer<T>(Marshal.ReadIntPtr(Marshal.ReadIntPtr(obj), i * nint.Size));
    static string Read(nint blob, out byte[] data)
    {
        var pointer = Slot<GetPointer>(blob, 3)(blob); var size = checked((int)Slot<GetSize>(blob, 4)(blob));
        data = new byte[size]; Marshal.Copy(pointer, data, 0, size);
        return Encoding.ASCII.GetString(data).TrimEnd('\0');
    }
    static void Free(nint blob) => Slot<Release>(blob, 2)(blob);

    // World-space foliage shadow caster: the stock batched_track shadow transform
    // (light viewProjection from CameraParamsConstantBuffer, cb3) plus the diffuse UV in
    // TEXCOORD1 for the stock alpha-tested CSM pixel shader (csm_depth_opaque.psh.mainAlphaTested).
    internal const string FoliageShadowVertex = """
        cbuffer CameraParamsConstantBuffer : register(b3)
        {
            float4x4 projection;
            float4x4 viewProjection;
            row_major float3x4 view;
            row_major float3x4 viewI;
            float3 eyePositionWS;
            float4x4 inverseProj;
            bool leftEye;
            bool padding3[3];
        };
        struct VSIn { float3 position : POSITION; float2 uv : TEXCOORD0; };
        struct VSOut { float4 position : SV_Position; float2 uv : TEXCOORD1; };
        VSOut main(VSIn input)
        {
            VSOut output;
            output.position = mul(viewProjection, float4(input.position, 1));
            output.uv = input.uv;
            return output;
        }
        """;
}
