using System.Buffers.Binary;
using System.Numerics;
using System.Xml.Linq;
using EgoEngineLibrary.Graphics.Pssg;
using EgoEngineLibrary.Graphics.Pssg.Elements;

namespace GRID2VR.Tracks;

internal readonly record struct Box(Vector3 Min, Vector3 Max)
{
    internal Box Union(Box other) => new(Vector3.Min(Min, other.Min), Vector3.Max(Max, other.Max));
    internal bool Contains(Box other, float tolerance = .05f) => Vector3.Max(other.Max - Max, Min - other.Min).Length() <= tolerance * 1.8f &&
        other.Min.X >= Min.X - tolerance && other.Min.Y >= Min.Y - tolerance && other.Min.Z >= Min.Z - tolerance &&
        other.Max.X <= Max.X + tolerance && other.Max.Y <= Max.Y + tolerance && other.Max.Z <= Max.Z + tolerance;
}

// GRID 2 track.vis (version 4, as DiRT 2) plus GRID 2's trailing bounds section,
// whose offset is header int 11 (byte 44). Layout learned from the stock files:
// header (128 B), view-tree records (6 B), visibility masks, spatial nodes (48 B
// + 32 B records: bounds, kind at +12, id at +28), then 32 B bounds records.
internal static class Visibility
{
    // Terrain tiles (top-level LAND_/ROOT_ nodes under "surface") in the order their
    // VIS identities are assigned. Verified against the donor: see CheckDonor.
    internal static PssgNode[] TerrainTiles(PssgFile tracksplit)
    {
        var surface = tracksplit.Elements<PssgNode>().Single(n => n.Id == "surface");
        return surface.ChildElements.OfType<PssgNode>()
            .Where(n => n.Id.StartsWith("ROOT_", StringComparison.Ordinal) || n.Id.StartsWith("LAND_", StringComparison.Ordinal))
            .Reverse().ToArray();
    }

    // GRID 2 leaves node bounding boxes at zero; use the drawn geometry instead.
    internal static Box Bounds(PssgNode node)
    {
        Box? box = null;
        foreach (var draw in node.Elements<PssgRenderStreamInstance>())
        {
            var reader = new EgoEngineLibrary.Formats.Pssg.RenderDataSourceReader(draw.GetRenderDataSource());
            for (int i = 0; i < reader.IndexCount; i++)
            {
                var p = reader.GetPosition(reader.GetIndex(i));
                box = box is { } b ? b.Union(new(p, p)) : new(p, p);
            }
        }
        return box ?? new(node.BoundingBox.BoundsMin, node.BoundingBox.BoundsMax);
    }

    static Box ReadBox(byte[] b, int p) => new(
        new(BitConverter.ToSingle(b, p), BitConverter.ToSingle(b, p + 4), BitConverter.ToSingle(b, p + 8)),
        new(BitConverter.ToSingle(b, p + 16), BitConverter.ToSingle(b, p + 20), BitConverter.ToSingle(b, p + 24)));

