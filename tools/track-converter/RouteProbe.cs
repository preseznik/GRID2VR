using System.Globalization;
using System.Numerics;
using System.Xml.Linq;
using EgoEngineLibrary.Archive.Jpk;
using EgoEngineLibrary.Formats.TrackQuadTree;
using EgoEngineLibrary.Formats.TrackQuadTree.Static;
using EgoEngineLibrary.Graphics.Pssg.Elements;
using EgoEngineLibrary.Xml;

namespace GRID2VR.Tracks;

// Gate 1a: an Assetto Corsa layout's collision, driving line, progress, grid and
// recovery planes on a stock GRID 2 donor venue, delivered as a -mod overlay.
// Donor visuals are left in place and the source is shifted away from them, so
// this proves physics/route/session loading only, not appearance.
internal static class RouteProbe
{
    internal const string DonorVenue = "tracks/locations/circuits/brands_hatch";
    internal const string DonorRoute = "route_2";
    internal const string Location = "g2vr_nordschleife";
    internal const string Route = "route_0";
    // Proven stock counts (Okutama: 906 progress / 963 AI gates); stay inside them.
    internal const int ProgressGateBudget = 900;
    internal const int AiGateBudget = 960;
    internal const int GridSlots = 20;

    static string F(float v) => v.ToString("0.###", CultureInfo.InvariantCulture);
    static XElement Vec(string name, Vector3 v) => new(name, new XAttribute("format", "float3"), $"{F(v.X)} {F(v.Y)} {F(v.Z)}");
    static Vector3 LeftOf(Vector3 tangent) => Vector3.Normalize(new Vector3(tangent.Z, 0, -tangent.X));

    internal static void Run(string acTrack, string layout, string gameRoot, string output, bool visuals = false, bool reverseTileIds = false)
    {
        string exe = Files.Inside(gameRoot, "grid2_avx.exe");
        // Same authentication as the launcher: exact known bytes or the Valve-signed Steam build.
        GRID2VR.GameExecutable.Validate(exe, avx: true);
        // The user's own copy, and only the version this conversion was validated on.
        SourcePins.Verify(acTrack, layout, Console.WriteLine);
        Files.NewOutput(output, acTrack, gameRoot);
        string donor = Files.Inside(gameRoot, DonorVenue), donorRoute = Files.Inside(donor, DonorRoute);
        string venueOut = Path.Combine(output, "overlay", "tracks", "locations", "circuits", Location), routeOut = Path.Combine(venueOut, Route);
        Directory.CreateDirectory(routeOut);
        var inputs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { [exe] = Files.Hash(exe) };
        foreach (var file in Directory.EnumerateFiles(donor)) { Files.NoLinks(file); inputs[file] = Files.Hash(file); File.Copy(file, Path.Combine(venueOut, Path.GetFileName(file))); }
        foreach (var file in Directory.EnumerateFiles(donorRoute)) { Files.NoLinks(file); inputs[file] = Files.Hash(file); File.Copy(file, Path.Combine(routeOut, Path.GetFileName(file))); }

        // Keep the course clear of the donor's own scenery and entities.
        float donorMaxX = DonorBounds(Path.Combine(donorRoute, "track.jpk")).Max.X;
        var source = AcSource.Read(acTrack, layout, (min, max) => new Vector3(min.X - (donorMaxX + 1500), min.Y - 20, min.Z), visuals);
        SourcePins.CheckInputs(acTrack, layout, source.Inputs);
        foreach (var (path, hash) in source.Inputs) inputs[path] = hash;
        Console.WriteLine($"Course {source.Length:N0} m, {source.Gates.Length} gates, {source.Collision.Count:N0} collision triangles" +
            (visuals ? $", {source.Visuals.Sum(m => m.Indices.Length / 3):N0} visual triangles, {source.Textures.Count} textures." : "."));

        WriteCollision(source, routeOut);
        var route = WriteRoute(source, donorRoute, routeOut);
        var grid = WriteGrid(source, donorRoute, routeOut);
        WriteRecovery(source, routeOut);
        object? visualReport = null;
        if (visuals)
        {
            Console.WriteLine("Converting scenery, materials and textures...");
            var (tiles, report) = TrackVisuals.Build(source, donor, venueOut, reverseTileIds: reverseTileIds);
            var vis = Visibility.Write(tiles, File.ReadAllBytes(Path.Combine(donorRoute, "track.vis")));
            File.WriteAllBytes(Replace(routeOut, "track.vis"), vis);
            visualReport = new { Scenery = report, CutoutShader = CutoutShader.Write(gameRoot, Path.Combine(output, "overlay"), inputs) };
            Console.WriteLine($"Visuals: {tiles.Length} tiles in tracksplit.pssg, conservative all-visible track.vis.");
        }
        WriteSession(output, layout);
        // Installed-pack manifest: tools/session_config.py lists this build as a track.
        Files.Json(Path.Combine(output, "custom-track.json"), new
        {
            schema = 1, tracks = new[] { new { id = $"circuits/{Location}/{Route}", label = "Nordschleife (Assetto Corsa)" } },
            source = $"{Path.GetFileName(Path.TrimEndingDirectorySeparator(acTrack))}/{layout}", kind = visuals ? "visual-probe" : "route-probe"
        });

        foreach (var (path, hash) in inputs) if (Files.Hash(path) != hash) throw new InvalidDataException("Source changed while converting: " + path);
        Files.Json(Path.Combine(output, "inputs.json"), inputs);
        Files.Json(Path.Combine(output, "collision-materials.json"), source.CollisionMappings);
        Files.Json(Path.Combine(output, "build.json"), new
        {
            Schema = 1, Kind = visuals ? "visual-probe" : "route-probe", Visuals = visualReport, SourceLayout = $"{Path.GetFileName(Path.TrimEndingDirectorySeparator(acTrack))}/{layout}",
            Donor = $"{DonorVenue}/{DonorRoute}", Location = $"circuits/{Location}/{Route}", LengthMetres = source.Length,
            Translation = new[] { -source.Origin.X, -source.Origin.Y, -source.Origin.Z }, Surfaces = source.Surfaces, Route = route, Grid = grid,
            DonorVisualsRetained = true, RuntimeValidated = false
        });
        Console.WriteLine("Route probe built; runtime validation pending: " + output);
    }

