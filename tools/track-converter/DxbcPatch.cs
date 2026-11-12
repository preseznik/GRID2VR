using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Text;

namespace GRID2VR.Tracks;

// Minimal SM5 DXBC edit: a pixel shader that sampled one texture (by reflection name)
// instead reads the same value from an interpolated input the vertex shader already
// writes but the stock pixel shader ignored. Every other token stays identical. The
// container checksum is recomputed (self-checked on the unmodified input first), and
// the result must disassemble and create on a WARP D3D11 device.
internal static partial class DxbcPatch
{
    internal sealed record Result(byte[] Code, int InputRegister, string Disassembly);

    internal static Result SampleToInput(byte[] dxbc, string texture, string semantic, int semanticIndex)
    {
        if (!Checksum(dxbc).AsSpan().SequenceEqual(dxbc.AsSpan(4, 16))) throw new InvalidDataException("DXBC checksum self-check failed on the stock shader.");
        var chunks = Chunks(dxbc);
        var rdef = chunks.Single(c => c.Tag == "RDEF").Data; var isgn = chunks.Single(c => c.Tag == "ISGN").Data;
        var shex = chunks.Single(c => c.Tag is "SHEX" or "SHDR");
        int textureSlot = Binding(rdef, texture, 2);
        int register = InputRegister(isgn, semantic, semanticIndex, out int maskOffset);

        var tokens = new uint[shex.Data.Length / 4];
        for (int i = 0; i < tokens.Length; i++) tokens[i] = BinaryPrimitives.ReadUInt32LittleEndian(shex.Data.AsSpan(i * 4));
        if (tokens[1] != tokens.Length) throw new InvalidDataException("Unexpected shader program length.");
        var output = new List<uint> { tokens[0], 0 };
        int replaced = 0; bool declared = false, inserted = false;
        for (int i = 2; i < tokens.Length;)
        {
            uint op = tokens[i]; uint opcode = op & 0x7ff;
            int length = opcode == 0x35 ? (int)tokens[i + 1] : (int)((op >> 24) & 0x7f);
            if (length <= 0 || i + length > tokens.Length) throw new InvalidDataException("Malformed shader instruction stream.");
            var instruction = tokens.AsSpan(i, length);
            if (opcode == 0x62 && OperandIndex(instruction, 1) == register) declared = true; // dcl_input_ps vN
            // Declare the input (xyzw) in register order, with the interpolation of the next
            // declared input (dcl_input_ps linear centroid vM).
            if (opcode == 0x62 && !inserted && length == 3 && OperandIndex(instruction, 1) > register)
            {
                output.Add(instruction[0]); output.Add((instruction[1] & ~0xFFu) | 2u | 0xF0u); output.Add((uint)register); inserted = true;
            }
            if ((opcode is 0x45 or 0x48 or 0x49 or 0x4a) && SampleResource(instruction, out int resourceSlot, out int dest, out int destLength, out int resource) && resourceSlot == textureSlot)
            {
                // mov dest, vN.<resource swizzle>
                uint swizzle = (instruction[resource] >> 4) & 0xff;
                uint src = 2u | (1u << 2) | (swizzle << 4) | (1u << 12) | (1u << 20);
                var mov = new List<uint> { 0 }; for (int k = 0; k < destLength; k++) mov.Add(instruction[dest + k]);
                mov.Add(src); mov.Add((uint)register);
                mov[0] = 0x36u | ((uint)mov.Count << 24);
                output.AddRange(mov); replaced++; i += length; continue;
            }
            output.AddRange(instruction.ToArray()); i += length;
        }
        if (replaced != 1) throw new InvalidDataException($"Expected one {texture} sample, found {replaced}.");
        if (declared) throw new InvalidDataException($"Input v{register} is already declared.");
        if (!inserted) throw new InvalidDataException("No dcl_input_ps to copy the interpolation from.");
        output[1] = (uint)output.Count;
        var newShex = new byte[output.Count * 4];
        for (int i = 0; i < output.Count; i++) BinaryPrimitives.WriteUInt32LittleEndian(newShex.AsSpan(i * 4), output[i]);
        var newIsgn = isgn.ToArray(); newIsgn[maskOffset + 1] = 0x0f; // the input is now read (rwMask)
        var rebuilt = Assemble(dxbc, chunks.Select(c => c.Tag == shex.Tag ? (c.Tag, newShex) : c.Tag == "ISGN" ? (c.Tag, newIsgn) : (c.Tag, c.Data)).ToList());
        var text = Disassemble.Run(rebuilt);
        Validate(rebuilt);
        return new(rebuilt, register, text);
    }

