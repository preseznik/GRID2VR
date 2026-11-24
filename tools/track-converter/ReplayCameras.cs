using System.Globalization;
using System.Numerics;
using System.Xml.Linq;
using EgoEngineLibrary.Formats.TrackQuadTree;
using EgoEngineLibrary.Graphics.Pssg.Elements;

namespace GRID2VR.Tracks;

// Pre-race intro and replay cameras. The route keeps the donor's replay_camera_config.xml,
// whose world-space cameras frame the donor's starting grid. The race intro cuts through
// them (reveal_*, transition_*, 12watch_*): on the converted Nordschleife it showed ~12 s of
// empty donor space (Brands Hatch boards, no ground) before reaching the grid (camera path
// of xr-sim run 20261112-114756-067, frames 1220-1970). One rigid transform - the yaw and
// offset that carry the donor's pole slot onto the converted pole slot - moves every
// spline path point, camera position/orientation and trigger zone so each shot frames the
// new grid as it framed the old one. Timing curves and the dynamic rig are unchanged.
internal static class ReplayCameras
{
    internal static object Write(string donorRoute, string routeOut, IReadOnlyList<QuadTreeDataTriangle> ground, SceneryHeights scenery)
    {
        var heights = new GroundIndex(ground);
        var (donorPole, donorForward) = Pole(Files.Pssg(Path.Combine(donorRoute, "grids.pssg")));
        var (pole, forward) = Pole(Files.Pssg(Path.Combine(routeOut, "grids.pssg")));
        // Yaw about +Y carrying the donor heading onto ours, verified on the vectors themselves.
        float angle = MathF.Atan2(donorForward.Z, donorForward.X) - MathF.Atan2(forward.Z, forward.X);
        var yaw = Quaternion.CreateFromAxisAngle(Vector3.UnitY, angle);
        if (Vector3.Distance(Vector3.Transform(donorForward, yaw), forward) > 1e-3f) { yaw = Quaternion.CreateFromAxisAngle(Vector3.UnitY, -angle); }
        if (Vector3.Distance(Vector3.Transform(donorForward, yaw), forward) > 1e-3f) throw new InvalidDataException("Replay camera heading transform failed.");
        Vector3 Move(Vector3 p) => Vector3.Transform(p - donorPole, yaw) + pole;

        string path = Path.Combine(routeOut, "replay_camera_config.xml");
        XDocument doc;
        using (var stream = File.OpenRead(path)) doc = XDocument.Parse(new EgoEngineLibrary.Xml.XmlFile(stream).Document.OuterXml);
        var root = doc.Root ?? throw new InvalidDataException("Empty replay camera configuration.");
        // Positional paths are type="spline" in route coordinates, despite local="true": the
        // reveal_lea_2 path starts exactly where the intro camera was logged, and the initial
        // camera's points sit around the donor pole. Percentage curves (linear/bezierPercentage)
        // are timing, not positions. Brands' file repeats some idents (camera_target_spline_r0_008).
        var allPaths = root.Elements("Path").ToList();
        var splines = allPaths.Where(p => (string?)p.Attribute("type") == "spline").ToList();
        var worldNames = splines.Select(p => (string)p.Attribute("ident")!).ToHashSet(StringComparer.Ordinal);
        bool World(string? ident) => ident is { Length: > 0 } && worldNames.Contains(ident);
        int points = 0, cameras = 0, zones = 0, lifted = 0, uncovered = 0;
        // Brands is flat; the Nordschleife start area is not. A moved spline whose lowest point
        // is under 2 m above the collision ground there is raised as a whole (shape kept);
        // its camera's Position follows. Points without collision beneath stay as moved.
        var lifts = new Dictionary<string, float>(StringComparer.Ordinal);
        foreach (var p in splines)
        {
            float lift = 0; bool any = false;
            foreach (var point in p.Elements("Point"))
            {
                var moved = Move(GetVector(point)); SetVector(point, moved); points++;
                if (heights.Highest(moved.X, moved.Z) is float h) { any = true; lift = MathF.Max(lift, h + MinimumClearance - moved.Y); }
            }
            if (!any) { uncovered++; continue; }
            if (lift <= 0) continue;
            foreach (var point in p.Elements("Point")) { var v = GetVector(point); v.Y += lift; SetVector(point, v); }
            lifts[(string)p.Attribute("ident")!] = MathF.Max(lifts.GetValueOrDefault((string)p.Attribute("ident")!), lift); lifted++;
        }
        foreach (var camera in root.Elements("Camera").Where(c => World((string?)c.Attribute("sourcePath"))))
        {
            Transform(camera); cameras++;
            if (lifts.TryGetValue((string)camera.Attribute("sourcePath")!, out var lift))
                foreach (var position in camera.Elements("Parameter").Where(e => (string?)e.Attribute("name") == "Position")) { var v = GetVector(position); v.Y += lift; SetVector(position, v); }
        }
        foreach (var zone in root.Elements("TriggerZone")) { Transform(zone, "position"); zones++; }
        var intro = Reanchor(root, splines, Slots(Files.Pssg(Path.Combine(routeOut, "grids.pssg"))), heights, scenery);

