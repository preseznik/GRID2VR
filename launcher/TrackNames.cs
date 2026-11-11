namespace GRID2VR;

// English location and layout names from the installed language/language_eng.lng
// (db_<location>[_p2p]_route_<n> keys), in mixed case. Launch identifiers stay
// separate from presentation; see docs/evidence/track-names-2026-11-11.json.
internal static class TrackNames
{
    private static readonly IReadOnlyDictionary<string, string> Locations = new Dictionary<string, string>(StringComparer.Ordinal)
    {
        ["algarve"] = "Algarve", ["brands_hatch"] = "Brands Hatch", ["indianapolis"] = "Indianapolis",
        ["red_bull_ring"] = "Red Bull Ring", ["yas_marina"] = "Yas Marina",
        ["barcelona"] = "Barcelona", ["chicago"] = "Chicago", ["dubai"] = "Dubai", ["miami"] = "Miami", ["paris"] = "Paris",
        ["california"] = "California", ["cote_d_azur"] = "Cote d'Azur", ["hong_kong"] = "Hong Kong", ["okutama"] = "Okutama",
        ["bathurst"] = "Mount Panorama", ["spa_francorchamps"] = "Spa-Francorchamps",
    };

    private static readonly IReadOnlyDictionary<string, string[]> Routes = new Dictionary<string, string[]>(StringComparer.Ordinal)
    {
        ["circuits/algarve"] = ["Sport Circuit", "GP Circuit", "National Circuit", "Club Circuit"],
        ["circuits/brands_hatch"] = ["GP Circuit", "GP Circuit Reversed", "Indy Circuit", "Indy Circuit Reversed"],
        ["circuits/indianapolis"] = ["Sport Circuit", "", "", "GP Circuit", "Oval Circuit", "", "Infield Circuit", "North Circuit"],
        ["circuits/red_bull_ring"] = ["GP Circuit", "GP Circuit Reversed", "South Circuit", "South Circuit Reversed", "North Circuit", "North Circuit Reversed"],
        ["circuits/yas_marina"] = ["GP Circuit", "Championship Circuit", "International Circuit", "South Circuit", "Paddock Circuit"],
        ["cities/barcelona"] = ["Memorial Run", "Marine Gate", "Columbus Bay", "Cathedral Pass", "Fountain Loop", "High Street", "LiveRoutes"],
        ["cities/chicago"] = ["The Loop", "Underpass Ring", "Wabash Run", "Riverside", "Marina City", "Lake Shore Point", "LiveRoutes"],
        ["cities/dubai"] = ["Al Sufouh Strip", "Jumeirah Beach", "Nakheel Vista", "Hattan Way", "Orra Quay Loop", "Gulf Approach", "LiveRoutes"],
        ["cities/miami"] = ["Collins Park Ring", "Ocean Drive", "Art Deco Loop", "Downtown Speedway", "South Point Bay", "Causeway Approach", "LiveRoutes"],
        ["cities/paris"] = ["Arc de Triomphe", "Avenue de New-York", "Circuit de la Seine", "Champs Élysées", "Pont de l'Alma", "Le Trocadero", "LiveRoutes"],
        ["p2p/california"] = ["Big Sur", "Pebble Beach Drive", "Redwood Approach", "Pacific Way", "Bixby Pass", "Cabrillo Highway"],
        ["p2p/cote_d_azur"] = ["Route d'Azur", "Saint-Laurent", "La Turbie", "Route de Corniche", "Leopolda", "Villefranche-sur-Mer"],
        ["p2p/hong_kong"] = ["Peak Road Descent", "Kowloon Climb", "Magazine Gap", "Pok Fu Lam", "Wan Chai Gap", "Ap Lei Chau"],
        ["p2p/okutama"] = ["Mizu Mountain", "Tenshi Way", "Shinto Shrine", "Torii Rush", "Sakura Pass", "Tatsu Valley"],
        ["australia/bathurst"] = ["Motor Racing Circuit", "Motor Racing Circuit Reversed"],
        ["europe/spa_francorchamps"] = ["GP Circuit", "Sportscar", "GP Circuit Reversed", "Sportscar Reversed"],
    };

    // Conditions the game offers per route (db_..._route_<n>_day|night|sunset). Day is
    // always offered: it is the benchmark default every route has launched with.
    // Night: city and point-to-point routes except the sunset ones below, and Yas Marina.
    private static readonly IReadOnlySet<string> NightLocations = new HashSet<string>(StringComparer.Ordinal)
    {
        "circuits/yas_marina", "cities/barcelona", "cities/chicago", "cities/dubai", "cities/miami", "cities/paris",
        "p2p/california", "p2p/cote_d_azur", "p2p/hong_kong", "p2p/okutama",
    };
    private static readonly IReadOnlySet<string> SunsetRoutes = new HashSet<string>(StringComparer.Ordinal)
    {
        "cities/barcelona/route_2", "cities/barcelona/route_4", "cities/chicago/route_3", "cities/chicago/route_4",
        "cities/dubai/route_3", "cities/dubai/route_4", "cities/miami/route_1", "cities/miami/route_2",
        "cities/paris/route_3", "cities/paris/route_5", "p2p/california/route_3", "p2p/california/route_5",
        "p2p/cote_d_azur/route_3", "p2p/cote_d_azur/route_5", "p2p/hong_kong/route_3", "p2p/hong_kong/route_5",
        "p2p/okutama/route_3", "p2p/okutama/route_4",
    };

    internal const string Day = "day", Night = "night", Sunset = "sunset";

    // The conditions a route offers, Day first.
    internal static IReadOnlyList<string> Conditions(string track)
    {
        var separator = track.LastIndexOf('/');
        if (separator < 0) return [Day];
        var location = track[..separator];
        if (SunsetRoutes.Contains(track)) return [Day, Sunset];
        // LiveRoutes (route_6) is day only.
        return NightLocations.Contains(location) && track[(separator + 1)..] != "route_6" ? [Day, Night] : [Day];
    }

    // The benchmark lighting value for a route's condition, or "day" when it does not
    // offer that condition. Sunset is the location's one alternate lighting set:
    // night_alt at Okutama, day_alt elsewhere (tracks/locations/*/lighting_*.xml).
    internal static string Lighting(string track, string condition)
    {
        if (!Conditions(track).Contains(condition)) return "day";
        return condition switch
        {
            Night => "night",
            Sunset => track.StartsWith("p2p/okutama/", StringComparison.Ordinal) ? "night_alt" : "day_alt",
            _ => "day",
        };
    }

    internal static string ConditionLabel(string condition) => condition switch { Night => "Night", Sunset => "Sunset", _ => "Day" };

    // "okutama" -> "Okutama"; null when the location is not a known stock or DLC one.
    internal static string? Location(string name) => Locations.TryGetValue(name, out var label) ? label : null;

    // "p2p/okutama", "route_1" -> "Tenshi Way"; null when unknown.
    internal static string? Route(string location, string route)
    {
        if (!Routes.TryGetValue(location, out var names) || !route.StartsWith("route_", StringComparison.Ordinal) ||
            !int.TryParse(route.AsSpan(6), System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var index) ||
            index >= names.Length || names[index].Length == 0) return null;
        return names[index];
    }
}
