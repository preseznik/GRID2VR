// The Assetto Corsa layouts GRID2VR converts, and what each becomes in GRID 2.
// A layout is identified by its source track folder and layout name; anything else
// is refused. The launcher's CustomTracks.Supported must name the same locations.
namespace GRID2VR.Tracks;

// LakeMaterials: AC materials drawn as GRID 2 native water (LakeWater) instead of terrain.
internal sealed record TrackDefinition(string AcTrack, string Layout, string Pins, string Location, string TitleLocation, string TitleRoute, string Label)
{
    internal string[] LakeMaterials { get; init; } = [];
    // Animation.cs: AC material name -> V scroll (UV/s, CSP offsetDSpeed); source kn5 files whose meshes float.
    internal Dictionary<string, float> Flows { get; init; } = [];
    internal string[] FloatingFiles { get; init; } = [];
    // Rotor discs spun about their own hub and axis (Animation.Mark).
    internal SpinSource[] Spins { get; init; } = [];
    // Lighting presets besides the donor's Daylight (LightingPresets.cs).
    internal string[] LightingPresets { get; init; } = [];
    // More layouts of the same venue, built into the same pack as further GRID 2 routes
    // (shared scenery, lighting and water; own collision, AI line, grid and recovery).
    internal (string Layout, string Route, string Label)[] ExtraRoutes { get; init; } = [];
    // Draw all scenery from anywhere on the course (TrackVisuals full view): for wide open venues.
    internal bool FullView { get; init; }
    // A layout that exists only as another definition's extra route (source pins only).
    internal bool RouteOnly { get; init; }
    // AI brake lines (GRID 2 brake_line + hold_line), per layout: corners GRID 2's AI misjudges
    // from the line's shape alone. Layouts without zones keep no brake lines.
    internal BrakeZone[] BrakeZones { get; init; } = [];
}

// From/Hold: metres along the AC driving line where braking starts and the speed cap ends;
// MaxSpeed in m/s (stock brake_data units: Brands Hatch Indy 60 before Paddock Hill, 34 before Druids).
internal sealed record BrakeZone(string Layout, float From, float Hold, float MaxSpeed);

internal static class TrackDefinitions
{
    internal static readonly TrackDefinition[] All =
    [
        new("ks_nordschleife", "nordschleife", "nordschleife-sources.json", "g2vr_nordschleife", "NÜRBURGRING", "NORDSCHLEIFE", "Nordschleife (Assetto Corsa)"),
        // Misty Loch 1.4 (Reboot Team), an Assetto Corsa add-on; same pinned files as DiRT2VR's port.
        new("rt_misty_loch", "normal", "misty-loch-sources.json", "g2vr_misty_loch", "SCOTLAND", "MISTY LOCH", "Misty Loch (Assetto Corsa)")
        {
            LakeMaterials = ["lake1"],
            // Mountains across the lake are 3-7 km away: no distance pop-in.
            FullView = true,
            // extension/ext_config.ini stPerPixelNM_UVflow offsetDSpeed (V); boats from rt_misty_loch_ships.kn5.
            Flows = new() { ["waterfall"] = -1.1f, ["waterfall2"] = -.5f, ["beachwaves"] = -.1f },
            FloatingFiles = ["rt_misty_loch_ships.kn5"],
            // Helicopter (heli.kn5): CSP offsetNMSpeed U 6.3 on both rotors. The tail disc
            // (Rotor2) has polar UVs, so that is ~10 rad/s; the main disc (Rotor) is planar-mapped
            // and turns at the same rate, counter-clockwise from above like the Bell 47's.
            Spins = [new("Rotor", 6.3f, 10f), new("Rotor2", 6.3f, 10f)],
            // DiRT2VR's Misty Loch presets: Daylight (day), Evening (day_alt), Morning Fog (night_alt).
            LightingPresets = [Tracks.LightingPresets.Evening, Tracks.LightingPresets.MorningFog],
            // The reverse layout differs only in its driving line and AC marker dummies (001.kn5 for 100.kn5).
            ExtraRoutes = [("reverse", "route_1", "Misty Loch Reverse (Assetto Corsa)")],
            // The drop into the left-hander after the abbey (20% downhill, crest, 86 m radius at
            // 4,600 m): without a brake line the AI arrived airborne at 200 km/h and ran off or
            // rolled (runtime 2026-11-23/24, lap ~1:25). AC's own line brakes only here
            // (fast_lane.ai brake 4,460-4,540 m, apex 84 km/h in its slower car); 30 m/s is
            // about the grip limit at that radius.
            BrakeZones = [new("normal", 4460, 4615, 30)],
        },
        new("rt_misty_loch", "reverse", "misty-loch-reverse-sources.json", "g2vr_misty_loch", "SCOTLAND", "MISTY LOCH", "Misty Loch Reverse (Assetto Corsa)") { RouteOnly = true },
    ];

    internal static TrackDefinition For(string trackRoot, string layout)
    {
        var track = Path.GetFileName(Path.TrimEndingDirectorySeparator(Path.GetFullPath(trackRoot)));
        var matches = All.Where(d => d.Layout == layout).ToArray();
        if (matches.Length == 0) throw new InvalidDataException("No supported source version is known for layout " + layout + ".");
        return matches.SingleOrDefault(d => string.Equals(d.AcTrack, track, StringComparison.OrdinalIgnoreCase))
            ?? throw new InvalidDataException(matches.Length == 1 ? $"Choose the {matches[0].AcTrack} track folder." : $"Unsupported track folder {track} for layout {layout}.");
    }
}