        void Transform(XElement element, string positionName = "Position")
        {
            foreach (var parameter in element.Elements("Parameter"))
            {
                string? name = (string?)parameter.Attribute("name");
                if (string.Equals(name, positionName, StringComparison.Ordinal) && (string?)parameter.Attribute("type") == "vector3") SetVector(parameter, Move(GetVector(parameter)));
                else if (name == "orientation" && (string?)parameter.Attribute("type") == "Quaternion")
                {
                    var q = new Quaternion(F(parameter, "x"), F(parameter, "y"), F(parameter, "z"), F(parameter, "w"));
                    var r = Quaternion.Normalize(yaw * q);
                    foreach (var (axis, value) in new[] { ("x", r.X), ("y", r.Y), ("z", r.Z), ("w", r.W) }) parameter.SetAttributeValue(axis, value.ToString("0.000000", CultureInfo.InvariantCulture));
                }
            }
        }
        File.Delete(path); Files.Xml(doc, path);
        // Read back: same structure, every moved value as written.
        XDocument saved;
        using (var stream = File.OpenRead(path)) saved = XDocument.Parse(new EgoEngineLibrary.Xml.XmlFile(stream).Document.OuterXml);
        if (saved.Descendants().Count() != doc.Descendants().Count() || saved.Descendants("Point").Count() != doc.Descendants("Point").Count())
            throw new InvalidDataException("Replay camera configuration changed in serialization.");
        return new
        {
            DonorPole = new[] { donorPole.X, donorPole.Y, donorPole.Z }, Pole = new[] { pole.X, pole.Y, pole.Z },
            YawDegrees = MathF.Round(2 * MathF.Acos(Math.Clamp(yaw.W, -1, 1)) * 180 / MathF.PI * MathF.Sign(yaw.Y), 2),
            WorldPathPoints = points, WorldCameras = cameras, TriggerZones = zones,
            SplinesLiftedAboveGround = lifted, SplinesWithoutCollisionBelow = uncovered, MinimumClearanceMetres = MinimumClearance,
            Intro = intro
        };
    }

    const float MinimumClearance = 2;

    // Race-intro shots (reveal_*, transition_*, grid_*, 12watch_* with a target). The rigid move
    // keeps them where they were relative to the donor's pole, which frames nothing here: on
    // Misty Loch they looked at hillsides 270-440 m from the grid, mostly from inside them
    // (iztok, 0.2.27 test 2026-11-24; camera-path.jsonl of run 20261124-180306-741). Each shot is
    // moved so its target is the middle of this grid, keeping its distance, height and
    // motion, then turned about the grid (0, ±20°, ... 180°, raised up to 30 m if needed) to the
    // first vantage where every camera point is outside the scenery, clear of the ground and
    // sees its target.
    static readonly string[] IntroPrefixes = ["reveal_", "transition_", "grid_", "12watch_"];

