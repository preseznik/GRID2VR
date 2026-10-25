namespace GRID2VR;

internal static class LaunchModeTests
{
    internal static void RunForm(MainForm form)
    {
        const System.Reflection.BindingFlags flags = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        static void Check(bool value) { if (!value) throw new InvalidOperationException("Opponent car form regression."); }
        void Set(string name, object value) => typeof(MainForm).GetField(name, flags)!.SetValue(form, value);
        object? Call(string name, params object[] args) => typeof(MainForm).GetMethod(name, flags)!.Invoke(form, args);
        ComboBox Choice(string name) => (ComboBox)form.Controls.Find(name, true).Single();
        ContentItem[] cars = [new("foc", "Ford Focus ST", "hot_hatches", "Hot hatches"),
            new("gol", "Volkswagen Golf R", "hot_hatches", "Hot hatches"), new("pag", "Pagani Huayra", "supercars", "Supercars"), new("custom", "Custom car")];
        ContentItem[] routes = [new("cities/chicago/route_0", "Chicago — Route 0")];
        Set("cars", cars); Set("routes", routes); Set("loaded", true); Set("known", true);
        Choice("PracticeCar").Items.AddRange(cars);
        Choice("PracticeEvent").Items.Add(new ContentItem("cities/chicago", "Chicago"));
        var selected = new Choices("foc", routes[0].Id) { LaunchMode = "Race", OpponentCars = "class", ModernInterface = LauncherAppearance.Modern };
        Call("ApplyChoices", selected);
        var opponents = Choice("OpponentCars"); var mode = Choice("LaunchMode"); var car = Choice("PracticeCar");
        var hint = (Label)form.Controls.Find("OpponentHint", true).Single();
        Check(opponents.Items.Cast<ContentItem>().Select(c => c.Label).SequenceEqual(new[] { "Same car", "Mixed", "Same class" }));
        Check(opponents.Enabled && opponents.Text == "Same class" && hint.Text.Contains("Class: Hot hatches"));
        foreach (var id in new[] { "Practice", "Normal", "Race" })
        {
            mode.SelectedItem = mode.Items.Cast<ContentItem>().Single(c => c.Id == id);
            Check(opponents.Enabled == (id == "Race") && ((ContentItem)opponents.SelectedItem!).Id == "class");
            Check(((Choices)Call("Current")!).OpponentCars == "class");
        }
        car.SelectedItem = cars[2]; Check(hint.Text.Contains("Class: Supercars") && hint.Text.Contains("No other installed models"));
        car.SelectedItem = cars[3]; Check(hint.Text.Contains("Unknown vehicle class"));
        car.SelectedItem = cars[0];
        opponents.SelectedIndex = 1; Check(hint.Text.Contains("Mixed: all installed"));
        opponents.SelectedIndex = 0; Check(hint.Text.Contains("All opponents use your selected car"));
        opponents.SelectedIndex = 2;
        foreach (var state in new[] { "active", "pending", "busy", "capturingRecenter" })
        {
            Set(state, true); Call("EnableActions"); Check(!opponents.Enabled);
            Set(state, false); Call("EnableActions"); Check(opponents.Enabled);
        }
        Check(!form.Visible);
    }

    internal static void Run(SessionClient client)
    {
        static void Check(bool value) { if (!value) throw new InvalidOperationException("Launch mode regression."); }
        var original = client.Load();
        Check(new Choices().LaunchMode == "Practice" && new Choices().Opponents == 7 && new Choices().VrGarageMenus && new Choices().OpponentCars == "same");
        foreach (var mode in new[] { "Normal", "Practice", "Race" })
            foreach (var vr in new[] { false, true })
            foreach (var opponentCars in new[] { "same", "mixed", "class" })
            {
                var selected = original with { LaunchMode = mode, Opponents = 3, OpponentCars = opponentCars, Laps = 20, VrGarageMenus = !vr };
                client.Save(selected); Check(client.Load() == selected);
                var args = client.LaunchCommand(selected, vr).ArgumentList;
                Check(args[args.IndexOf("-LaunchMode") + 1] == mode);
                Check(args[args.IndexOf("-Opponents") + 1] == "3");
                Check(args[args.IndexOf("-OpponentCars") + 1] == opponentCars);
                Check(args[args.IndexOf("-Mode") + 1] == (vr ? "VR" : "Desktop"));
                Check(vr ? args[args.IndexOf("-VrGarageMenus") + 1] == "0" : !args.Contains("-VrGarageMenus"));
                Check(selected.Graphics == original.Graphics);
            }
        client.Save(original);
        foreach (var invalid in new[] { original with { LaunchMode = "Race; Stop-Process" }, original with { Opponents = 0 }, original with { Opponents = 8 }, original with { OpponentCars = "unknown" }, original with { OpponentCars = null! } })
        {
            var rejected = false;
            try { client.Save(invalid); } catch (ArgumentException) { rejected = true; }
            Check(rejected && client.Load() == original);
        }
        var normal = LauncherPresentation.RequestedSession(null, null, "VDXR", 121, 0, 20, "Normal");
        Check(normal.Contains("choose your event in the game") && normal.Contains("VDXR") && !normal.Contains("laps"));
        var race = LauncherPresentation.RequestedSession(new("pag", "Pagani"), new("cities/chicago/route_0", "Chicago"), "VDXR", 121, 0, 20, "Race", 3);
        Check(race.Contains("3 AI opponents") && race.Contains("20 laps"));
        var focus = new ContentItem("foc", "Ford Focus ST", "hot_hatches", "Hot hatches");
        ContentItem[] cars = [focus, new("gol", "Volkswagen Golf R", "hot_hatches", "Hot hatches")];
        Check(LauncherPresentation.OpponentHint(focus, cars, "Race", "class").Contains("Class: Hot hatches"));
        Check(LauncherPresentation.OpponentHint(focus, [focus], "Race", "class").Contains("No other installed models"));
        Check(LauncherPresentation.OpponentHint(new("mod", "Custom"), cars, "Race", "class").Contains("Unknown vehicle class"));
        Check(LauncherPresentation.OpponentHint(focus, cars, "Practice", "mixed").Contains("kept for Race"));
        foreach (var choice in new[] { "same", "mixed", "class" })
        {
            var text = LauncherPresentation.RequestedSession(focus, new("cities/chicago/route_0", "Chicago"), "VDXR", 121, 0, 1, "Race", 7, choice);
            Check(text.Contains(LauncherPresentation.OpponentSelection(focus, choice)));
        }
        Check(LauncherPresentation.OpponentSelection(new("mod", "Custom"), "class").Contains("unknown class; same car"));
        using var slider = new ValueSlider("Laps", 1, 20, 1);
        var changes = 0; slider.ValueChanged += (_, _) => ++changes;
        slider.Value = 20; Check(slider.Value == 20 && changes == 1);
        slider.Value = 1; Check(slider.Value == 1 && changes == 2);
        Check(slider.AccessibleDescription == "1; 1–20");
    }
}
