using System.Text.Json;

namespace GRID2VR;

internal static class CockpitTests
{
    internal static void Run(SessionClient client)
    {
        static void Check(bool ok, [System.Runtime.CompilerServices.CallerLineNumber] int line = 0)
        { if (!ok) throw new InvalidOperationException($"Cockpit launcher regression at line {line}."); }
        var original = client.Load();
        Check(new Choices().VrCockpit && new Choices().CockpitDriver == "HeadOnly");
        using var source = JsonDocument.Parse("""
            {"cars":[
              {"id":"s20","liveries":["user"],"classId":"sports","className":"Sports"},
              {"id":"ind","label":"Dallara IndyCar","liveries":["user"],"classId":"open_wheel","className":"Open wheel","dlc":true,"cockpitAvailable":false},
              {"id":"b12","label":" ","liveries":["user"],"classId":null,"className":null,"dlc":true},
              {"id":"240","liveries":["user"],"classId":null,"className":null,"dlc":true,"cockpitAvailable":true},
              {"id":"370","liveries":["livery_00"],"classId":null,"className":null,"dlc":true}
            ],"tracks":[
              {"id":"europe/spa_francorchamps/route_0","label":"Spa-Francorchamps — Grand Prix","dlc":true},
              {"id":"circuits/brands_hatch/route_2"}
            ]}
            """);
        var content = SessionClient.ParseCatalog(source.RootElement);
        Check(content.Cars.Length == 4 && content.Tracks.Length == 2);
        var stock = content.Cars.Single(c => c.Id == "s20");
        var dlc = content.Cars.Single(c => c.Id == "ind");
        Check(stock.CockpitAvailable && !stock.Dlc && stock.Label == SessionClient.CarLabel("s20"));
        Check(dlc.Dlc && !dlc.CockpitAvailable && dlc.Label == "Dallara IndyCar" && dlc.ClassId == "open_wheel");
        Check(!content.Cars.Single(c => c.Id == "b12").CockpitAvailable && content.Cars.Single(c => c.Id == "b12").Label == SessionClient.CarLabel("b12"));
        Check(content.Cars.Single(c => c.Id == "240").CockpitAvailable);
        var track = content.Tracks.Single(t => t.Dlc);
        Check(track.Id == "europe/spa_francorchamps/route_0" && track.Label == "Spa-Francorchamps — Grand Prix");
        Check(content.Tracks.Single(t => !t.Dlc).Label == "Brands Hatch — Route 2");
        Check(LauncherPresentation.FilterCars(content.Cars, "IndyCar").Single() == dlc);
        foreach (var route in new[] { "australia/bathurst/route_0", "australia/bathurst/route_1", "europe/spa_francorchamps/route_3" })
        {
            var selected = original with { Track = route, Laps = 5 };
            client.Save(selected); Check(client.Load() == selected);
            var command = client.LaunchCommand(selected, true).ArgumentList;
            Check(command[command.IndexOf("-Track") + 1] == route && command[command.IndexOf("-Laps") + 1] == "5");
        }
        foreach (var mode in new[] { "Practice", "Race" })
        {
            var text = LauncherPresentation.RequestedSession(dlc, track, "VDXR", 121, 0, mode: mode);
            Check(text.Contains("DLC car uses original cameras") && text.Contains("custom VR cockpit and seat alignment are unavailable"));
            Check(!LauncherPresentation.RequestedSession(stock, track, "VDXR", 121, 0, mode: mode).Contains("unavailable"));
            var selected = original with { Car = dlc.Id, Track = track.Id, LaunchMode = mode, VrCockpit = true };
            client.Save(selected); Check(client.Load() == selected);
            var vr = client.LaunchCommand(selected, true).ArgumentList;
            Check(vr[vr.IndexOf("-Car") + 1] == dlc.Id && vr[vr.IndexOf("-CockpitMode") + 1] == selected.CockpitDriver);
            Check(!client.LaunchCommand(selected, false).ArgumentList.Contains("-CockpitMode"));
        }
        foreach (var mode in new[] { "Normal", "Practice", "Race" })
            foreach (var driver in new[] { "HeadOnly", "HideDriver" })
                foreach (var enabled in new[] { false, true })
                {
                    var choices = original with { LaunchMode = mode, VrCockpit = enabled, CockpitDriver = driver };
                    client.Save(choices); Check(client.Load() == choices);
                    var vr = client.LaunchCommand(choices, true).ArgumentList;
                    Check(vr[vr.IndexOf("-CockpitMode") + 1] == (enabled ? driver : "Off"));
                    Check(!client.LaunchCommand(choices, false).ArgumentList.Contains("-CockpitMode"));
                    Check(choices.Graphics == original.Graphics);
                }
        client.Save(original);
        var rejected = false;
        try { client.Save(original with { CockpitDriver = "HeadOnly; bad" }); } catch (ArgumentException) { rejected = true; }
        Check(rejected && client.Load() == original);
        using var form = new MainForm(client, new ProfileStore(client.Root, () => { }, () => null));
        var toggle = (CheckBox)form.Controls.Find("VrCockpit", true).Single();
        var visibility = (ComboBox)form.Controls.Find("CockpitDriver", true).Single();
        Check(toggle.Checked && !toggle.Enabled && !visibility.Enabled && ((ContentItem)visibility.SelectedItem!).Id == "HeadOnly");
        toggle.Enabled = true;
        toggle.Checked = false; Check(!visibility.Enabled);
        toggle.Checked = true; Check(visibility.Enabled);
        Check(!form.Visible);
    }
}