    static object Reanchor(XElement root, List<XElement> splines, IReadOnlyList<Vector3> slots, GroundIndex ground, SceneryHeights scenery)
    {
        var anchor = slots.Aggregate(Vector3.Zero, (sum, s) => sum + s) / slots.Count + Vector3.UnitY;
        var byIdent = splines.GroupBy(p => (string)p.Attribute("ident")!).ToDictionary(g => g.Key, g => g.ToList(), StringComparer.Ordinal);
        var cameras = root.Elements("Camera").ToList();
        // Paths any other camera also uses stay as they are.
        var users = cameras.SelectMany(c => new[] { (string?)c.Attribute("sourcePath"), (string?)c.Attribute("targetPath") }.Where(s => !string.IsNullOrEmpty(s)).Distinct().Select(s => s!))
            .GroupBy(s => s, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.Count(), StringComparer.Ordinal);
        var shots = new List<object>();
        foreach (var camera in cameras)
        {
            string ident = (string?)camera.Attribute("ident") ?? "";
            string? sourceIdent = (string?)camera.Attribute("sourcePath"), targetIdent = (string?)camera.Attribute("targetPath");
            if (!IntroPrefixes.Any(p => ident.StartsWith(p, StringComparison.Ordinal)) || string.IsNullOrEmpty(targetIdent) || string.IsNullOrEmpty(sourceIdent)) continue;
            if (!byIdent.TryGetValue(sourceIdent, out var sourcePaths) || !byIdent.TryGetValue(targetIdent, out var targetPaths)) continue;
            if (users[sourceIdent] > 1 || users[targetIdent] > 1) { shots.Add(new { Camera = ident, Kept = "path shared with another camera" }); continue; }
            var eye = sourcePaths.SelectMany(p => p.Elements("Point")).Select(GetVector).ToArray();
            var aim = targetPaths.SelectMany(p => p.Elements("Point")).Select(GetVector).ToArray();
            // The target's middle goes to the grid's middle, its lowest point to the grid's
            // height: crane shots whose target rises (to the sky) start on the cars.
            var centre = aim.Aggregate(Vector3.Zero, (sum, v) => sum + v) / aim.Length;
            var shift = new Vector3(anchor.X - centre.X, anchor.Y - aim.Min(v => v.Y), anchor.Z - centre.Z);
            // Shots that looked up at the donor's stands or trees would sit under this grid:
            // every camera point starts at least 1.5 m above the target.
            float floor = MathF.Max(0, anchor.Y + 1.5f - eye.Min(v => v.Y + shift.Y));
            Vector3 Place(Vector3 v, Quaternion turn, float lift) => Vector3.Transform(v + shift - anchor, turn) + anchor + (lift > 0 ? new Vector3(0, lift, 0) : Vector3.Zero);
            bool Clear(Quaternion turn, float lift)
            {
                for (int i = 0; i < eye.Length; i++)
                {
                    var p = Place(eye[i], turn, lift);
                    if (ground.Highest(p.X, p.Z) is float h && p.Y < h + MinimumClearance) return false;
                    if (scenery.Empty) continue;
                    var (inside, below) = scenery.At(p);
                    if (inside || (below is float b && p.Y < b + MinimumClearance)) return false;
                    var target = Place(aim[Math.Min(aim.Length - 1, i * aim.Length / eye.Length)], turn, 0);
                    if (scenery.Blocked(p, target, 6)) return false;
                }
                return true;
            }
            (float Degrees, float Lift)? found = null;
            foreach (var lift in new float[] { 0, 5, 10, 20, 30 }.Select(l => l + floor))
            {
                foreach (var step in Enumerable.Range(0, 19).Select(k => (k + 1) / 2 * 20f * (k % 2 == 0 ? 1 : -1)).Where(d => d > -180))
                    if (Clear(Quaternion.CreateFromAxisAngle(Vector3.UnitY, step * MathF.PI / 180), lift)) { found = (step, lift); break; }
                if (found is not null) break;
            }
            var (degrees, raise) = found ?? (0, floor);
            var turn = Quaternion.CreateFromAxisAngle(Vector3.UnitY, degrees * MathF.PI / 180);
            foreach (var point in sourcePaths.SelectMany(p => p.Elements("Point"))) SetVector(point, Place(GetVector(point), turn, raise));
            foreach (var point in targetPaths.SelectMany(p => p.Elements("Point"))) SetVector(point, Place(GetVector(point), turn, 0));
            foreach (var parameter in camera.Elements("Parameter"))
            {
                string? name = (string?)parameter.Attribute("name");
                if (name == "Position" && (string?)parameter.Attribute("type") == "vector3") SetVector(parameter, Place(GetVector(parameter), turn, raise));
                else if (name == "orientation" && (string?)parameter.Attribute("type") == "Quaternion")
                {
                    var r = Quaternion.Normalize(turn * new Quaternion(F(parameter, "x"), F(parameter, "y"), F(parameter, "z"), F(parameter, "w")));
                    foreach (var (axis, value) in new[] { ("x", r.X), ("y", r.Y), ("z", r.Z), ("w", r.W) }) parameter.SetAttributeValue(axis, value.ToString("0.000000", CultureInfo.InvariantCulture));
                }
            }
            var first = Place(eye[0], turn, raise);
            shots.Add(new
            {
                Camera = ident, Distance = MathF.Round(Vector3.Distance(eye[0], aim[0]), 1), TurnDegrees = degrees, RaisedMetres = raise,
                Clear = found is not null, Eye = new[] { MathF.Round(first.X, 1), MathF.Round(first.Y, 1), MathF.Round(first.Z, 1) }
            });
        }
        return new { Target = new[] { MathF.Round(anchor.X, 1), MathF.Round(anchor.Y, 1), MathF.Round(anchor.Z, 1) }, SceneryChecked = !scenery.Empty, Shots = shots };
    }

