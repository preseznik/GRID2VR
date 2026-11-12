using System.Numerics;
using System.Xml.Linq;

namespace GRID2VR.Tracks;

// Baked sun visibility per vertex (1 = lit), cast toward GRID 2's own sun for the
// location. GRID 2 blends baked shadow (AO alpha) with its dynamic shadow mask by
// camera distance: dynamic only within ~33 m, baked only beyond ~80 m (PerFrame
// shadowBlend = (0.000189, -0.2107, 1, 0.7) read from the live terrain shader,
// run 20261112-082338-345). Occluders: opaque scenery, and alpha-tested foliage as
// partially transparent layers (no texture lookup). Grass tufts and blended
// decals do not occlude.
internal static class SunShadow
{
    // Toward-sun vector from lighting_day.xml rotationangle/heightangle (degrees).
    // Convention measured, not assumed: Brands Hatch day (32, 37) gives
    // (-0.4232, 0.6018, 0.6773), equal to the terrain shader's sunDirectionAndTime.
    internal static Vector3 Direction(float rotation, float height)
    {
        float r = rotation * MathF.PI / 180, h = height * MathF.PI / 180;
        return Vector3.Normalize(new(-MathF.Sin(r) * MathF.Cos(h), MathF.Sin(h), MathF.Cos(r) * MathF.Cos(h)));
    }

    internal static (Vector3 Direction, float Rotation, float Height) FromLighting(string lightingXml)
    {
        using var stream = File.OpenRead(lightingXml);
        var doc = XDocument.Parse(new EgoEngineLibrary.Xml.XmlFile(stream).Document.OuterXml);
        float Value(string name) => float.Parse(doc.Descendants("param").Attributes(name).Single().Value, System.Globalization.CultureInfo.InvariantCulture);
        float rotation = Value("rotationangle"), height = Value("heightangle");
        if (!float.IsFinite(rotation) || !float.IsFinite(height) || height is <= 0 or > 90) throw new InvalidDataException("Unsupported sun angles in " + lightingXml);
        return (Direction(rotation, height), rotation, height);
    }

    const float FoliageTransmission = .45f, MaxDistance = 1500, Offset = .05f;

    // Returns each mesh with Sun set; occluder kinds decided by the caller.
    internal static (List<Mesh> Meshes, object Report) Bake(IReadOnlyList<Mesh> meshes, Func<Mesh, int> occluder, Vector3 sun)
    {
        var watch = System.Diagnostics.Stopwatch.StartNew();
        var bvh = new Bvh(meshes, occluder);
        long lit = 0, shadowed = 0, vertices = 0;
        var result = new Mesh[meshes.Count];
        Parallel.For(0, meshes.Count, i =>
        {
            var mesh = meshes[i]; var sunVis = new float[mesh.Positions.Length]; long l = 0, s = 0;
            for (int v = 0; v < sunVis.Length; v++)
            {
                var n = mesh.Normals[v];
                // Faces turned away get N.L = 0 in the shader anyway; keep them lit here so
                // the blend toward dynamic shadow near the camera stays continuous.
                if (Vector3.Dot(n, sun) <= 0) { sunVis[v] = 1; continue; }
                var origin = mesh.Positions[v] + n * Offset + sun * Offset;
                sunVis[v] = bvh.Transmission(origin, sun, MaxDistance);
                if (sunVis[v] >= .999f) l++; else s++;
            }
            result[i] = mesh with { Sun = sunVis };
            Interlocked.Add(ref lit, l); Interlocked.Add(ref shadowed, s); Interlocked.Add(ref vertices, sunVis.Length);
        });
        return (result.ToList(), new
        {
            Sun = new[] { sun.X, sun.Y, sun.Z }, bvh.OpaqueTriangles, bvh.FoliageTriangles, FoliageTransmission, MaxDistance,
            Vertices = vertices, LitVertices = lit, ShadowedVertices = shadowed, Seconds = Math.Round(watch.Elapsed.TotalSeconds, 1)
        });
    }

    // Occluder kinds: 0 none, 1 opaque, 2 foliage (partial).
    sealed class Bvh
    {
        readonly Vector3[] a, e1, e2; readonly byte[] kind;
        readonly int[] order;
        readonly List<Node> nodes = [];
        record struct Node(Vector3 Min, Vector3 Max, int Left, int Right, int Start, int Count);
        internal int OpaqueTriangles { get; } internal int FoliageTriangles { get; }