    static (Vector3 Min, Vector3 Max) DonorBounds(string jpkPath)
    {
        using var stream = File.OpenRead(jpkPath); var jpk = new JpkFile(); jpk.Read(stream);
        var ground = TrackGround.Load(jpk);
        return (ground.BoundsMin, ground.BoundsMax);
    }

    static void WriteCollision(AcSource source, string routeOut)
    {
        var builder = new QuadTreeMeshDataBuilder(VcQuadTreeTypeInfo.Get(VcQuadTreeType.DirtShowdown));
        foreach (var triangle in source.Collision) builder.Add(triangle);
        var ground = TrackGround.Create(builder.Build());
        string path = Path.Combine(routeOut, "track.jpk");
        File.Delete(path);
        using (var stream = new FileStream(path, FileMode.CreateNew)) ground.Save().Write(stream);
        using var input = File.OpenRead(path); var jpk = new JpkFile(); jpk.Read(input);
        var type = TrackGround.Identify(jpk);
        if (type.Type != VcQuadTreeType.DirtShowdown) throw new InvalidDataException("Collision did not read back as the GRID 2 encoding.");
        var readback = TrackGround.Load(jpk, type).TraverseGrid().SelectMany(c => c.QuadTree.GetTriangles()).ToArray();
        var missing = GeometryCoverage.UnmatchedList(source.Collision, readback);
        var extra = GeometryCoverage.UnmatchedList(readback, source.Collision);
        if (missing.Count != 0 || extra.Count != 0)
        {
            // Diagnose: nearest readback triangle (centroid) for a sample of unmatched source triangles.
            var centroids = readback.Select(t => ((t.Position0 + t.Position1 + t.Position2) / 3, t)).ToArray();
            static float Area(QuadTreeDataTriangle t) => Vector3.Cross(t.Position1 - t.Position0, t.Position2 - t.Position0).Length() / 2;
            Console.WriteLine($"missing by material: {string.Join(", ", missing.GroupBy(t => t.Material).Select(g => g.Key + "=" + g.Count()))}");
            Console.WriteLine($"missing area<1cm2: {missing.Count(t => Area(t) < 1e-4f)}, extra area<1cm2: {extra.Count(t => Area(t) < 1e-4f)}");
            foreach (var s in missing.Where((_, i) => i % Math.Max(1, missing.Count / 6) == 0).Take(6))
            {
                var c = (s.Position0 + s.Position1 + s.Position2) / 3;
                var (rc, r) = centroids.MinBy(x => Vector3.DistanceSquared(x.Item1, c));
                Console.WriteLine($"src {s.Material} {s.Position0} {s.Position1} {s.Position2} area={Area(s):0.######}\n rb {r.Material} {r.Position0} {r.Position1} {r.Position2} d={Vector3.Distance(rc, c):0.###}");
            }
            throw new InvalidDataException($"Collision coverage mismatch: missing {missing.Count}, extra {extra.Count}.");
        }
        Console.WriteLine($"Collision: {readback.Length:N0} triangles read back, 0 unmatched at 2 cm.");
    }

