namespace GRID2VR;

internal static class CockpitTests
{
    internal static void Run(SessionClient client)
    {
        static void Check(bool ok, [System.Runtime.CompilerServices.CallerLineNumber] int line = 0)
        { if (!ok) throw new InvalidOperationException($"Cockpit launcher regression at line {line}."); }
        var original = client.Load();
        Check(new Choices().VrCockpit && new Choices().CockpitDriver == "HeadOnly");
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
