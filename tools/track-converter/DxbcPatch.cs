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

    // Terrain shadow by distance (PerFrame shadowBlend, cb1[21]): stock terrain pixel shaders
    // compute w = sat(dist^2 * shadowBlend.x + shadowBlend.y) and then
    //     add a, a, -d      (a = baked sun visibility, d = dynamic shadow mask)
    //     mad a, w, a, d    (a = lerp(d, baked, w): dynamic only within ~33 m)
    // so converted scenery whose shadows exist only baked (foliage casts no dynamic shadow)
    // lost them near the car. This rewrites the pair to
    //     min a, a, d
    //     mov <mad's dest>, a
    // : both shadows at every distance (the dynamic mask is lit beyond its range).
    internal static Result ShadowMin(byte[] dxbc)
    {
        if (!Checksum(dxbc).AsSpan().SequenceEqual(dxbc.AsSpan(4, 16))) throw new InvalidDataException("DXBC checksum self-check failed on the stock shader.");
        var chunks = Chunks(dxbc);
        var shex = chunks.Single(c => c.Tag is "SHEX" or "SHDR");
        var tokens = new uint[shex.Data.Length / 4];
        for (int i = 0; i < tokens.Length; i++) tokens[i] = BinaryPrimitives.ReadUInt32LittleEndian(shex.Data.AsSpan(i * 4));
        if (tokens[1] != tokens.Length) throw new InvalidDataException("Unexpected shader program length.");
        var instructions = new List<(int At, int Length, uint Opcode)>();
        for (int i = 2; i < tokens.Length;)
        {
            uint op = tokens[i]; uint opcode = op & 0x7ff;
            int length = opcode == 0x35 ? (int)tokens[i + 1] : (int)((op >> 24) & 0x7f);
            if (length <= 0 || i + length > tokens.Length) throw new InvalidDataException("Malformed shader instruction stream.");
            instructions.Add((i, length, opcode)); i += length;
        }
        List<(int At, int Length)> Operands(int at, int length)
        {
            var ins = tokens.AsSpan(at, length); int p = 1; while ((ins[p - 1] >> 31) != 0 && p < ins.Length) p++;
            var list = new List<(int, int)>(); while (p < ins.Length) { int n = OperandLength(ins, p); list.Add((at + p, n)); p += n; }
            return list;
        }
        bool Cb(int at, int slot, int element, uint component)
        {
            uint tok = tokens[at]; if (((tok >> 12) & 0xff) != 8 || ((tok >> 20) & 3) != 2 || (tok >> 31) != 0) return false;
            if (((tok >> 2) & 3) != 2 || ((tok >> 4) & 3) != component) return false; // select_1 of that component
            return tokens[at + 1] == slot && tokens[at + 2] == element;
        }
        int Temp(int at) => ((tokens[at] >> 12) & 0xff) == 0 ? (int)tokens[at + 1 + (int)(tokens[at] >> 31)] : -1;
        var replacements = new Dictionary<int, uint[]>(); int sites = 0;
        for (int k = 0; k < instructions.Count; k++)
        {
            var (at, length, opcode) = instructions[k];
            if (opcode != 0x32 || ((tokens[at] >> 13) & 1) == 0) continue; // mad_sat
            var ops = Operands(at, length);
            if (ops.Count != 4 || !Cb(ops[2].At, 1, 21, 0) || !Cb(ops[3].At, 1, 21, 1)) continue;
            // The lerp follows within a few instructions: add a, a, -d ; mad a, w, a, d.
            for (int j = k + 1; j < Math.Min(k + 6, instructions.Count - 1); j++)
            {
                var add = instructions[j]; var mad = instructions[j + 1];
                if (add.Opcode != 0x00 || mad.Opcode != 0x32 || ((tokens[mad.At] >> 13) & 1) != 0) continue;
                var a = Operands(add.At, add.Length); var m = Operands(mad.At, mad.Length);
                if (a.Count != 3 || m.Count != 4) continue;
                int dest = Temp(a[0].At), src0 = Temp(a[1].At), neg = Temp(a[2].At);
                uint negTok = tokens[a[2].At];
                if (dest < 0 || src0 != dest || neg < 0 || (negTok >> 31) == 0 || (tokens[a[2].At + 1] & 0x3f) != 1 || ((tokens[a[2].At + 1] >> 6) & 0xff) != 1) continue;
                // The mad may write a or the weight register (decal mask-1 pass: mad w, w, a, d).
                if (Temp(m[0].At) < 0 || Temp(m[2].At) != dest || Temp(m[3].At) != neg) continue;
                // min dest, src0, d   (the negate's extended operand token dropped)
                var min = new List<uint> { 0 };
                for (int t = 0; t < a[0].Length; t++) min.Add(tokens[a[0].At + t]);
                for (int t = 0; t < a[1].Length; t++) min.Add(tokens[a[1].At + t]);
                min.Add(negTok & 0x7fffffffu); for (int t = 2; t < a[2].Length; t++) min.Add(tokens[a[2].At + t]);
                min[0] = 0x33u | ((uint)min.Count << 24);
                // mov dest, a  (the mad's own source a)
                var mov = new List<uint> { 0 };
                for (int t = 0; t < m[0].Length; t++) mov.Add(tokens[m[0].At + t]);
                for (int t = 0; t < m[2].Length; t++) mov.Add(tokens[m[2].At + t]);
                mov[0] = 0x36u | ((uint)mov.Count << 24);
                replacements[add.At] = min.ToArray(); replacements[mad.At] = mov.ToArray(); sites++;
                break;
            }
        }
        if (sites != 1) throw new InvalidDataException($"Expected one shadowBlend lerp, found {sites}.");
        var output = new List<uint> { tokens[0], 0 };
        foreach (var (at, length, _) in instructions)
            if (replacements.TryGetValue(at, out var with)) output.AddRange(with); else for (int t = 0; t < length; t++) output.Add(tokens[at + t]);
        output[1] = (uint)output.Count;
        var newShex = new byte[output.Count * 4];
        for (int i = 0; i < output.Count; i++) BinaryPrimitives.WriteUInt32LittleEndian(newShex.AsSpan(i * 4), output[i]);
        var rebuilt = Assemble(dxbc, chunks.Select(c => c.Tag == shex.Tag ? (c.Tag, newShex) : (c.Tag, c.Data)).ToList());
        var text = Disassemble.Run(rebuilt);
        Validate(rebuilt);
        return new(rebuilt, -1, text);
    }

    // Prepends a compiled prologue to a stock pixel shader and feeds its two results into the
    // stock code: SV_Target0.xyz replaces the albedo sampled from `diffuse`, SV_Target1.xyz
    // replaces every read of the interpolated world normal (TEXCOORD1). The stock lighting,
    // shadows, lights, fog and output encoding stay as they are. The prologue must use the
    // stock input signature (same registers), cb0 offsets of the stock $Globals, the stock
    // diffuse texture/sampler slots, and new textures at `slots` (texture tN with sampler sM,
    // names from `textures`). Its temps are renumbered past the stock ones. The stock RDEF is
    // kept and extended: new bindings (sampler and texture named alike, as EGO binds them) and
    // the USED flag on `constants`.
    internal static Result PrependLayers(byte[] dxbc, Func<int, int, byte[]> compile, string diffuse, IReadOnlyList<(string Name, int Texture, int Sampler)> textures, IReadOnlyList<string> constants)
    {
        if (!Checksum(dxbc).AsSpan().SequenceEqual(dxbc.AsSpan(4, 16))) throw new InvalidDataException("DXBC checksum self-check failed on the stock shader.");
        var chunks = Chunks(dxbc);
        var rdef = chunks.Single(c => c.Tag == "RDEF").Data; var isgn = chunks.Single(c => c.Tag == "ISGN").Data;
        var shex = chunks.Single(c => c.Tag is "SHEX" or "SHDR");
        int diffuseTexture = Binding(rdef, diffuse, 2), diffuseSampler = Binding(rdef, diffuse, 3);
        foreach (var (name, texture, sampler) in textures)
            if (Bindings(rdef).Any(b => b.Name == name || (b.Type == 3 ? b.Point == sampler : b.Type != 0 && b.Point <= texture && texture < b.Point + b.Count)))
                throw new InvalidDataException($"Stock shader already binds {name} or slot t{texture}/s{sampler}.");
        var prologue = compile(diffuseTexture, diffuseSampler);
        var pchunks = Chunks(prologue);
        if (!Signature(pchunks.Single(c => c.Tag == "ISGN").Data).SequenceEqual(Signature(isgn))) throw new InvalidDataException("Prologue input signature differs from the stock shader.");
        int normal = Signature(isgn).Single(e => e.Semantic == "TEXCOORD" && e.Index == 1).Register;

        uint[] Tokens(byte[] data) { var t = new uint[data.Length / 4]; for (int i = 0; i < t.Length; i++) t[i] = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(i * 4)); if (t[1] != t.Length) throw new InvalidDataException("Unexpected shader program length."); return t; }
        var stock = Tokens(shex.Data); var pro = Tokens(pchunks.Single(c => c.Tag is "SHEX" or "SHDR").Data);
        static bool Declaration(uint opcode) => opcode is >= 0x58 and <= 0x6a or >= 0x8f and <= 0xa2; // dcl_stream .. dcl_resource_structured
        var stockDecl = Instructions(stock).TakeWhile(i => Declaration(i.Opcode)).ToList();
        var stockBody = Instructions(stock).Skip(stockDecl.Count).ToList();
        var proDecl = Instructions(pro).TakeWhile(i => Declaration(i.Opcode)).ToList();
        var proBody = Instructions(pro).Skip(proDecl.Count).ToList();
        if (proBody.Count == 0 || proBody[^1].Opcode != 0x3e || proBody.Take(proBody.Count - 1).Any(i => i.Opcode is 0x3e or 0x3f or 0x0d or 0x04 or 0x05 or 0x2c))
            throw new InvalidDataException("Prologue must end in its only ret, with no discard, call or label.");
        int Temps(uint[] t, List<(int At, int Length, uint Opcode)> decl) => decl.Count(i => i.Opcode == 0x68) == 1 ? (int)t[decl.Single(i => i.Opcode == 0x68).At + 1]
            : throw new InvalidDataException("No single dcl_temps among declarations " + string.Join(",", decl.Select(i => i.Opcode.ToString("x"))) + " / first " + string.Join(",", Instructions(t).Take(30).Select(i => i.Opcode.ToString("x"))));
        int stockTemps = Temps(stock, stockDecl), proTemps = Temps(pro, proDecl);
        int albedoTemp = stockTemps + proTemps, normalTemp = albedoTemp + 1;

        // Prologue declarations: inputs must already be declared by the stock shader (same or
        // wider mask); new samplers/resources are added; cb0 must fit the stock declaration.
        var addSamplers = new List<uint[]>(); var addResources = new List<uint[]>();
        foreach (var d in proDecl)
        {
            var ins = pro.AsSpan(d.At, d.Length);
            switch (d.Opcode)
            {
                case 0x62 or 0x63 or 0x64: // dcl_input_ps*
                    {
                        int reg = OperandIndex(ins, 1); uint mask = (ins[1] >> 4) & 0xf;
                        uint have = 0; foreach (var s in stockDecl) if (s.Opcode is 0x62 or 0x63 or 0x64 && OperandIndex(stock.AsSpan(s.At, s.Length), 1) == reg) have |= (stock[s.At + 1] >> 4) & 0xf;
                        if ((mask & ~have) != 0) throw new InvalidDataException($"Prologue reads v{reg} components the stock shader does not declare.");
                        break;
                    }
                case 0x5a: if (OperandIndex(ins, 1) != diffuseSampler) addSamplers.Add(ins.ToArray()); break;
                case 0x58: if (OperandIndex(ins, 1) != diffuseTexture) addResources.Add(ins.ToArray()); break;
                case 0x59:
                    {
                        int slot = (int)ins[2], size = (int)ins[3];
                        uint have = 0; foreach (var s in stockDecl) if (s.Opcode == 0x59 && stock[s.At + 2] == slot) have = stock[s.At + 3];
                        if (slot != 0 || size > have) throw new InvalidDataException($"Prologue cb{slot}[{size}] exceeds the stock declaration.");
                        break;
                    }
                case 0x65 or 0x68 or 0x6a: break; // outputs become temps; temps and flags are the stock ones
                default: throw new InvalidDataException($"Unexpected prologue declaration 0x{d.Opcode:x}.");
            }
        }
        if (addSamplers.Count != textures.Count || addResources.Count != textures.Count) throw new InvalidDataException("Prologue does not use every new texture.");

        // Operand rewriting: walks every operand of an instruction (no relative indexing allowed
        // where a register is renumbered).
        void Rewrite(uint[] t, int at, int length, Func<uint, int, (uint Type, int Index)?> map)
        {
            int p = at + 1; while ((t[p - 1] >> 31) != 0 && p < at + length) p++;
            while (p < at + length)
            {
                int n = OperandLength(t, p); uint tok = t[p], type = (tok >> 12) & 0xff;
                if (type is 0 or 1 or 2)
                {
                    if (((tok >> 20) & 3) != 1 || ((tok >> 22) & 7) != 0) throw new InvalidDataException("Relative register index in a rewritten operand.");
                    int idx = p + 1 + (int)(tok >> 31);
                    if (map(type, (int)t[idx]) is { } to) { t[p] = (tok & ~(0xffu << 12)) | (to.Type << 12); t[idx] = (uint)to.Index; }
                }
                p += n;
            }
        }
        var body = new List<uint>();
        foreach (var i in proBody.Take(proBody.Count - 1))
        {
            var copy = pro.AsSpan(i.At, i.Length).ToArray();
            Rewrite(copy, 0, copy.Length, (type, index) => type switch
            {
                0 => (0u, stockTemps + index),
                2 when index is 0 or 1 => (0u, index == 0 ? albedoTemp : normalTemp),
                2 => throw new InvalidDataException("Prologue writes an unexpected output."),
                _ => null
            });
            body.AddRange(copy);
        }
        int albedoSites = 0, normalReads = 0;
        foreach (var i in stockBody)
        {
            var copy = stock.AsSpan(i.At, i.Length).ToArray();
            Rewrite(copy, 0, copy.Length, (type, index) => type == 1 && index == normal ? (normalReads++ >= 0 ? (0u, normalTemp) : null) : null);
            body.AddRange(copy);
            if ((i.Opcode is 0x45 or 0x48 or 0x49 or 0x4a) && SampleResource(stock.AsSpan(i.At, i.Length), out int slot, out int dest, out _, out _) && slot == diffuseTexture)
            {
                uint destTok = stock[i.At + dest];
                if (((destTok >> 12) & 0xff) != 0 || (destTok >> 31) != 0) throw new InvalidDataException("Albedo sample does not write a temp.");
                // mov rD.xyz, rAlbedo.xyzx
                body.AddRange([0x36u | (5u << 24), 0x00100072u, stock[i.At + dest + 1], 0x00100246u, (uint)albedoTemp]);
                albedoSites++;
            }
        }
        if (albedoSites != 1) throw new InvalidDataException($"Expected one {diffuse} sample, found {albedoSites}.");
        if (normalReads == 0) throw new InvalidDataException("Stock shader never reads the world normal.");

        var output = new List<uint> { stock[0], 0 };
        int lastSampler = stockDecl.FindLastIndex(d => d.Opcode == 0x5a), lastResource = stockDecl.FindLastIndex(d => d.Opcode == 0x58);
        for (int k = 0; k < stockDecl.Count; k++)
        {
            var d = stockDecl[k]; var ins = stock.AsSpan(d.At, d.Length).ToArray();
            if (d.Opcode == 0x68) ins[1] = (uint)(normalTemp + 1);
            output.AddRange(ins);
            if (k == lastSampler) foreach (var s in addSamplers) output.AddRange(s);
            if (k == lastResource) foreach (var r in addResources) output.AddRange(r);
        }
        output.AddRange(body);
        output[1] = (uint)output.Count;
        var newShex = new byte[output.Count * 4];
        for (int i = 0; i < output.Count; i++) BinaryPrimitives.WriteUInt32LittleEndian(newShex.AsSpan(i * 4), output[i]);
        var newRdef = ExtendRdef(rdef, diffuse, textures, constants);
        var rebuilt = Assemble(dxbc, chunks.Select(c => c.Tag == shex.Tag ? (c.Tag, newShex) : c.Tag == "RDEF" ? (c.Tag, newRdef) : (c.Tag, c.Data)).ToList());
        var text = Disassemble.Run(rebuilt);
        Validate(rebuilt);
        return new(rebuilt, normalReads, text);
    }

    static List<(int At, int Length, uint Opcode)> Instructions(uint[] tokens)
    {
        var list = new List<(int, int, uint)>();
        for (int i = 2; i < tokens.Length;)
        {
            uint op = tokens[i]; uint opcode = op & 0x7ff;
            int length = opcode == 0x35 ? (int)tokens[i + 1] : (int)((op >> 24) & 0x7f);
            if (length <= 0 || i + length > tokens.Length) throw new InvalidDataException("Malformed shader instruction stream.");
            list.Add((i, length, opcode)); i += length;
        }
        return list;
    }

    // Reflection order the PSSG program input definitions follow in every stock program:
    // $Globals variables by offset, then non-sampler resource bindings in table order.
    internal static (List<string> Globals, List<string> Resources) ReflectionOrder(byte[] dxbc)
    {
        var rdef = Chunks(dxbc).Single(c => c.Tag == "RDEF").Data;
        var globals = new List<string>();
        int cbCount = BinaryPrimitives.ReadInt32LittleEndian(rdef), cbOffset = BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(4));
        for (int c = 0; c < cbCount; c++)
        {
            int cb = cbOffset + c * 24;
            if (CString(rdef, BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(cb))) != "$Globals") continue;
            int vars = BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(cb + 4)), varOffset = BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(cb + 8));
            for (int v = 0; v < vars; v++) globals.Add(CString(rdef, BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(varOffset + v * 40))));
        }
        return (globals, Bindings(rdef).Where(b => b.Type is not (0 or 3)).Select(b => b.Name).ToList());
    }

    static List<(string Semantic, int Index, int Register)> Signature(byte[] sgn)
    {
        int count = BinaryPrimitives.ReadInt32LittleEndian(sgn); var list = new List<(string, int, int)>();
        for (int i = 0; i < count; i++)
        {
            int e = 8 + i * 24;
            list.Add((CString(sgn, BinaryPrimitives.ReadInt32LittleEndian(sgn.AsSpan(e))), BinaryPrimitives.ReadInt32LittleEndian(sgn.AsSpan(e + 4)), BinaryPrimitives.ReadInt32LittleEndian(sgn.AsSpan(e + 16))));
        }
        return list;
    }

    static List<(string Name, int Type, int Point, int Count, int Entry)> Bindings(byte[] rdef)
    {
        int count = BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(8)), offset = BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(12));
        return Enumerable.Range(0, count).Select(i => offset + i * 32).Select(e => (CString(rdef, BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(e))),
            BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(e + 4)), BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(e + 20)), BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(e + 24)), e)).ToList();
    }

    // Stock reflection plus new sampler/texture bindings (copies of the diffuse's entries; the
    // table and names are appended, so every stock offset stays valid) and USED flags.
    static byte[] ExtendRdef(byte[] rdef, string diffuse, IReadOnlyList<(string Name, int Texture, int Sampler)> textures, IReadOnlyList<string> constants)
    {
        if (BinaryPrimitives.ReadUInt16LittleEndian(rdef.AsSpan(16)) != 0x0500 || Encoding.ASCII.GetString(rdef, 28, 4) != "RD11") throw new InvalidDataException("Unexpected RDEF version.");
        var data = rdef.ToList(); while (data.Count % 4 != 0) data.Add(0);
        var stockEntries = Bindings(rdef);
        var samplerTemplate = stockEntries.Single(b => b.Name == diffuse && b.Type == 3); var textureTemplate = stockEntries.Single(b => b.Name == diffuse && b.Type == 2);
        var names = new Dictionary<string, int>();
        foreach (var (name, _, _) in textures) { names[name] = data.Count; data.AddRange(Encoding.ASCII.GetBytes(name)); data.Add(0); }
        while (data.Count % 4 != 0) data.Add(0);
        byte[] Entry(int template, int nameOffset, int point) { var e = rdef.AsSpan(template, 32).ToArray(); BinaryPrimitives.WriteInt32LittleEndian(e, nameOffset); BinaryPrimitives.WriteInt32LittleEndian(e.AsSpan(20), point); return e; }
        var table = new List<byte[]>();
        int lastSampler = stockEntries.FindLastIndex(b => b.Type == 3), lastTexture = stockEntries.FindLastIndex(b => b.Type == 2);
        for (int k = 0; k < stockEntries.Count; k++)
        {
            table.Add(rdef.AsSpan(stockEntries[k].Entry, 32).ToArray());
            if (k == lastSampler) foreach (var t in textures) table.Add(Entry(samplerTemplate.Entry, names[t.Name], t.Sampler));
            if (k == lastTexture) foreach (var t in textures) table.Add(Entry(textureTemplate.Entry, names[t.Name], t.Texture));
        }
        int tableOffset = data.Count; foreach (var e in table) data.AddRange(e);
        var result = data.ToArray();
        BinaryPrimitives.WriteInt32LittleEndian(result.AsSpan(8), table.Count); BinaryPrimitives.WriteInt32LittleEndian(result.AsSpan(12), tableOffset);
        // $Globals variables (cbuffer desc 24 bytes, variable desc 40 bytes for RD11).
        int cbCount = BinaryPrimitives.ReadInt32LittleEndian(rdef), cbOffset = BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(4));
        var pending = new HashSet<string>(constants);
        for (int c = 0; c < cbCount; c++)
        {
            int cb = cbOffset + c * 24;
            if (CString(rdef, BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(cb))) != "$Globals") continue;
            int vars = BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(cb + 4)), varOffset = BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(cb + 8));
            for (int v = 0; v < vars; v++)
            {
                int at = varOffset + v * 40;
                if (pending.Remove(CString(rdef, BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(at)))))
                    BinaryPrimitives.WriteInt32LittleEndian(result.AsSpan(at + 12), BinaryPrimitives.ReadInt32LittleEndian(rdef.AsSpan(at + 12)) | 2);
            }
        }
        if (pending.Count > 0) throw new InvalidDataException("Stock $Globals lacks " + string.Join(", ", pending));
        return result;
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

    // Diagnostic: D3DReflect (as the engine reads bindings) and, optionally, creation on the
    // hardware adapter. Returns one line per name: binding point, or the failing HRESULT.
    [LibraryImport("d3dcompiler_47.dll")]
    private static partial int D3DReflect(nint data, nuint size, in Guid iid, out nint reflector);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate int BindingByName(nint self, [MarshalAs(UnmanagedType.LPStr)] string name, nint desc);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate nint VariableByName(nint self, [MarshalAs(UnmanagedType.LPStr)] string name);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate int VariableDesc(nint self, nint desc);
    internal static IEnumerable<string> Reflect(byte[] code, IEnumerable<string> resources, IEnumerable<string> variables, bool hardware)
    {
        var lines = new List<string>();
        var handle = GCHandle.Alloc(code, GCHandleType.Pinned); var desc = Marshal.AllocHGlobal(256);
        try
        {
            T Slot<T>(nint obj, int i) => Marshal.GetDelegateForFunctionPointer<T>(Marshal.ReadIntPtr(Marshal.ReadIntPtr(obj), i * nint.Size));
            int hr = D3DReflect(handle.AddrOfPinnedObject(), (nuint)code.Length, new Guid("8d536ca1-0cca-4956-a837-786963755584"), out var reflector);
            lines.Add($"D3DReflect hr=0x{hr:X8}");
            if (hr >= 0)
            {
                foreach (var name in resources)
                {
                    int r = Slot<BindingByName>(reflector, 11)(reflector, name, desc);
                    lines.Add(r < 0 ? $"  {name}: hr=0x{r:X8}" : $"  {name}: type={Marshal.ReadInt32(desc, nint.Size)} point={Marshal.ReadInt32(desc, nint.Size + 4)}");
                }
                foreach (var name in variables)
                {
                    var v = Slot<VariableByName>(reflector, 10)(reflector, name);
                    int r = Marshal.GetDelegateForFunctionPointer<VariableDesc>(Marshal.ReadIntPtr(Marshal.ReadIntPtr(v), 0))(v, desc);
                    lines.Add(r < 0 ? $"  {name}: hr=0x{r:X8}" : $"  {name}: offset={Marshal.ReadInt32(desc, nint.Size)} size={Marshal.ReadInt32(desc, nint.Size + 4)} flags={Marshal.ReadInt32(desc, nint.Size + 8)}");
                }
                Slot<ReleaseFn>(reflector, 2)(reflector);
            }
            if (hardware)
            {
                hr = D3D11CreateDevice(0, 1 /* hardware */, 0, 0, 0, 0, 7, out var device, out _, out var context);
                if (hr < 0) { lines.Add($"hardware device hr=0x{hr:X8}"); return lines; }
                hr = Slot<CreatePixelShader>(device, 15)(device, handle.AddrOfPinnedObject(), (nuint)code.Length, 0, out var shader);
                lines.Add($"hardware CreatePixelShader hr=0x{hr:X8}");
                if (shader != 0) Slot<ReleaseFn>(shader, 2)(shader);
                Slot<ReleaseFn>(context, 2)(context); Slot<ReleaseFn>(device, 2)(device);
            }
        }
        finally { Marshal.FreeHGlobal(desc); handle.Free(); }
        return lines;
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