    static object WriteRoute(AcSource source, string donorRoute, string routeOut)
    {
        var gates = source.Gates;
        // Progress: circuit type, gate 0 on the start/finish line, three timed splits.
        var progressIndices = ProgressGates.Indices(gates, ProgressGateBudget,
            [.. new[] { .25f, .5f, .75f }.Select(f => Array.FindIndex(gates, g => g.Distance >= f * source.Length))]);
        var progress = new XElement("gates", new XAttribute("num_gates", progressIndices.Length));
        for (int i = 0; i < progressIndices.Length; i++)
        {
            var g = gates[progressIndices[i]]; var left = LeftOf(g.Tangent);
            var l = g.Position + left * (g.Left + 1); var r = g.Position - left * (g.Right + 1);
            progress.Add(new XElement("gate", new XAttribute("id", i), new XAttribute("distance", F(g.Distance + 1)), Vec("left", l), Vec("right", r), Vec("crossing", (l + r) / 2)));
        }
        var splits = new XElement("route", new XAttribute("id", 0), new XAttribute("direction", "forwards"), new XAttribute("num_splits", 4),
            new XElement("split", new XAttribute("id", 0), new XAttribute("type", "finish"), new XAttribute("gate", 0)));
        foreach (var f in new[] { .25f, .5f, .75f })
            splits.Add(new XElement("split", new XAttribute("id", splits.Elements().Count()), new XAttribute("type", "time"),
                new XAttribute("gate", Array.FindIndex(progressIndices, i => gates[i].Distance >= f * source.Length))));
        var all = source.Collision.SelectMany(t => new[] { t.Position0, t.Position1, t.Position2 });
        var min = all.Aggregate(Vector3.Min); var max = all.Aggregate(Vector3.Max);
        Files.Xml(new XDocument(new XElement("progress_track_data", new XAttribute("exporter_version", "3.0.0"),
            new XElement("routes", new XAttribute("num_routes", 1), splits), progress,
            new XElement("track", new XAttribute("type", "circuit"), new XAttribute("total_distance", F(source.Length)),
                new XElement("track_bounds", Vec("min", min), Vec("max", max))))), Replace(routeOut, "progress_track.xml"));

        // AI line: gate position on the left track limit, normal towards the right (stock layout).
        var aiIndices = ProgressGates.Indices(gates, AiGateBudget);
        var aiGates = new XElement("gates", new XAttribute("num_gates", aiIndices.Length));
        var links = new XElement("links", new XAttribute("num_links", aiIndices.Length));
        for (int i = 0; i < aiIndices.Length; i++)
        {
            var g = gates[aiIndices[i]]; var left = LeftOf(g.Tangent); float width = g.Left + g.Right;
            XElement Waypoint(int id, string type, float at) => new("waypoint", new XAttribute("id", id), new XAttribute("type", type), new XAttribute("length", F(at)),
                type == "racing_line" ? new XElement("racing_line", new XAttribute("type", "optimal")) : null);
            aiGates.Add(new XElement("gate", new XAttribute("id", i), Vec("position", g.Position + left * g.Left), Vec("normal", -left),
                new XElement("waypoints", new XAttribute("num_waypoints", 5),
                    Waypoint(0, "right_track_limit", width), Waypoint(1, "left_track_limit", 0),
                    Waypoint(2, "right_racing_limit", width - MathF.Min(1, g.Right / 2)), Waypoint(3, "left_racing_limit", MathF.Min(1, g.Left / 2)),
                    Waypoint(4, "racing_line", g.Left))));
            links.Add(new XElement("link", new XAttribute("id", i), new XAttribute("fork_id", 0), new XAttribute("from_gate", i), new XAttribute("to_gate", (i + 1) % aiIndices.Length)));
        }
        Files.Xml(new XDocument(new XElement("ai_track_data", new XAttribute("version_major", 3), new XAttribute("version_minor", 0), new XAttribute("version_revision", 2),
            new XElement("track", new XAttribute("name", "default"), aiGates, links,
                new XElement("brake_lines", new XAttribute("num_brake_lines", 0)), new XElement("min_speed_lines", new XAttribute("num_min_speed_lines", 0)),
                new XElement("retire_lines", new XAttribute("num_retire_lines", 0)), new XElement("fork_sets", new XAttribute("num_fork_sets", 0))))),
            Replace(routeOut, "ai_track.xml"));