    // Every grid slot position (slot_00 ...).
    static List<Vector3> Slots(EgoEngineLibrary.Graphics.Pssg.PssgFile grid) =>
        [.. grid.Elements<PssgNode>().Where(n => n.Id?.StartsWith("slot_", StringComparison.Ordinal) == true).Select(n => new Vector3(n.Transform.Transform.M41, n.Transform.Transform.M42, n.Transform.Transform.M43))];

    // Highest collision surface under (x, z), via a 25 m cell index.
    sealed class GroundIndex
    {
        const float Cell = 25;
        readonly Dictionary<(int, int), List<QuadTreeDataTriangle>> cells = [];
        internal GroundIndex(IReadOnlyList<QuadTreeDataTriangle> triangles)
        {
            foreach (var t in triangles)
            {
                float minX = MathF.Min(t.Position0.X, MathF.Min(t.Position1.X, t.Position2.X)), maxX = MathF.Max(t.Position0.X, MathF.Max(t.Position1.X, t.Position2.X));
                float minZ = MathF.Min(t.Position0.Z, MathF.Min(t.Position1.Z, t.Position2.Z)), maxZ = MathF.Max(t.Position0.Z, MathF.Max(t.Position1.Z, t.Position2.Z));
                for (int x = (int)MathF.Floor(minX / Cell); x <= (int)MathF.Floor(maxX / Cell); x++)
                    for (int z = (int)MathF.Floor(minZ / Cell); z <= (int)MathF.Floor(maxZ / Cell); z++)
                    {
                        if (!cells.TryGetValue((x, z), out var list)) cells[(x, z)] = list = [];
                        list.Add(t);
                    }
            }
        }
        internal float? Highest(float px, float pz)
        {
            if (!cells.TryGetValue(((int)MathF.Floor(px / Cell), (int)MathF.Floor(pz / Cell)), out var list)) return null;
            float? best = null;
            foreach (var t in list)
            {
                var a = t.Position0; var b = t.Position1; var c = t.Position2;
                float divisor = (b.Z - c.Z) * (a.X - c.X) + (c.X - b.X) * (a.Z - c.Z);
                if (MathF.Abs(divisor) < 1e-6f) continue;
                float u = ((b.Z - c.Z) * (px - c.X) + (c.X - b.X) * (pz - c.Z)) / divisor, v = ((c.Z - a.Z) * (px - c.X) + (a.X - c.X) * (pz - c.Z)) / divisor, w = 1 - u - v;
                if (MathF.Min(u, MathF.Min(v, w)) < -1e-5f) continue;
                float h = u * a.Y + v * b.Y + w * c.Y; if (best is null || h > best) best = h;
            }
            return best;
        }
    }

    // Pole slot (slot_00) position and forward (the slot transform's third row is -forward).
    static (Vector3 Position, Vector3 Forward) Pole(EgoEngineLibrary.Graphics.Pssg.PssgFile grid)
    {
        var node = grid.Elements<PssgNode>().Single(n => n.Id == "slot_00");
        var m = node.Transform.Transform;
        var forward = Vector3.Normalize(new Vector3(-m.M31, 0, -m.M33));
        return (new Vector3(m.M41, m.M42, m.M43), forward);
    }

    static float F(XElement e, string axis) => float.Parse((string)e.Attribute(axis)!, CultureInfo.InvariantCulture);
    static Vector3 GetVector(XElement e) => new(F(e, "x"), F(e, "y"), F(e, "z"));
    static void SetVector(XElement e, Vector3 v)
    {
        e.SetAttributeValue("x", v.X.ToString("0.000000", CultureInfo.InvariantCulture));
        e.SetAttributeValue("y", v.Y.ToString("0.000000", CultureInfo.InvariantCulture));
        e.SetAttributeValue("z", v.Z.ToString("0.000000", CultureInfo.InvariantCulture));
    }
}
