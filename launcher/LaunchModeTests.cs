namespace GRID2VR;

internal static class LaunchModeTests
{
    internal static void Run(SessionClient client)
    {
        static void Check(bool value) { if (!value) throw new InvalidOperationException("Launch mode regression."); }
        var original = client.Load();
        Check(new Choices().LaunchMode == "Practice" && new Choices().Opponents == 7 && new Choices().VrGarageMenus);
        foreach (var mode in new[] { "Normal", "Practice", "Race" })
            foreach (var vr in new[] { false, true })
            {
                var selected = original with { LaunchMode = mode, Opponents = 3, Laps = 20, VrGarageMenus = !vr };
                client.Save(selected); Check(client.Load() == selected);
                var args = client.LaunchCommand(selected, vr).ArgumentList;
                Check(args[args.IndexOf("-LaunchMode") + 1] == mode);
                Check(args[args.IndexOf("-Opponents") + 1] == "3");
                Check(args[args.IndexOf("-Mode") + 1] == (vr ? "VR" : "Desktop"));
                Check(vr ? args[args.IndexOf("-VrGarageMenus") + 1] == "0" : !args.Contains("-VrGarageMenus"));
                Check(selected.Graphics == original.Graphics);
            }
        client.Save(original);
        foreach (var invalid in new[] { original with { LaunchMode = "Race; Stop-Process" }, original with { Opponents = 0 }, original with { Opponents = 8 } })
        {
            var rejected = false;
            try { client.Save(invalid); } catch (ArgumentException) { rejected = true; }
            Check(rejected && client.Load() == original);
        }
        var normal = LauncherPresentation.RequestedSession(null, null, "VDXR", 121, 0, 20, "Normal");
        Check(normal.Contains("choose your event in the game") && normal.Contains("VDXR") && !normal.Contains("laps"));
        var race = LauncherPresentation.RequestedSession(new("pag", "Pagani"), new("cities/chicago/route_0", "Chicago"), "VDXR", 121, 0, 20, "Race", 3);
        Check(race.Contains("3 AI opponents") && race.Contains("20 laps"));
        using var slider = new ValueSlider("Laps", 1, 20, 1);
        var changes = 0; slider.ValueChanged += (_, _) => ++changes;
        slider.Value = 20; Check(slider.Value == 20 && changes == 1);
        slider.Value = 1; Check(slider.Value == 1 && changes == 2);
        Check(slider.AccessibleDescription == "1; 1–20");
    }
}