        // Stock per-vehicle brake lines refer to the donor's AI brake lines; clear them.
        using (var input = File.OpenRead(Path.Combine(donorRoute, "ai_vehicle_track.xml")))
        {
            var vehicle = new XmlFile(input);
            foreach (System.Xml.XmlElement brake in vehicle.Document.SelectNodes("//brake_lines")!) { brake.RemoveAll(); brake.SetAttribute("num_brake_lines", "0"); }
            using var stream = new FileStream(Replace(routeOut, "ai_vehicle_track.xml"), FileMode.CreateNew); vehicle.Write(stream);
        }
        return new { ProgressGates = progressIndices.Length, ProgressGateBudget, AiGates = aiIndices.Length, AiGateBudget, Splits = 4, Type = "circuit",
            VehicleProgressProfile = "donor (not rescaled)", ReplayCameras = "donor (replays skipped)" };
    }

    static object WriteGrid(AcSource source, string donorRoute, string routeOut)
    {
        var start = source.Gates[0];
        var tangent = Vector3.Normalize(new Vector3(start.Tangent.X, 0, start.Tangent.Z));
        var left = LeftOf(tangent); var right = -left;
        var grid = Files.Pssg(Path.Combine(donorRoute, "grids.pssg"));
        var slots = grid.Elements<PssgNode>().Where(n => n.Id.StartsWith("slot_", StringComparison.Ordinal)).ToArray();
        if (slots.Length != GridSlots) throw new InvalidDataException("Unsupported donor grid.");
        var placed = new List<object>();
        foreach (var node in grid.Elements<PssgNode>())
        {
            node.Transform.Transform = Matrix4x4.Identity;
            if (!node.Id.StartsWith("slot_", StringComparison.Ordinal)) continue;
            int index = int.Parse(node.Id.AsSpan(5), CultureInfo.InvariantCulture);
            if (index is < 0 or >= GridSlots) throw new InvalidDataException("Unsupported donor grid slot: " + node.Id);
            // Staggered two-column grid behind the line, as on stock circuits.
            // Narrow starts (Nordschleife: ~8 m) get closer columns; below that, single file.
            float back = 14 + index * 4.2f;
            var behind = source.BeforeStart(back);
            float column = MathF.Min(2.8f, (behind.Left + behind.Right) / 2 - 1.5f);
            if (column < 1.9f) { column = 0; back = 14 + index * 7f; behind = source.BeforeStart(back); }
            float lateral = index % 2 == 0 ? column : -column;
            // The AC spline is a racing line; centre the grid on the road between its edges.
            float centre = (behind.Left - behind.Right) / 2;
            var position = behind.Position + LeftOf(behind.Tangent) * (centre + lateral);
            var forward = Vector3.Normalize(new Vector3(behind.Tangent.X, 0, behind.Tangent.Z)); var slotRight = -LeftOf(forward);
            try
            {
                position.Y = source.RoadHeight(position) + .6f;
                // The whole car footprint must sit on asphalt or kerb, not just its centre.
                foreach (int front in new[] { -1, 1 }) foreach (int side in new[] { -1, 1 })
                {
                    var corner = position + forward * (front * 2.5f) + slotRight * (side * 1.2f);
                    if (source.SurfaceAt(corner) is not (AcSource.Road or "RBL*")) source.RoadHeight(corner);
                }
            }
            catch (InvalidDataException error)
            {
                throw new InvalidDataException($"Grid {node.Id} ({back:0} m back, road L {behind.Left:0.0} / R {behind.Right:0.0} m): {error.Message}");
            }
            node.Transform.Transform = new Matrix4x4(slotRight.X, slotRight.Y, slotRight.Z, 0, 0, 1, 0, 0, -forward.X, -forward.Y, -forward.Z, 0, position.X, position.Y, position.Z, 1);
            placed.Add(new { node.Id, BackMetres = back, LateralMetres = lateral, Position = new[] { position.X, position.Y, position.Z } });
        }
        Files.Save(grid, Replace(routeOut, "grids.pssg"));
        return new { Slots = placed, RoadFootprintsVerified = true, StartRoadWidth = new[] { start.Left, start.Right } };
    }

