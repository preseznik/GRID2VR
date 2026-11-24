using System.Numerics;

namespace GRID2VR.Tracks;

// Vertical probes and sight lines through the drawn scenery, in game coordinates. A point is
// inside the scenery when the first surface straight above it faces up (its vertex normals),
// i.e. it is seen from behind: under terrain or water, inside a hill or a building.
internal sealed class SceneryHeights
{
    const float Cell = 10;
    readonly List<(Vector3 A, Vector3 B, Vector3 C, float NormalY)> triangles = [];
    readonly Dictionary<(int, int), List<int>> cells = [];

    internal SceneryHeights(IEnumerable<Mesh> meshes, Vector3 translation = default)
    {
        foreach (var m in meshes)
            for (int i = 0; i + 2 < m.Indices.Length; i += 3)
            {
                int i0 = m.Indices[i], i1 = m.Indices[i + 1], i2 = m.Indices[i + 2];
                var a = m.Positions[i0] + translation; var b = m.Positions[i1] + translation; var c = m.Positions[i2] + translation;
                float normalY = m.Normals.Length > 0 ? (m.Normals[i0].Y + m.Normals[i1].Y + m.Normals[i2].Y) / 3 : 0;
                float minX = MathF.Min(a.X, MathF.Min(b.X, c.X)), maxX = MathF.Max(a.X, MathF.Max(b.X, c.X));
                float minZ = MathF.Min(a.Z, MathF.Min(b.Z, c.Z)), maxZ = MathF.Max(a.Z, MathF.Max(b.Z, c.Z));
                if (maxX - minX > 2000 || maxZ - minZ > 2000) continue;
                int index = triangles.Count; triangles.Add((a, b, c, normalY));
                for (int x = (int)MathF.Floor(minX / Cell); x <= (int)MathF.Floor(maxX / Cell); x++)
                    for (int z = (int)MathF.Floor(minZ / Cell); z <= (int)MathF.Floor(maxZ / Cell); z++)
                    {
                        if (!cells.TryGetValue((x, z), out var list)) cells[(x, z)] = list = [];
                        list.Add(index);
                    }
            }
    }

    internal bool Empty => triangles.Count == 0;

    // Inside: the nearest surface above faces up. Below: the highest surface under the point.
    internal (bool Inside, float? Below) At(Vector3 p)
    {
        if (!cells.TryGetValue(((int)MathF.Floor(p.X / Cell), (int)MathF.Floor(p.Z / Cell)), out var list)) return (false, null);
        float? below = null, above = null; float aboveNormal = 0;
        foreach (var index in list)
        {
            var (a, b, c, normalY) = triangles[index];
            float divisor = (b.Z - c.Z) * (a.X - c.X) + (c.X - b.X) * (a.Z - c.Z);
            if (MathF.Abs(divisor) < 1e-6f) continue;
            float u = ((b.Z - c.Z) * (p.X - c.X) + (c.X - b.X) * (p.Z - c.Z)) / divisor, v = ((c.Z - a.Z) * (p.X - c.X) + (a.X - c.X) * (p.Z - c.Z)) / divisor, w = 1 - u - v;
            if (MathF.Min(u, MathF.Min(v, w)) < -1e-5f) continue;
            float h = u * a.Y + v * b.Y + w * c.Y;
            if (h <= p.Y) { if (below is null || h > below) below = h; }
            else if (above is null || h < above) { above = h; aboveNormal = normalY; }
        }
        return (above is not null && aboveNormal > .2f, below);
    }

    // Whether any scenery triangle crosses the sight line from eye to target, leaving out the
    // last `spare` metres before the target (the ground and objects the shot is aimed at).
    internal bool Blocked(Vector3 eye, Vector3 target, float spare)
    {
        var direction = target - eye; float length = direction.Length();
        if (length <= spare) return false;
        float reach = (length - spare) / length;
        var seen = new HashSet<int>();
        int steps = (int)MathF.Ceiling(new Vector2(direction.X, direction.Z).Length() * reach / (Cell / 2)) + 1;
        for (int s = 0; s <= steps; s++)
        {
            var p = eye + direction * (reach * s / steps);
            for (int dx = -1; dx <= 1; dx++)
                for (int dz = -1; dz <= 1; dz++)
                    if (cells.TryGetValue(((int)MathF.Floor(p.X / Cell) + dx, (int)MathF.Floor(p.Z / Cell) + dz), out var list))
                        foreach (var index in list)
                            if (seen.Add(index) && Hit(triangles[index], eye, direction) is float t && t > 1e-4f && t < reach) return true;
        }
        return false;
    }

    // Möller-Trumbore, both faces: the segment parameter of the crossing.
    static float? Hit((Vector3 A, Vector3 B, Vector3 C, float NormalY) tri, Vector3 origin, Vector3 direction)
    {
        var e1 = tri.B - tri.A; var e2 = tri.C - tri.A;
        var p = Vector3.Cross(direction, e2); float det = Vector3.Dot(e1, p);
        if (MathF.Abs(det) < 1e-9f) return null;
        float inv = 1 / det; var s = origin - tri.A;
        float u = Vector3.Dot(s, p) * inv; if (u < 0 || u > 1) return null;
        var q = Vector3.Cross(s, e1);
        float v = Vector3.Dot(direction, q) * inv; if (v < 0 || u + v > 1) return null;
        return Vector3.Dot(e2, q) * inv;
    }
}
