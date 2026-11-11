using System.Runtime.InteropServices;

namespace GRID2VR.Tracks;

// Offline DXBC disassembly (Windows d3dcompiler_47) of shaders extracted from GRID 2's
// shaderpack, to read how a shader actually uses its inputs.
internal static partial class Disassemble
{
    [LibraryImport("d3dcompiler_47.dll")]
    private static partial int D3DDisassemble(nint data, nuint size, uint flags, nint comments, out nint blob);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate nint GetPointer(nint self);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate nuint GetSize(nint self);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate uint Release(nint self);

    internal static string Run(byte[] dxbc)
    {
        var handle = GCHandle.Alloc(dxbc, GCHandleType.Pinned);
        try
        {
            int hr = D3DDisassemble(handle.AddrOfPinnedObject(), (nuint)dxbc.Length, 0, 0, out var blob);
            if (hr < 0 || blob == 0) throw new InvalidDataException($"D3DDisassemble failed: 0x{hr:X8}");
            var vtable = Marshal.ReadIntPtr(blob);
            T Slot<T>(int i) => Marshal.GetDelegateForFunctionPointer<T>(Marshal.ReadIntPtr(vtable, i * nint.Size));
            try
            {
                var pointer = Slot<GetPointer>(3)(blob); var size = Slot<GetSize>(4)(blob);
                return Marshal.PtrToStringAnsi(pointer, checked((int)size)).TrimEnd('\0');
            }
            finally { Slot<Release>(2)(blob); }
        }
        finally { handle.Free(); }
    }
}