    internal static Dictionary<int, Box> TerrainRecords(byte[] b)
    {
        int I(int p) => BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(p, 4));
        if (I(0) != 4) throw new InvalidDataException("Unsupported VIS version.");
        var result = new Dictionary<int, Box>();
        int cursor = I(28);
        for (int n = 0; n < I(12); n++)
        {
            int count = (int)((uint)I(cursor + 28) >> 16);
            if ((I(cursor + 28) & 0xffff) != n) throw new InvalidDataException("VIS node sequence mismatch.");
            for (int k = 0; k < count; k++)
            {
                int r = cursor + 48 + k * 32;
                if (I(r + 12) == 0 && !result.TryAdd(I(r + 28), ReadBox(b, r))) throw new InvalidDataException("Duplicate VIS terrain id.");
            }
            cursor += 48 + count * 32;
        }
        if (cursor != I(44)) throw new InvalidDataException("VIS spatial section does not end at the bounds section.");
        return result;
    }

    // Confirms the identity rule on a stock venue: VIS terrain id i bounds tile i.
    internal static string CheckDonor(string tracksplitPath, string visPath)
    {
        var tiles = TerrainTiles(Files.Pssg(tracksplitPath));
        var vis = File.ReadAllBytes(visPath);
        var records = TerrainRecords(vis);
        // Every flagged leaf mask must expand to exactly header int 16 bytes.
        int V(int p) => BinaryPrimitives.ReadInt32LittleEndian(vis.AsSpan(p, 4));
        int leaves = 0, visibleBits = 0;
        for (int i = 0; i < V(4); i++)
        {
            int r = V(20) + i * 6;
            if (BinaryPrimitives.ReadUInt16LittleEndian(vis.AsSpan(r, 2)) != 0) continue;
            if (vis[r + 5] != 1) throw new InvalidDataException("Unexpected leaf mask flag " + vis[r + 5]);
            int at = vis[r + 2] | vis[r + 3] << 8 | vis[r + 4] << 16;
            visibleBits += Expand(vis.AsSpan(at), V(16)).Sum(b => System.Numerics.BitOperations.PopCount(b)); leaves++;
        }
        Console.WriteLine($"{leaves} leaf masks expand to {V(16)} bytes each; mean visible bits {visibleBits / Math.Max(1, leaves)}.");
        var boxes = tiles.Select(Bounds).ToArray();
        // Report the best geometric match of each VIS id, to learn the rule.
        foreach (var (id, box) in records.OrderBy(r => r.Key).Take(12))
        {
            int best = Enumerable.Range(0, tiles.Length).MinBy(i => Vector3.Distance(boxes[i].Min, box.Min) + Vector3.Distance(boxes[i].Max, box.Max));
            Console.WriteLine($"vis id {id}: best tile index {best} {tiles[best].Id} error {Vector3.Distance(boxes[best].Min, box.Min) + Vector3.Distance(boxes[best].Max, box.Max):0.00} m");
        }
        if (records.Count != tiles.Length || Enumerable.Range(0, tiles.Length).Any(i => !records.ContainsKey(i)))
            throw new InvalidDataException($"VIS has {records.Count} terrain ids for {tiles.Length} tiles.");
        tiles = tiles.ToArray();
        var mismatched = Enumerable.Range(0, tiles.Length).Where(i => !records[i].Contains(boxes[i], 1f) || !boxes[i].Contains(records[i], 1f)).ToArray();
        if (mismatched.Length > 0)
            throw new InvalidDataException("VIS/tile identity rule failed for ids " + string.Join(",", mismatched.Take(8)) +
                $" (e.g. id {mismatched[0]}: vis {records[mismatched[0]]} tile {tiles[mismatched[0]].Id} {boxes[mismatched[0]]})");
        return $"{tiles.Length} terrain tiles: VIS id i = tile i in reverse file order (first id 0 = {tiles[0].Id}).";
    }

    // Control experiment: a stock venue cloned under a new name with only track.vis
    // replaced by Write() for its own tiles. If its terrain renders, the synthesized
    // VIS is sound and any missing converted geometry has another cause.
    internal static void Control(string gameRoot, string output)
    {
        GRID2VR.GameExecutable.Validate(Files.Inside(gameRoot, "grid2_avx.exe"), avx: true);
        Files.NewOutput(output, gameRoot);
        string donor = Files.Inside(gameRoot, RouteProbe.DonorVenue), route = Files.Inside(donor, RouteProbe.DonorRoute);
        string venueOut = Path.Combine(output, "overlay", "tracks", "locations", "circuits", "g2vr_viscontrol"), routeOut = Path.Combine(venueOut, RouteProbe.DonorRoute);
        Directory.CreateDirectory(routeOut);
        foreach (var file in Directory.EnumerateFiles(donor)) { Files.NoLinks(file); File.Copy(file, Path.Combine(venueOut, Path.GetFileName(file))); }
        foreach (var file in Directory.EnumerateFiles(route)) { Files.NoLinks(file); File.Copy(file, Path.Combine(routeOut, Path.GetFileName(file))); }
        var tiles = TerrainTiles(Files.Pssg(Path.Combine(donor, "tracksplit.pssg"))).Select(Bounds).ToArray();
        var vis = Write(tiles, File.ReadAllBytes(Path.Combine(route, "track.vis")));
        File.Delete(Path.Combine(routeOut, "track.vis")); File.WriteAllBytes(Path.Combine(routeOut, "track.vis"), vis);
        new XDocument(new XElement("config", new XAttribute("cycle_camera", "false"), new XAttribute("infinite_loop", "false"), new XAttribute("skipreplays", "true"),
            new XAttribute("outputfilename", "G2VRTrack"),
            new XElement("track", new XAttribute("country", "circuits"), new XAttribute("name", "g2vr_viscontrol"), new XAttribute("route", RouteProbe.DonorRoute),
                new XAttribute("lighting", "day"), new XAttribute("weather", "dry"), new XAttribute("laps", 1), new XAttribute("number_cars", 1),
                new XElement("car", new XAttribute("name", "z28"), new XAttribute("livery", "user"), new XAttribute("ai_driver", "Cau_Demo_1_1"),
                    new XAttribute("ai_driver0", "Cau_Demo_1_0"), new XAttribute("grid", 0))))).Save(Path.Combine(output, "overlay", "G2VRTrack.xml"));
        new XDocument(new XElement("ModList", new XElement("Mod", new XAttribute("Path", Path.GetFullPath(Path.Combine(output, "overlay")))))).Save(Path.Combine(output, "modlist.xml"));
        Files.Json(Path.Combine(output, "build.json"), new { Schema = 1, Kind = "vis-control", Donor = $"{RouteProbe.DonorVenue}/{RouteProbe.DonorRoute}", Tiles = tiles.Length, VisBytes = vis.Length, RuntimeValidated = false });
        Console.WriteLine($"VIS control built ({tiles.Length} stock tiles, synthesized track.vis {vis.Length} bytes): {output}");
    }

    // Diagnostic, conservative VIS: one view leaf whose mask marks everything visible,
    // one spatial node listing every terrain tile, and the GRID 2 bounds section.
    // Donor object/tree inventories keep their mask bits (the mask is sized for them).
    internal static byte[] Write(Box[] tiles, byte[] donor)
    {
        if (tiles.Length is < 1 or > 65535) throw new InvalidDataException("Invalid VIS tile inventory.");
        int Donor(int p) => BinaryPrimitives.ReadInt32LittleEndian(donor.AsSpan(p, 4));
        const int view = 128, mask = 144;
        int objects = Enumerable.Range(0, 5).Sum(k => Donor(64 + k * 4));
        // GRID 2 leaves use flag 1: a run-length mask (0x80|n, b = n copies of b;
        // n, ... = n literal bytes) that expands to header int 16 bytes. Stock Brands
        // Hatch: ff ff x8, b0 ff, 01 03, 86 00, 01 00 = 1072 bytes.
        int expanded = checked(Math.Max(Donor(16), (objects + tiles.Length + 7) / 8) + 128 + 15) / 16 * 16;
        var compressed = new List<byte>();
        for (int left = expanded; left > 0; left -= Math.Min(127, left)) { compressed.Add((byte)(0x80 | Math.Min(127, left))); compressed.Add(0xff); }
        int maskBytes = (compressed.Count + 15) / 16 * 16;
        int spatial = mask + maskBytes, tail = checked(spatial + 48 + tiles.Length * 32);
        var bytes = new byte[tail + 32];
        void I(int p, int v) => BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(p, 4), v);
        void WriteBox(int p, Box b)
        {
            for (int k = 0; k < 3; k++)
            {
                BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(p + k * 4, 4), b.Min[k]);
                BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(p + 16 + k * 4, 4), b.Max[k]);
            }
        }
        var all = tiles.Aggregate((a, b) => a.Union(b));
        I(0, 4); I(4, 1); I(8, 1); I(12, 1); I(16, expanded); I(20, view); I(24, mask); I(28, spatial);
        WriteBox(32, all); I(44, tail); I(64, tiles.Length);
        // View leaf: type 0, 24-bit mask offset, flag 1 (run-length mask) as in every stock file.
        bytes[view + 2] = mask & 0xff; bytes[view + 3] = 0; bytes[view + 4] = 0; bytes[view + 5] = 1;
        compressed.ToArray().CopyTo(bytes.AsSpan(mask));
        if (Expand(bytes.AsSpan(mask, maskBytes), expanded).Any(b => b != 0xff)) throw new InvalidDataException("Visibility mask does not expand to all-visible.");
        WriteBox(spatial, all); I(spatial + 28, tiles.Length << 16); I(spatial + 32, 0);
        for (int i = 0; i < tiles.Length; i++) { int p = spatial + 48 + i * 32; WriteBox(p, tiles[i]); I(p + 12, 0); I(p + 28, i); }
        // GRID 2 bounds section: the stock minimal file has the whole-scene box only.
        for (int k = 0; k < 3; k++)
        {
            BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(tail + k * 4, 4), all.Min[k]);
            BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(tail + 12 + k * 4, 4), all.Max[k]);
        }
        var check = TerrainRecords(bytes);
        if (check.Count != tiles.Length) throw new InvalidDataException("Written VIS does not read back.");
        return bytes;
    }

    // Real visibility (Pvs): the conservative file's spatial section and mask size, with a view
    // tree over camera space. Format decoded from stock files (Brands Hatch route_2):
    //  - view records, 6 bytes. Internal: u16 a = right (upper) child | axis << 13; u16 c = left
    //    (lower) child (12 bits) | split bits 0-3 << 12; u16 d = split bits 4-19. The split is a
    //    20-bit fraction of the node's box along the axis; the root box is header bytes 32-56.
    //    Leaf: u16 0, 24-bit mask offset, flag 1 (run-length mask). Header int 4 = records,
    //    int 8 = leaves.
    //  - mask bits, LSB first, in spatial-section file order: one bit per spatial node, then one
    //    per record of that node (here: bit 0 = the single node, bit 1 + i = tile i). Stock leaves
    //    overlapping a tile have that bit set in 2118 of 2172 cases (the rest partial overlaps).
    // The tree wraps the eye samples (plus Margin); cameras elsewhere get an all-visible leaf.
    // Every bit other than invisible tiles stays set, as in the conservative file.
    internal static readonly Vector3 Margin = new(25, 6, 25);
    internal static (byte[] Bytes, object Report) WritePvs(Box[] tiles, byte[] donor, IReadOnlyList<Pvs.Cell> cells)
    {
        var conservative = Write(tiles, donor);
        int C(int p) => BinaryPrimitives.ReadInt32LittleEndian(conservative.AsSpan(p, 4));
        int expanded = C(16), spatialStart = C(28), spatialLength = conservative.Length - spatialStart, tailRelative = C(44) - spatialStart;
        if (cells.Any(c => c.Visible.Length != tiles.Length)) throw new InvalidDataException("Visibility sets do not cover every tile.");
        // Points with their cell, and the tree.
        var points = cells.SelectMany((c, i) => c.Eyes.Select(e => (P: e, Cell: i))).ToArray();
        var all = tiles.Aggregate((a, b) => a.Union(b));
        var eyeBox = new Box(points.Select(p => p.P).Aggregate(Vector3.Min) - Margin, points.Select(p => p.P).Aggregate(Vector3.Max) + Margin);
        var root = all.Union(eyeBox);
        var records = new List<(bool Leaf, int Axis, uint Fraction, int Left, int Right, HashSet<int>? Cells)>();
        // Siblings occupy consecutive records (left = lower half, right = left + 1), as in every
        // stock tree (root children 1/2, then 5/6, 3/4, 11/12, ...). Preorder numbering, which left
        // siblings apart, crashed GRID 2 while loading the track (game+0x957e74, run 20261113-131854-187).
        void Build(int index, Box box, (Vector3 P, int Cell)[] pts, int depth)
        {
            void Leaf(HashSet<int>? set) => records[index] = (true, 0, 0, 0, 0, set);
            if (pts.Length == 0) { Leaf(null); return; }
            var lo = Vector3.Max(box.Min, pts.Select(p => p.P).Aggregate(Vector3.Min) - Margin);
            var hi = Vector3.Min(box.Max, pts.Select(p => p.P).Aggregate(Vector3.Max) + Margin);
            // Carve empty space off first (largest gap), so leaves wrap the samples.
            int gapAxis = -1; bool gapLow = false; float gap = 40; // smaller empty slabs stay inside the leaf
            for (int k = 0; k < 3; k++)
            {
                if (lo[k] - box.Min[k] > gap) { gap = lo[k] - box.Min[k]; gapAxis = k; gapLow = true; }
                if (box.Max[k] - hi[k] > gap) { gap = box.Max[k] - hi[k]; gapAxis = k; gapLow = false; }
            }
            float split; int axis;
            if (gapAxis >= 0) { axis = gapAxis; split = gapLow ? lo[axis] : hi[axis]; }
            else
            {
                var distinct = pts.Select(p => p.Cell).Distinct().Count();
                var extent = hi - lo;
                // Up to three cells share a leaf (their sets merged: conservative, and the tree fits 12-bit indices).
                if (distinct <= 3 || depth > 40 || Math.Max(extent.X, extent.Z) < 120) { Leaf(pts.Select(p => p.Cell).ToHashSet()); return; }
                axis = extent.X >= extent.Z ? 0 : 2;
                var sorted = pts.Select(p => p.P[axis]).OrderBy(v => v).ToArray();
                split = sorted[sorted.Length / 2];
                if (split <= box.Min[axis] + .01f || split >= box.Max[axis] - .01f) split = (box.Min[axis] + box.Max[axis]) / 2;
            }
            uint fraction = (uint)Math.Clamp(MathF.Round((split - box.Min[axis]) / (box.Max[axis] - box.Min[axis]) * (1 << 20)), 1, (1 << 20) - 1);
            float at = box.Min[axis] + fraction / (float)(1 << 20) * (box.Max[axis] - box.Min[axis]); // as the engine reads it
            var lowMax = box.Max; lowMax[axis] = at; var highMin = box.Min; highMin[axis] = at;
            int left = records.Count, right = left + 1; records.Add(default); records.Add(default);
            if (right > 4095) throw new InvalidDataException("Visibility tree exceeds the 12-bit child index.");
            records[index] = (false, axis, fraction, left, right, null);
            Build(left, new(box.Min, lowMax), pts.Where(p => p.P[axis] < at).ToArray(), depth + 1);
            Build(right, new(highMin, box.Max), pts.Where(p => p.P[axis] >= at).ToArray(), depth + 1);
        }
        records.Add(default);
        Build(0, root, points, 0);
        // Masks: all bits set; invisible tiles cleared (tile i = bit 1 + i). Shared by content.
        byte[] Mask(HashSet<int>? set)
        {
            var mask = new byte[expanded]; Array.Fill(mask, (byte)0xff);
            if (set is null) return mask;
            for (int t = 0; t < tiles.Length; t++)
                if (!set.Any(c => cells[c].Visible[t])) mask[(1 + t) >> 3] &= (byte)~(1 << ((1 + t) & 7));
            return mask;
        }
        static byte[] Rle(byte[] data)
        {
            var output = new List<byte>(); int i = 0;
            while (i < data.Length)
            {
                int run = 1; while (i + run < data.Length && run < 127 && data[i + run] == data[i]) run++;
                if (run >= 3) { output.Add((byte)(0x80 | run)); output.Add(data[i]); i += run; continue; }
                int start = i, length = 0;
                while (i < data.Length && length < 127)
                {
                    int r = 1; while (i + r < data.Length && r < 3 && data[i + r] == data[i]) r++;
                    if (r >= 3) break;
                    i++; length++;
                }
                output.Add((byte)length); output.AddRange(data.AsSpan(start, length).ToArray());
            }
            return [.. output];
        }
        int viewStart = 128, maskStart = (viewStart + records.Count * 6 + 15) / 16 * 16;
        var maskSection = new List<byte>(); var offsets = new Dictionary<string, int>(); var leafMask = new Dictionary<int, byte[]>();
        int leaves = 0, allVisibleLeaves = 0;
        for (int r = 0; r < records.Count; r++)
        {
            if (!records[r].Leaf) continue;
            leaves++; if (records[r].Cells is null) allVisibleLeaves++;
            var mask = Mask(records[r].Cells); leafMask[r] = mask;
            // One mask per leaf, in record order, as in every stock file (no sharing).
            var key = r.ToString(System.Globalization.CultureInfo.InvariantCulture);
            while (maskSection.Count % 16 != 0) maskSection.Add(0); // each stock mask starts 16-byte aligned
            offsets[key] = maskStart + maskSection.Count; maskSection.AddRange(Rle(mask));
        }
        int spatial = (maskStart + maskSection.Count + 15) / 16 * 16;
        if (spatial > 0xffffff) throw new InvalidDataException("Visibility masks exceed the 24-bit offset.");
        var bytes = new byte[spatial + spatialLength];
        conservative.AsSpan(0, 128).CopyTo(bytes);
        conservative.AsSpan(spatialStart).CopyTo(bytes.AsSpan(spatial));
        void I(int p, int v) => BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(p, 4), v);
        I(4, records.Count); I(8, leaves); I(20, viewStart); I(24, maskStart); I(28, spatial); I(44, spatial + tailRelative);
        for (int k = 0; k < 3; k++) { BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(32 + k * 4), root.Min[k]); BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(48 + k * 4), root.Max[k]); }
        maskSection.ToArray().CopyTo(bytes, maskStart);
        for (int r = 0; r < records.Count; r++)
        {
            int p = viewStart + r * 6; var rec = records[r];
            if (rec.Leaf)
            {
                int offset = offsets[r.ToString(System.Globalization.CultureInfo.InvariantCulture)];
                bytes[p] = 0; bytes[p + 1] = 0; bytes[p + 2] = (byte)offset; bytes[p + 3] = (byte)(offset >> 8); bytes[p + 4] = (byte)(offset >> 16); bytes[p + 5] = 1;
            }
            else
            {
                BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(p), (ushort)(rec.Right | rec.Axis << 13));
                BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(p + 2), (ushort)(rec.Left | (int)(rec.Fraction & 0xf) << 12));
                BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(p + 4), (ushort)(rec.Fraction >> 4));
            }
        }
        // Read back: every eye lands in a leaf whose mask shows its cell's visible tiles.
        if (TerrainRecords(bytes).Count != tiles.Length) throw new InvalidDataException("PVS file does not read back.");
        int misses = 0;
        foreach (var (p, cell) in points)
        {
            var mask = Expand(bytes.AsSpan(Leaf(bytes, p)), expanded);
            for (int t = 0; t < tiles.Length; t++) if (cells[cell].Visible[t] && (mask[(1 + t) >> 3] >> ((1 + t) & 7) & 1) == 0) misses++;
        }
        if (misses > 0) throw new InvalidDataException($"PVS read-back: {misses} visible tiles missing from their eyes' leaves.");
        var visibleBits = cells.Select(c => c.Visible.Count(v => v)).ToArray();
        return (bytes, new { ViewRecords = records.Count, Leaves = leaves, AllVisibleLeaves = allVisibleLeaves, DistinctMasks = leafMask.Values.Select(Convert.ToBase64String).Distinct().Count(), MaskBytes = maskSection.Count, Bytes = bytes.Length,
            RootMin = new[] { root.Min.X, root.Min.Y, root.Min.Z }, RootMax = new[] { root.Max.X, root.Max.Y, root.Max.Z }, MarginMetres = new[] { Margin.X, Margin.Y, Margin.Z }, EyeReadBack = points.Length });
    }

    // The engine's lookup: descend the view tree to the leaf containing p; returns its mask offset.
    internal static int Leaf(byte[] b, Vector3 p)
    {
        int I(int o) => BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(o, 4));
        var lo = new Vector3(BitConverter.ToSingle(b, 32), BitConverter.ToSingle(b, 36), BitConverter.ToSingle(b, 40));
        var hi = new Vector3(BitConverter.ToSingle(b, 48), BitConverter.ToSingle(b, 52), BitConverter.ToSingle(b, 56));
        int node = 0;
        for (int guard = 0; guard < 4096; guard++)
        {
            int r = I(20) + node * 6; ushort a = BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(r)), c = BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(r + 2)), d = BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(r + 4));
            if (a == 0) return b[r + 2] | b[r + 3] << 8 | b[r + 4] << 16;
            int axis = (a >> 13) & 3; float s = lo[axis] + ((d << 4) | (c >> 12)) / (float)(1 << 20) * (hi[axis] - lo[axis]);
            if (p[axis] < s) { hi[axis] = s; node = c & 0xfff; } else { lo[axis] = s; node = a & 0x1fff; }
        }
        throw new InvalidDataException("Visibility tree does not terminate.");
    }

    // Run-length mask decoder (for checks, and for reading stock masks).
    internal static byte[] Expand(ReadOnlySpan<byte> stream, int length)
    {
        var result = new List<byte>(length); int p = 0;
        while (result.Count < length)
        {
            if (p >= stream.Length) throw new InvalidDataException("Truncated visibility mask.");
            int control = stream[p++];
            if ((control & 0x80) != 0) { if (p >= stream.Length) throw new InvalidDataException("Truncated mask run."); result.AddRange(Enumerable.Repeat(stream[p++], control & 0x7f)); }
            else { if (p + control > stream.Length) throw new InvalidDataException("Truncated mask literal."); result.AddRange(stream.Slice(p, control).ToArray()); p += control; }
        }
        if (result.Count != length) throw new InvalidDataException("Visibility mask overruns its length.");
        return [.. result];
    }
}
