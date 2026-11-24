// Extra lighting presets for a converted venue, as DiRT2VR offers for Misty Loch (Daylight,
// Evening, Morning Fog; tools/misty-converter/MistyLighting.cs, same author). GRID 2 names a
// location's lighting files by set (day, day_alt, night, night_alt): lighting_<set>.xml plus its
// skymap, envmap, ambientmaps, ground-cover palette and waterdefs, and per route sky_<set>.pssg…
// But a benchmark session at a g2vr_ venue renders the day set whatever lighting it names
// (runtime 2026-11-23: day_alt and night_alt both looked like Daylight; stock routes switch,
// presumably through their database rows, which a converted venue cannot have). So each
// preset is its own small -mod overlay, <pack>/presets/<set>/, holding only the files that
// differ from Daylight under the day names; the session lists it before the pack's overlay
// (GRID 2 loads the first copy it finds) and runs the day set.
//  - Evening (day_alt): California's stock sunset set (lighting, sky dome, skymap, envmap) with
//    the venue's own geography (ambient-map scale, fog heights) and the venue's sun azimuth.
//  - Morning Fog (night_alt): the day set with a low, cool, diffuse sun and dense fog
//    (DiRT2VR's Morning Fog light on GRID 2's parameters; the fog itself neutral grey), under GRID 2's own fog sky: Okutama
//    night_alt's flat grey dome, sky gamma, skymap and envmap. Runtime 2026-11-24: the venue's
//    blue, cloudy dome showed above the fog; Okutama's overcast day dome then rendered dark and
//    stormy with hard edges against the fogged hills (the dome is not fogged).
// Every preset keeps the day sun azimuth: converted scenery bakes sun shadows toward it
// (SunShadow), and the sky dome is rotated so its painted sun stays where the light comes from.
using System.Globalization;
using System.Xml.Linq;
using EgoEngineLibrary.Xml;

namespace GRID2VR.Tracks;

internal static class LightingPresets
{
    internal const string Evening = "Evening", MorningFog = "Morning Fog";
    internal static string Set(string preset) => preset switch { Evening => "day_alt", MorningFog => "night_alt", _ => throw new ArgumentException("Unknown lighting preset: " + preset) };

    const string SunsetDonor = "tracks/locations/p2p/california";
    // A California sunset route (TrackNames sunset routes): its sky dome for Evening.
    const string SunsetSky = "tracks/locations/p2p/california/route_3/sky_day_alt.pssg";
    // Every stock route ships these per lighting set (Barcelona route_2, California route_3,
    // Chicago route_0…); a converted route has the donor's day versions.
    static readonly string[] RouteSet = ["ambient_sh_{0}.bin", "buildings_lm_{0}.pssg", "shadow_map_{0}.clm", "sky_{0}.pssg", "terrain_lm_{0}.pssg"];

    // Okutama's night_alt set is GRID 2's fog (grey fog at 100 m under a flat grey dome): Morning Fog's sky.
    const string FogDonor = "tracks/locations/p2p/okutama";
    const string FogSky = "tracks/locations/p2p/okutama/route_3/sky_night_alt.pssg";
    static string Sky(string preset) => preset == Evening ? SunsetSky : FogSky;
    // How bright the dome draws (the venue's 1.4 inner gamma darkens it).
    static readonly string[] SkyGamma = ["skyGammaInnerAngle", "skyGammaInnerGamma", "skyGammaInnerScale", "skyGammaOuterAngle", "skyGammaOuterGamma", "skyGammaOuterScale"];

    // Route-level files of each preset that differ from Daylight: its sky dome.
    internal static object WriteRoute(string gameRoot, Func<string, string> presetRoute, IReadOnlyList<string> presets, IDictionary<string, string> inputs)
    {
        var written = new List<string>();
        foreach (var preset in presets)
        {
            string from = Files.Inside(gameRoot, Sky(preset)), to = Path.Combine(presetRoute(Set(preset)), "sky_day.pssg");
            Files.NoLinks(from); inputs.TryAdd(from, Files.Hash(from));
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            if (File.Exists(to)) throw new InvalidDataException("Preset route already has " + to);
            File.Copy(from, to); written.Add($"{Set(preset)}: sky_day.pssg");
        }
        return written;
    }
    const string SunsetWater = "tracks/locations/p2p/okutama/waterdefs_night_alt.xml";
    // Venue-specific values kept from the venue's own day set when another location's set is used.
    static readonly string[] Geography = ["ambientLightMapXScale", "ambientLightMapZScale", "fogTopLevel", "fogBaseLevel", "fogConsoleTopLevel", "fogConsoleBaseLevel", "vfogEnable", "vfogInvert", "vfogHeight", "vfogSoften"];

    static XDocument Read(string path, IDictionary<string, string> inputs)
    {
        Files.NoLinks(path); inputs.TryAdd(path, Files.Hash(path));
        using var input = File.OpenRead(path); return XDocument.Parse(new XmlFile(input).Document.OuterXml);
    }
    static XElement Param(XDocument lighting, string key) => lighting.Root!.Elements("param").SingleOrDefault(p => p.Attribute(key) is not null)
        ?? throw new InvalidDataException("Lighting has no " + key);
    static float Value(XDocument lighting, string key) => float.Parse((string)Param(lighting, key).Attribute(key)!, CultureInfo.InvariantCulture);
    static void Set(XDocument lighting, string key, string value) => Param(lighting, key).SetAttributeValue(key, value);
    static string F(float v) => v.ToString("0.00000", CultureInfo.InvariantCulture);

