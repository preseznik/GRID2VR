using System.Numerics;

namespace GRID2VR.Tracks;

// Potentially visible sets for converted tracks. The previous track.vis marked every tile
// visible from everywhere; on the Nordschleife the scene is CPU-bound in VR (xr-sim 20261113-
// 130007-549: 11.1 ms/frame, ~7.8 ms submitting three views). Terrain hides most of the course:
// from eye points along the driving line, about a third of the tiles within the far plane are
// actually visible (pvs-estimate: 28.9 of 82.1 per 100 m).
//
// Per 100 m of track: eyes at several heights and lateral positions (past the edges too); a tile
// is visible if any eye reaches any of its sampled surface points past opaque geometry (cutouts,
// decals and grass are see-through, so foliage never hides anything). Conservative margins: the
// neighbouring cells' sets are merged in, and tiles within NearAlways of the cell are visible.
// Cameras outside the region these eyes cover fall into all-visible leaves (Visibility.WritePvs).
internal static class Pvs
{
    internal sealed record Cell(Vector3[] Eyes, bool[] Visible);

    const float CellLength = 100, Far = 3700, NearAlways = 400;
    const int TargetsPerTile = 192;
    static readonly float[] Heights = [.7f, 1.2f, 2.2f, 4f];
    static readonly float[] Lateral = [-1.15f, -.6f, 0, .6f, 1.15f];

    internal static (List<Cell> Cells, object Report) Compute(IReadOnlyList<List<Mesh>> parts, IReadOnlyList<Mesh> occluderMeshes, Func<Mesh, bool> opaque, Gate[] gates, int extraTiles)
    {
        var bvh = new SunShadow.Bvh(occluderMeshes, m => opaque(m) ? 1 : 0);
        var rng = new Random(20261113);
        var tiles = parts.Select(meshes =>
        {
            var faces = meshes.SelectMany(m => Enumerable.Range(0, m.Indices.Length / 3).Select(i => (m, i))).ToArray();
            // Area-weighted surface samples, lifted off their face so the face does not hide them.
            var area = new double[faces.Length]; double total = 0;
            for (int f = 0; f < faces.Length; f++)
            {
                var (m, i) = faces[f]; Vector3 a = m.Positions[m.Indices[i * 3]], b = m.Positions[m.Indices[i * 3 + 1]], c = m.Positions[m.Indices[i * 3 + 2]];
                total += Vector3.Cross(b - a, c - a).Length(); area[f] = total;
            }
            var targets = new Vector3[Math.Min(TargetsPerTile, faces.Length)];
            for (int k = 0; k < targets.Length; k++)
            {
                int f = rng.Next(faces.Length);
                if (total > 0) { int idx = Array.BinarySearch(area, rng.NextDouble() * total); f = Math.Min(faces.Length - 1, idx >= 0 ? idx : ~idx); }
                var (m, i) = faces[f]; Vector3 a = m.Positions[m.Indices[i * 3]], b = m.Positions[m.Indices[i * 3 + 1]], c = m.Positions[m.Indices[i * 3 + 2]];
                float u = rng.NextSingle(), v = rng.NextSingle(); if (u + v > 1) { u = 1 - u; v = 1 - v; }
                var n = Vector3.Cross(b - a, c - a); n = n.LengthSquared() > 0 ? Vector3.Normalize(n) : Vector3.UnitY;
                targets[k] = a + (b - a) * u + (c - a) * v + n * .3f;
            }
            var all = meshes.SelectMany(m => m.Positions);
            return (Targets: targets, Min: all.Aggregate(Vector3.Min), Max: all.Aggregate(Vector3.Max), Triangles: faces.Length);
        }).ToArray();

        var groups = gates.GroupBy(g => (int)(g.Distance / CellLength)).OrderBy(g => g.Key).Select(g => g.ToArray()).ToArray();
        var eyes = groups.Select(cell =>
        {
            var list = new List<Vector3>();
            for (int k = 0; k < cell.Length; k += 3)
            {
                var g = cell[k]; var side = Vector3.Cross(Vector3.UnitY, g.Tangent); side = side.LengthSquared() > 0 ? Vector3.Normalize(side) : Vector3.UnitX;
                foreach (float h in Heights) foreach (float s in Lateral) list.Add(g.Position + Vector3.UnitY * h + side * (s < 0 ? s * g.Left : s * g.Right));
            }
            return list.ToArray();
        }).ToArray();
        static float BoxDistance(Vector3 p, Vector3 lo, Vector3 hi) => Vector3.Distance(p, Vector3.Clamp(p, lo, hi));
        var raw = new bool[groups.Length][]; long rays = 0;
        Parallel.For(0, groups.Length, ci =>
        {
            var seen = new bool[tiles.Length]; long count = 0;
            for (int t = 0; t < tiles.Length; t++)
            {
                var tile = tiles[t]; float nearest = eyes[ci].Min(e => BoxDistance(e, tile.Min, tile.Max));
                if (nearest > Far) continue;
                if (nearest <= NearAlways) { seen[t] = true; continue; }
                foreach (var target in tile.Targets)
                {
                    foreach (var e in eyes[ci])
                    {
                        var d = target - e; float len = d.Length(); if (len > Far || len < 1e-3f) continue;
                        count++;
                        if (bvh.Transmission(e, d / len, len - .5f) > 0) { seen[t] = true; break; }
                    }
                    if (seen[t]) break;
                }
            }
            raw[ci] = seen; Interlocked.Add(ref rays, count);
        });
        // Neighbouring cells merged in (the course is a loop: first and last are neighbours).
        var cells = new List<Cell>();
        for (int ci = 0; ci < groups.Length; ci++)
        {
            var visible = new bool[tiles.Length + extraTiles];
            for (int d = -1; d <= 1; d++) { var r = raw[(ci + d + groups.Length) % groups.Length]; for (int t = 0; t < tiles.Length; t++) visible[t] |= r[t]; }
            for (int t = tiles.Length; t < visible.Length; t++) visible[t] = true; // kept donor tiles: always
            cells.Add(new(eyes[ci], visible));
        }
        var counts = cells.Select(c => c.Visible.Count(v => v)).ToArray();
        var inRange = cells.Select(c => tiles.Count(t => c.Eyes.Min(e => BoxDistance(e, t.Min, t.Max)) <= Far)).ToArray();
        return (cells, new { CellLengthMetres = CellLength, Cells = cells.Count, EyesPerCell = eyes.Max(e => e.Length), TargetsPerTile, FarMetres = Far, NearAlwaysMetres = NearAlways,
            OccluderTriangles = bvh.OpaqueTriangles, Rays = rays, VisibleTilesMean = counts.Average(), VisibleTilesMax = counts.Max(), InRangeTilesMean = inRange.Average(), Tiles = tiles.Length + extraTiles });
    }
}
