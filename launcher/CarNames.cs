namespace GRID2VR;

// English names checked against the installed language/audio data. Keep launch
// identifiers separate from presentation; see docs/evidence/car-names-2026-10-15.json.
internal static class CarNames
{
    private static readonly IReadOnlyDictionary<string, string> Names = new Dictionary<string, string>(StringComparer.Ordinal)
    {
        ["177"] = "Aston Martin One-77",
        ["190"] = "Mercedes-Benz 190E 2.5-16 Evolution II",
        ["300"] = "Nissan Fairlady Z Version S TT 2Seater",
        ["320"] = "BMW 320 Touring Car",
        ["37r"] = "Nissan 370Z Roadster",
        ["a4c"] = "Alfa Romeo 4C",
        ["a8c"] = "Alfa Romeo 8C Competizione",
        ["ato"] = "Ariel Atom 3",
        ["b1m"] = "BMW 1 Series M Coupe",
        ["bac"] = "BAC Mono",
        ["bm3"] = "BMW M3 Coupe",
        ["bos"] = "Ford Mustang Boss 302",
        ["brz"] = "Subaru BRZ",
        ["bug"] = "Bugatti Veyron 16.4 Super Sport",
        ["c63"] = "Mercedes-Benz C63 AMG",
        ["cat"] = "Caterham SP300R",
        ["cha"] = "Dodge Challenger SRT8 392",
        ["crt"] = "Dodge Charger R/T",
        ["crz"] = "Chevrolet Cruze Touring Car",
        ["css"] = "Chevrolet Camaro SS",
        ["e30"] = "BMW E30 Sport Evo",
        ["foc"] = "Ford Focus ST",
        ["gen"] = "Hyundai Genesis Coupe",
        ["giu"] = "Alfa Romeo Giulietta Quadrifoglio Verde",
        ["gol"] = "Volkswagen Golf R",
        ["koe"] = "Koenigsegg Agera R",
        ["ktm"] = "KTM X-Bow R",
        ["mac"] = "Ford Mustang Mach 1 Twister Special",
        ["mf1"] = "McLaren F1 GT",
        ["mp4"] = "McLaren MP4-12C",
        ["nsx"] = "Honda NSX R",
        ["pag"] = "Pagani Huayra",
        ["r8l"] = "Audi R8 LMS Ultra",
        ["rs5"] = "Audi RS5 Coupe",
        ["rx1"] = "Mazda RX7 Type RZ",
        ["s15"] = "Nissan Silvia Spec-R Aero (S15)",
        ["s20"] = "Honda S2000",
        ["s60"] = "Volvo S60 BTCS",
        ["scr"] = "Gomez Competition GC10-V8",
        ["sky"] = "Nissan Nismo R34GT-R Z-Tune",
        ["sl6"] = "Mercedes-Benz SL65 AMG Black Series",
        ["slr"] = "Mercedes-Benz SLR McLaren 722",
        ["sls"] = "Mercedes-Benz SLS AMG GT3",
        ["ttr"] = "Audi TT RS Roadster",
        ["van"] = "Aston Martin Vanquish",
        ["vip"] = "SRT Viper GTS",
        ["xkr"] = "Jaguar XKR-S",
        ["z06"] = "Chevrolet Corvette Z06",
        ["z28"] = "Chevrolet Camaro Z28",
        ["zag"] = "Aston Martin V12 Zagato",
        ["zen"] = "Zenvo ST1"
    };

    public static string Label(string id) => Names.TryGetValue(id, out var name) ? name : id;
}