    // Returns operand offsets of a sample instruction: dest (with length) and resource.
    static bool SampleResource(ReadOnlySpan<uint> ins, out int slot, out int dest, out int destLength, out int resource)
    {
        slot = -1; dest = 0; destLength = 0; resource = 0;
        int p = 1; while ((ins[p - 1] >> 31) != 0 && p < ins.Length) p++; // extended opcode tokens
        var offsets = new List<(int At, int Length)>();
        while (p < ins.Length) { int n = OperandLength(ins, p); offsets.Add((p, n)); p += n; }
        if (offsets.Count < 4) return false;
        dest = offsets[0].At; destLength = offsets[0].Length; resource = offsets[2].At;
        if (((ins[resource] >> 12) & 0xff) != 7) return false; // RESOURCE
        slot = OperandIndex(ins, resource);
        return slot >= 0;
    }

    static int OperandIndex(ReadOnlySpan<uint> ins, int at)
    {
        uint tok = ins[at]; int p = at + 1 + (int)(tok >> 31);
        if (((tok >> 20) & 3) < 1 || ((tok >> 22) & 7) != 0) return -1;
        return (int)ins[p];
    }

    static int OperandLength(ReadOnlySpan<uint> t, int at)
    {
        uint tok = t[at]; int n = 1;
        if ((tok >> 31) != 0) n++;
        uint type = (tok >> 12) & 0xff, components = tok & 3;
        if (type == 4) return n + (components == 1 ? 1 : components == 2 ? 4 : 0);
        if (type == 5) return n + (components == 1 ? 2 : components == 2 ? 8 : 0);
        uint dims = (tok >> 20) & 3;
        for (int d = 0; d < dims; d++)
        {
            uint rep = (tok >> (22 + 3 * d)) & 7;
            n += rep switch { 0 => 1, 1 => 2, 2 => OperandLength(t, at + n), 3 => 1 + OperandLength(t, at + n + 1), 4 => 2 + OperandLength(t, at + n + 2), _ => throw new InvalidDataException("Unsupported operand index") };
        }
        return n;
    }