        internal Bvh(IReadOnlyList<Mesh> meshes, Func<Mesh, int> occluder)
        {
            var tris = new List<(Vector3 A, Vector3 B, Vector3 C, byte K)>();
            foreach (var mesh in meshes)
            {
                int k = occluder(mesh); if (k == 0) continue;
                for (int i = 0; i + 2 < mesh.Indices.Length; i += 3)
                    tris.Add((mesh.Positions[mesh.Indices[i]], mesh.Positions[mesh.Indices[i + 1]], mesh.Positions[mesh.Indices[i + 2]], (byte)k));
                if (k == 1) OpaqueTriangles += mesh.Indices.Length / 3; else FoliageTriangles += mesh.Indices.Length / 3;
            }
            int n = tris.Count; a = new Vector3[n]; e1 = new Vector3[n]; e2 = new Vector3[n]; kind = new byte[n];
            var centre = new Vector3[n]; var lo = new Vector3[n]; var hi = new Vector3[n];
            for (int i = 0; i < n; i++)
            {
                var t = tris[i]; a[i] = t.A; e1[i] = t.B - t.A; e2[i] = t.C - t.A; kind[i] = t.K;
                lo[i] = Vector3.Min(t.A, Vector3.Min(t.B, t.C)); hi[i] = Vector3.Max(t.A, Vector3.Max(t.B, t.C)); centre[i] = (lo[i] + hi[i]) / 2;
            }
            order = Enumerable.Range(0, n).ToArray();
            Build(0, n, centre, lo, hi);
        }

        int Build(int start, int count, Vector3[] centre, Vector3[] lo, Vector3[] hi)
        {
            var min = new Vector3(float.MaxValue); var max = new Vector3(float.MinValue);
            for (int i = start; i < start + count; i++) { min = Vector3.Min(min, lo[order[i]]); max = Vector3.Max(max, hi[order[i]]); }
            int index = nodes.Count; nodes.Add(default);
            if (count <= 4) { nodes[index] = new(min, max, -1, -1, start, count); return index; }
            var cmin = new Vector3(float.MaxValue); var cmax = new Vector3(float.MinValue);
            for (int i = start; i < start + count; i++) { cmin = Vector3.Min(cmin, centre[order[i]]); cmax = Vector3.Max(cmax, centre[order[i]]); }
            var extent = cmax - cmin; int axis = extent.X > extent.Y ? (extent.X > extent.Z ? 0 : 2) : (extent.Y > extent.Z ? 1 : 2);
            if (extent[axis] <= 0) { nodes[index] = new(min, max, -1, -1, start, count); return index; }
            // Median split on the widest centroid axis (primitive key sort).
            var keys = new float[count]; var slice = new int[count];
            for (int i = 0; i < count; i++) { slice[i] = order[start + i]; keys[i] = centre[slice[i]][axis]; }
            Array.Sort(keys, slice); slice.CopyTo(order, start);
            int mid = start + count / 2;
            int left = Build(start, mid - start, centre, lo, hi);
            int right = Build(mid, start + count - mid, centre, lo, hi);
            nodes[index] = new(min, max, left, right, start, count);
            return index;
        }

        internal float Transmission(Vector3 origin, Vector3 dir, float maxT)
        {
            var inv = new Vector3(1 / dir.X, 1 / dir.Y, 1 / dir.Z);
            Span<int> stack = stackalloc int[96]; int sp = 0; stack[sp++] = 0; float t = 1;
            while (sp > 0)
            {
                var node = nodes[stack[--sp]];
                if (!HitBox(node.Min, node.Max, origin, inv, maxT)) continue;
                if (node.Left < 0)
                {
                    for (int i = node.Start; i < node.Start + node.Count; i++)
                    {
                        int tri = order[i];
                        if (!HitTriangle(tri, origin, dir, maxT)) continue;
                        if (kind[tri] == 1) return 0;
                        t *= FoliageTransmission; if (t < .05f) return 0;
                    }
                    continue;
                }
                if (sp + 2 > stack.Length) return t;
                stack[sp++] = node.Left; stack[sp++] = node.Right;
            }
            return t;
        }

        static bool HitBox(Vector3 min, Vector3 max, Vector3 o, Vector3 inv, float maxT)
        {
            var t0 = (min - o) * inv; var t1 = (max - o) * inv;
            var tmin = Vector3.Min(t0, t1); var tmax = Vector3.Max(t0, t1);
            float enter = MathF.Max(MathF.Max(tmin.X, tmin.Y), MathF.Max(tmin.Z, 0)), exit = MathF.Min(MathF.Min(tmax.X, tmax.Y), MathF.Min(tmax.Z, maxT));
            return enter <= exit;
        }

        bool HitTriangle(int i, Vector3 o, Vector3 d, float maxT)
        {
            var p = Vector3.Cross(d, e2[i]); float det = Vector3.Dot(e1[i], p);
            if (MathF.Abs(det) < 1e-9f) return false;
            float invDet = 1 / det; var s = o - a[i]; float u = Vector3.Dot(s, p) * invDet; if (u < 0 || u > 1) return false;
            var q = Vector3.Cross(s, e1[i]); float v = Vector3.Dot(d, q) * invDet; if (v < 0 || u + v > 1) return false;
            float t = Vector3.Dot(e2[i], q) * invDet; return t > 1e-4f && t < maxT;
        }
    }
}