    static void WriteRecovery(AcSource source, string routeOut)
    {
        var recovery = new RouteRecovery(source.Collision, AcSource.RoadSurfaces);
        var gates = source.Gates;
        foreach (var (name, material) in new[] { ("boundarylines.cqtc", "FBND"), ("resetlines.cqtc", "RESE"), ("cameralines.cqtc", "RESD") })
        {
            var planes = new QuadTreeMeshDataBuilder(CQuadTreeTypeInfo.Get(CQuadTreeType.Dirt));
            for (int i = 0; i < gates.Length; i++)
            {
                var g = gates[i]; var next = gates[(i + 1) % gates.Length]; var left = LeftOf(g.Tangent); var nextLeft = LeftOf(next.Tangent);
                if (material == "FBND")
                    foreach (int side in new[] { -1, 1 })
                    {
                        var a = g.Position + left * (side * (side > 0 ? g.Left + 20 : g.Right + 20)); var b = next.Position + nextLeft * (side * (side > 0 ? next.Left + 20 : next.Right + 20));
                        var c = b + Vector3.UnitY * 4; var d = a + Vector3.UnitY * 4;
                        planes.Add(new(a, b, c, material)); planes.Add(new(a, c, d, material)); planes.Add(new(c, b, a, material)); planes.Add(new(d, c, a, material));
                    }
                else
                {
                    var a = g.Position + left * (g.Left + 35); var b = next.Position + nextLeft * (next.Left + 35);
                    var c = next.Position - nextLeft * (next.Right + 35); var d = g.Position - left * (g.Right + 35);
                    float floor = recovery.Floor(a, b, c, d); a.Y = b.Y = c.Y = d.Y = floor;
                    planes.Add(new(a, b, c, material)); planes.Add(new(a, c, d, material));
                }
            }
            File.WriteAllBytes(Replace(routeOut, name), CQuadTreeFile.Create(planes.Build()).Bytes);
        }
    }

    // Benchmark session in the overlay root (GRID 2 resolves it through -mod) and the ModList.
    static void WriteSession(string output, string layout)
    {
        var overlay = Path.Combine(output, "overlay");
        new XDocument(new XElement("config", new XAttribute("cycle_camera", "false"), new XAttribute("infinite_loop", "false"), new XAttribute("skipreplays", "true"),
            new XAttribute("outputfilename", "G2VRTrack"),
            new XElement("track", new XAttribute("country", "circuits"), new XAttribute("name", Location), new XAttribute("route", Route),
                new XAttribute("lighting", "day"), new XAttribute("weather", "dry"), new XAttribute("laps", 1), new XAttribute("number_cars", 1),
                new XElement("car", new XAttribute("name", "z28"), new XAttribute("livery", "user"), new XAttribute("ai_driver", "Cau_Demo_1_1"),
                    new XAttribute("ai_driver0", "Cau_Demo_1_0"), new XAttribute("grid", 0))))).Save(Path.Combine(overlay, "G2VRTrack.xml"));
        new XDocument(new XElement("ModList", new XElement("Mod", new XAttribute("Path", Path.GetFullPath(overlay))))).Save(Path.Combine(output, "modlist.xml"));
    }

    // Generated route files replace the copied donor file of the same name.
    static string Replace(string folder, string name)
    {
        string path = Path.Combine(folder, name); Files.NoLinks(path); File.Delete(path); return path;
    }
}