    // RDEF resource binding register by name and input type (2 = texture, 3 = sampler).
    static int Binding(byte[] rdef, string name, int type)
    {
        int count = BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(8)), offset = BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(12));
        for (int i = 0; i < count; i++)
        {
            var e = rdef.AsSpan(offset + i * 32);
            string n = CString(rdef, BinaryPrimitives.ReadInt32LittleEndian(e));
            if (n == name && BinaryPrimitives.ReadInt32LittleEndian(e[4..]) == type) return BinaryPrimitives.ReadInt32LittleEndian(e[20..]);
        }
        throw new InvalidDataException("Shader has no binding " + name);
    }

    static int InputRegister(byte[] isgn, string semantic, int index, out int maskOffset)
    {
        int count = BinaryPrimitives.ReadInt32LittleEndian(isgn);
        for (int i = 0; i < count; i++)
        {
            int e = 8 + i * 24;
            if (CString(isgn, BinaryPrimitives.ReadInt32LittleEndian(isgn.AsSpan(e))) == semantic && BinaryPrimitives.ReadInt32LittleEndian(isgn.AsSpan(e + 4)) == index)
            {
                maskOffset = e + 20; if ((isgn[maskOffset] & 0xf) != 0xf) throw new InvalidDataException("Input is not 4-component: " + semantic);
                return BinaryPrimitives.ReadInt32LittleEndian(isgn.AsSpan(e + 16));
            }
        }
        throw new InvalidDataException($"Shader input {semantic}{index} not in the signature.");
    }

    static string CString(byte[] data, int offset) { int end = Array.IndexOf(data, (byte)0, offset); return Encoding.ASCII.GetString(data, offset, end - offset); }

    static List<(string Tag, byte[] Data)> Chunks(byte[] dxbc)
    {
        if (Encoding.ASCII.GetString(dxbc, 0, 4) != "DXBC" || BinaryPrimitives.ReadInt32LittleEndian(dxbc.AsSpan(24)) != dxbc.Length) throw new InvalidDataException("Not a DXBC container.");
        int count = BinaryPrimitives.ReadInt32LittleEndian(dxbc.AsSpan(28)); var list = new List<(string, byte[])>();
        for (int i = 0; i < count; i++)
        {
            int at = BinaryPrimitives.ReadInt32LittleEndian(dxbc.AsSpan(32 + i * 4));
            int size = BinaryPrimitives.ReadInt32LittleEndian(dxbc.AsSpan(at + 4));
            list.Add((Encoding.ASCII.GetString(dxbc, at, 4), dxbc.AsSpan(at + 8, size).ToArray()));
        }
        return list;
    }

    static byte[] Assemble(byte[] original, List<(string Tag, byte[] Data)> chunks)
    {
        int header = 32 + chunks.Count * 4, size = header + chunks.Sum(c => 8 + c.Data.Length);
        var output = new byte[size]; original.AsSpan(0, 24).CopyTo(output);
        BinaryPrimitives.WriteInt32LittleEndian(output.AsSpan(24), size); BinaryPrimitives.WriteInt32LittleEndian(output.AsSpan(28), chunks.Count);
        int at = header;
        for (int i = 0; i < chunks.Count; i++)
        {
            BinaryPrimitives.WriteInt32LittleEndian(output.AsSpan(32 + i * 4), at);
            Encoding.ASCII.GetBytes(chunks[i].Tag).CopyTo(output, at); BinaryPrimitives.WriteInt32LittleEndian(output.AsSpan(at + 4), chunks[i].Data.Length);
            chunks[i].Data.CopyTo(output, at + 8); at += 8 + chunks[i].Data.Length;
        }
        Checksum(output).CopyTo(output, 4);
        return output;
    }

    // DXBC container hash: MD5 over everything after the checksum, with the length words
    // placed at the start and end of the final block instead of MD5's standard trailer.
    internal static byte[] Checksum(byte[] dxbc)
    {
        var data = dxbc.AsSpan(20); uint size = (uint)data.Length, bits = size * 8, tail = (bits >> 2) | 1;
        uint[] s = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
        int full = (int)(size / 64 * 64);
        for (int i = 0; i < full; i += 64) Md5Block(s, data.Slice(i, 64));
        int left = (int)(size - full); var block = new byte[64];
        if (left >= 56)
        {
            data[full..].CopyTo(block); block[left] = 0x80; Md5Block(s, block);
            block = new byte[64]; BinaryPrimitives.WriteUInt32LittleEndian(block, bits); BinaryPrimitives.WriteUInt32LittleEndian(block.AsSpan(60), tail); Md5Block(s, block);
        }
        else
        {
            BinaryPrimitives.WriteUInt32LittleEndian(block, bits); data[full..].CopyTo(block.AsSpan(4)); block[4 + left] = 0x80;
            BinaryPrimitives.WriteUInt32LittleEndian(block.AsSpan(60), tail); Md5Block(s, block);
        }
        var result = new byte[16]; for (int i = 0; i < 4; i++) BinaryPrimitives.WriteUInt32LittleEndian(result.AsSpan(i * 4), s[i]);
        return result;
    }

    static readonly int[] R = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
    static readonly uint[] K = Enumerable.Range(0, 64).Select(i => (uint)(long)Math.Floor(Math.Abs(Math.Sin(i + 1)) * 4294967296.0)).ToArray();
    static void Md5Block(uint[] s, ReadOnlySpan<byte> block)
    {
        Span<uint> m = stackalloc uint[16]; for (int i = 0; i < 16; i++) m[i] = BinaryPrimitives.ReadUInt32LittleEndian(block[(i * 4)..]);
        uint a = s[0], b = s[1], c = s[2], d = s[3];
        for (int i = 0; i < 64; i++)
        {
            uint f; int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = 7 * i % 16; }
            uint temp = d; d = c; c = b; b += uint.RotateLeft(a + f + K[i] + m[g], R[i]); a = temp;
        }
        s[0] += a; s[1] += b; s[2] += c; s[3] += d;
    }

    // Creating the shader on WARP runs the D3D11 runtime's own bytecode and hash checks.
    [LibraryImport("d3d11.dll")]
    private static partial int D3D11CreateDevice(nint adapter, int driverType, nint software, uint flags, nint levels, uint count, uint sdk, out nint device, out int level, out nint context);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate int CreatePixelShader(nint self, nint code, nuint length, nint linkage, out nint shader);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate uint ReleaseFn(nint self);
    // ID3D11Device vtable: CreateVertexShader = 12, CreatePixelShader = 15.
    internal static void Validate(byte[] code, bool vertex = false)
    {
        int hr = D3D11CreateDevice(0, 5 /* WARP */, 0, 0, 0, 0, 7, out var device, out _, out var context);
        if (hr < 0) throw new InvalidDataException($"No WARP device to validate the shader (0x{hr:X8}).");
        T Slot<T>(nint obj, int i) => Marshal.GetDelegateForFunctionPointer<T>(Marshal.ReadIntPtr(Marshal.ReadIntPtr(obj), i * nint.Size));
        var handle = GCHandle.Alloc(code, GCHandleType.Pinned);
        try
        {
            hr = Slot<CreatePixelShader>(device, vertex ? 12 : 15)(device, handle.AddrOfPinnedObject(), (nuint)code.Length, 0, out var shader);
            if (hr < 0 || shader == 0) throw new InvalidDataException($"Patched shader rejected by D3D11 (0x{hr:X8}).");
            Slot<ReleaseFn>(shader, 2)(shader);
        }
        finally { handle.Free(); Slot<ReleaseFn>(context, 2)(context); Slot<ReleaseFn>(device, 2)(device); }
    }
}