    internal static object Write(string gameRoot, string venueOut, Func<string, string> presetVenue, IReadOnlyList<string> presets, IDictionary<string, string> inputs)
    {
        var report = new List<object>();
        string Venue(string name) => Path.Combine(venueOut, name);
        foreach (var name in new[] { "lighting_day.xml", "skymap_day.clm", "envmap_day.pssg", "ambientmaps_day.pssg", "ground_cover_palette_day.xml" })
            if (!File.Exists(Venue(name))) throw new InvalidDataException("Converted venue lacks its day set: " + name);
        var day = Read(Venue("lighting_day.xml"), inputs);
        float azimuth = Value(day, "rotationangle");
        foreach (var preset in presets)
        {
            string set = Set(preset), target = presetVenue(set);
            if (Directory.Exists(target)) throw new InvalidDataException($"Preset {set} already exists.");
            Directory.CreateDirectory(target);
            XDocument lighting; var copies = new Dictionary<string, string>(StringComparer.Ordinal);
            Dictionary<string, string> changed;
            if (preset == Evening)
            {
                string donor = Files.Inside(gameRoot, SunsetDonor);
                lighting = Read(Path.Combine(donor, "lighting_day_alt.xml"), inputs);
                foreach (var key in Geography) Set(lighting, key, (string)Param(day, key).Attribute(key)!);
                // Same azimuth as the bake; the sky turns by the same amount so its sun follows.
                float turn = azimuth - Value(lighting, "rotationangle");
                changed = new() { ["rotationangle"] = F(azimuth), ["spec_rotationangle"] = F(azimuth), ["skyRotationAngle"] = F(Value(lighting, "skyRotationAngle") + turn),
                    ["flagLightRotation"] = F(Value(lighting, "flagLightRotation") + turn), ["teardropLightRotation"] = F(Value(lighting, "teardropLightRotation") + turn) };
                // The venue's ground-cover palette and ambient maps stay (the pack's day files).
                copies["skymap"] = Path.Combine(donor, "skymap_day_alt.clm"); copies["envmap"] = Path.Combine(donor, "envmap_day_alt.pssg");
                copies["waterdefs"] = File.Exists(Venue("waterdefs_day.xml")) ? Files.Inside(gameRoot, SunsetWater) : "";
            }
            else
            {
                lighting = new XDocument(day);
                // Neutral grey at DiRT2VR's brightness, matching the grey fog dome (runtime 2026-11-24:
                // blue-grey fog left distant ridges outlined against it).
                const string Fog = "0.680 0.680 0.680", Haze = "0.670 0.670 0.670", Haze2 = "0.730 0.730 0.730";
                changed = new()
                {
                    ["heightangle"] = F(10), ["spec_heightangle"] = F(10), ["skySunHeightAngle"] = F(-5), ["skySunMultiplier"] = F(2),
                    ["backlight_sun_colour"] = "0.720 0.780 0.860", ["backlight_sunscale"] = F(.7f), ["backlight_sky_colour"] = "0.550 0.600 0.660", ["backlight_skyscale"] = F(1.6f),
                    ["backlight_ambient_colour"] = "0.360 0.400 0.450", ["backlight_back_colour"] = "0.400 0.440 0.480", ["specular_colour"] = "0.320 0.360 0.420",
                    ["pfx_sun_colour"] = "0.650 0.700 0.780", ["skyGlobalColour"] = "0.620 0.670 0.740", ["gi_directLightingIntensity"] = F(.6f),
                };
                foreach (var prefix in new[] { "fog", "fogConsole" })
                {
                    changed[prefix + "Colour"] = Fog; changed[prefix + "ColourBrightness"] = F(.85f); changed[prefix + "HazeColour"] = Haze; changed[prefix + "HazeColour2"] = Haze2;
                    changed[prefix + "HazeBrightness2"] = F(1); changed[prefix + "Distance"] = F(120); changed[prefix + "DensityCap"] = F(.85f); changed[prefix + "HazeDistance"] = F(400);
                }
                // The fog dome draws as bright as in its own set and turns with the sun like Evening's.
                string donor = Files.Inside(gameRoot, FogDonor);
                var fog = Read(Path.Combine(donor, "lighting_night_alt.xml"), inputs);
                foreach (var key in SkyGamma) changed[key] = (string)Param(fog, key).Attribute(key)!;
                changed["skyRotationAngle"] = F(Value(fog, "skyRotationAngle") + azimuth - Value(fog, "rotationangle"));
                copies["skymap"] = Path.Combine(donor, "skymap_night_alt.clm"); copies["envmap"] = Path.Combine(donor, "envmap_night_alt.pssg");
                // Ambient maps, palette and water stay the pack's day files.
            }
            foreach (var (key, value) in changed) Set(lighting, key, value);
            Files.Xml(lighting, Path.Combine(target, "lighting_day.xml"));
            var written = new List<string> { "lighting_day.xml" };
            foreach (var (kind, from) in copies.Where(c => c.Value.Length > 0))
            {
                string to = Path.Combine(target, $"{kind}_day{Path.GetExtension(from)}");
                Files.NoLinks(from); inputs.TryAdd(from, Files.Hash(from));
                File.Copy(from, to); written.Add(Path.GetFileName(to));
            }
            // Read back: the set parses and carries the preset's values.
            var saved = Read(Path.Combine(target, "lighting_day.xml"), new Dictionary<string, string>());
            foreach (var (key, value) in changed) if ((string?)Param(saved, key).Attribute(key) != value) throw new InvalidDataException($"{preset} lighting lost {key}.");
            report.Add(new { Preset = preset, Set = set, Files = written, Values = changed, SunAzimuth = azimuth, SunHeight = Value(saved, "heightangle"), Source = preset == Evening ? SunsetDonor + " day_alt" : "venue day set; sky " + FogDonor + " night_alt" });
        }
        return report;
    }
}
