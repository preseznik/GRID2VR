namespace GRID2VR;

internal static class CustomTracksTests
{
    internal static void Run(string repository, string fixture, SessionClient client)
    {
        static void Check(bool ok, string message) { if (!ok) throw new Exception("Custom tracks: " + message); }
        // Event labels: converted g2vr_ locations are named and marked; stock names are unchanged.
        Check(SessionClient.LocationLabel("circuits/g2vr_nordschleife") == "Nordschleife (custom)", "custom event label");
        Check(SessionClient.LocationLabel("circuits/brands_hatch") == "Brands Hatch" && !SessionClient.LocationLabel("circuits/g2vr_").Contains("(custom)"), "stock and degenerate labels");
        var source = CustomTracks.Supported.Single();
        Check(source.Id == "circuits/g2vr_nordschleife/route_0" && source.SteamAppId == "244210", "the supported source matches the converter's location");

        var root = Path.Combine(fixture, "custom-tracks-test"); Directory.CreateDirectory(root);
        string Pack(string name, string json, bool overlay = true)
        {
            var folder = Path.Combine(root, name); Directory.CreateDirectory(folder);
            File.WriteAllText(Path.Combine(folder, "custom-track.json"), json);
            if (overlay) { Directory.CreateDirectory(Path.Combine(folder, "overlay")); File.WriteAllBytes(Path.Combine(folder, "overlay", "data.bin"), new byte[1000]); }
            return folder;
        }
        string Manifest(string id) => "{\"schema\":1,\"tracks\":[{\"id\":\"" + id + "\",\"label\":\"x\"}]}";
        var good = Pack("nordschleife", Manifest(source.Id));
        Pack("no-overlay", Manifest(source.Id), overlay: false);
        Pack("other", Manifest("circuits/g2vr_other/route_0"));
        Pack("broken", "{not json");
        Pack("schema2", "{\"schema\":2,\"tracks\":[{\"id\":\"" + source.Id + "\"}]}");
        Pack("oversized", Manifest(source.Id) + new string(' ', 70 * 1024));
        var installed = CustomTracks.Installed(root, source.Id);
        Check(installed.Length == 1 && installed[0].Folder == good && installed[0].Bytes > 1000, "only a complete pack for this id is installed");
        Check(CustomTracks.Installed(Path.Combine(fixture, "missing"), source.Id).Length == 0, "a missing folder has no packs");

        // Source discovery requires the layout's AI line and the track model.
        var game = Path.Combine(fixture, "ac");
        var track = Path.Combine(game, "content", "tracks", source.SourceTrack);
        Directory.CreateDirectory(Path.Combine(track, source.Layout, "ai"));
        Check(CustomTracks.FindSource(source, [game]) is null, "an incomplete source is not offered");
        File.WriteAllBytes(Path.Combine(track, source.Layout, "ai", "fast_lane.ai"), [1]);
        File.WriteAllBytes(Path.Combine(track, source.SourceTrack + ".kn5"), [1]);
        Check(CustomTracks.FindSource(source, [Path.Combine(fixture, "none"), game]) == track, "the installed source is found");

        // A chosen folder may be the game folder or the track folder itself; anything else is refused.
        Check(CustomTracks.FindSource(source, [], game) == track && CustomTracks.FindSource(source, [], track) == track, "a chosen game or track folder is accepted");
        Check(CustomTracks.FindSource(source, [], Path.Combine(game, "content")) is null, "a folder without the track is not");

        var notInstalled = CustomTracks.Describe(new(source, null, null));
        Check(notInstalled.Contains("not installed") && notInstalled.Contains("Assetto Corsa") && notInstalled.Contains("choose its folder"), "missing source names the requirement and the folder choice");
        Check(CustomTracks.Describe(new(source, track, installed[0])).Contains("installed (") , "an installed pack shows its size");

        // Converter output → progress: monotonic, bounded, staged.
        var stages = new[] { "Checked source 17/17: x", "Reading 10.kn5", "Reading 3.kn5", "Course 20,695 m", "Collision: 1", "Converting scenery, materials and textures...", "Writing track tiles...", "Visuals: 180 tiles", "Route probe built; x", "unrelated line" };
        var percent = 0; var seen = new List<int>();
        foreach (var line in stages) { percent = CustomTracks.Percent(line, percent); seen.Add(percent); }
        Check(seen.SequenceEqual(seen.Order()) && seen[0] == 10 && seen[^1] == 96 && seen.All(p => p is >= 0 and <= 100), "progress is staged and never moves backwards");
        Check(CustomTracks.Percent("Checked source 0/17: a", 0) == 0 && CustomTracks.Percent("Checked source x/y: a", 5) == 5, "malformed counts keep the previous value");

        // The Launcher tab carries the DiRT2VR-style panel: toggle, track list, build actions.
        using var form = new MainForm(client, new ProfileStore(client.Root, () => { }, () => null));
        var launcherTab = ((TabControl)form.Controls.Find("LauncherTabs", true).Single()).TabPages.Cast<TabPage>().Single(p => p.Text == "Launcher");
        Check(form.SectionHeaders.All(h => h.Title != "Custom tracks"), "no separate Settings section");
        var toggle = (CheckBox)launcherTab.Controls.Find("CustomTracks", true).Single();
        Check(!toggle.Checked && launcherTab.Controls.Find("CustomTrackList", true).Length == 0, "off by default; the track browser is hidden");
        var eventRow = form.Controls.Find("PracticeEventRow", true).Single(); var trackRow = form.Controls.Find("PracticeTrackRow", true).Single();
        toggle.Checked = true;
        Check(new[] { "CustomTrackList", "BuildCustomTrack", "ChooseCustomTrackSource", "ManageCustomTrack", "StopCustomTrack", "CustomTrackStatus", "CustomTrackProgress" }
              .All(name => launcherTab.Controls.Find(name, true).Length == 1), "the CUSTOM tracks browser and actions are on the Launcher tab");
        var list = (ListBox)launcherTab.Controls.Find("CustomTrackList", true).Single();
        Check(list.Items.Cast<CustomTrackSource>().Select(s => s.Id).SequenceEqual(CustomTracks.Supported.Select(s => s.Id)), "the browser lists the supported tracks");
        Check(list.Parent!.Parent is CustomTrackPanel && !VerticalStack.IsShown(eventRow) && !VerticalStack.IsShown(trackRow), "on: the browser replaces the Event and Track rows");
        toggle.Checked = false;
        Check(VerticalStack.IsShown(eventRow) && VerticalStack.IsShown(trackRow) && list.Parent!.Parent is null, "off: the original selection returns");
        // Launch: Practice/Race load the custom track only while CUSTOM tracks is on.
        var choices = new Choices(Track: "circuits/brands_hatch/route_2") { LaunchMode = "Practice", CustomTracks = true };
        Check(choices.SessionTrack == source.Id && (choices with { CustomTracks = false }).SessionTrack == "circuits/brands_hatch/route_2" &&
              (choices with { LaunchMode = "Normal" }).SessionTrack == "circuits/brands_hatch/route_2", "session track follows the toggle and mode");
        var command = client.LaunchCommand(choices, false);
        var trackArgument = command.ArgumentList.IndexOf("-Track");
        Check(trackArgument > 0 && command.ArgumentList[trackArgument + 1] == source.Id, "Launch passes the custom track");
        Check(SessionClient.ParseChoices(System.Text.Json.JsonSerializer.Serialize(choices)) is { CustomTracks: true } parsed && parsed.CustomTrack == source.Id &&
              !System.Text.Json.JsonSerializer.Serialize(choices).Contains("SessionTrack"), "the toggle and selection persist");
        var invalid = false; try { SessionClient.Validate(choices with { CustomTrack = "circuits/brands_hatch/route_2" }); } catch (ArgumentException) { invalid = true; }
        Check(invalid, "only g2vr_ locations are custom tracks");
        Directory.Delete(root, true); Directory.Delete(game, true);
    }

    // GRID2VR.exe --preview-custom-tracks (development): run after the launcher's
    // own startup configuration so the window looks exactly as users see it.
    internal static int Preview()
    {
        var repository = SessionClient.FindRoot(AppContext.BaseDirectory);
        var fixture = Path.Combine(repository, "build", "launcher-preview-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(fixture);
        try { SavePreviews(repository, new SessionClient(fixture)); return 0; }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
        finally { Directory.Delete(fixture, true); }
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr hdc, uint flags);

    // Screenshots of the real launcher window with this PC's custom-track status and
    // installed content, idle and during a build. The window opens beyond every monitor,
    // never takes focus and runs none of its startup actions; it is captured by its own
    // handle (never the desktop) and closed. No input is sent.
    static void SavePreviews(string repository, SessionClient fixture)
    {
        var source = CustomTracks.Supported.Single();
        var status = CustomTracks.Status(repository, source);
        var catalog = Task.Run(() => new SessionClient(repository).Catalog()).GetAwaiter().GetResult();
        var notInstalled = status with { Installed = null };
        var states = new (string Name, CustomTrackStatus Status, bool Enabled, int? Percent, string? Line)[]
        {
            ("off", status, false, null, null),
            ("installed", status, true, null, null),
            ("not-installed", notInstalled, true, null, null),
            ("building", notInstalled, true, 55, "Collision: 880,256 triangles read back, 0 unmatched at 2 cm."),
            ("simple-installed", status, true, null, null),
        };
        foreach (var state in states)
        {
            using var form = new MainForm(fixture, new ProfileStore(fixture.Root, () => { }, () => null))
            {
                PreviewMode = true, ShowInTaskbar = false, StartPosition = FormStartPosition.Manual,
                Location = new Point(SystemInformation.VirtualScreen.Right + 400, SystemInformation.VirtualScreen.Top), ClientSize = new Size(1000, 1000),
            };
            LauncherAppearance.Modern = true; LauncherAppearance.Apply(form);
            form.Show();
            form.ShowCustomTrackPreview(state.Status, catalog.Cars, catalog.Tracks, "circuits/brands_hatch/route_2", state.Enabled, state.Percent, state.Line);
            if (catalog.Cars.FirstOrDefault(c => c.Id == "z28") is { } camaro) ((ComboBox)form.Controls.Find("PracticeCar", true).Single()).SelectedItem = camaro;
            if (state.Name.StartsWith("simple", StringComparison.Ordinal)) form.SelectSimplePreview(); else form.SelectPreviewTab("Launcher");
            for (int i = 0; i < 30; i++) { Application.DoEvents(); Thread.Sleep(20); }
            using var image = new Bitmap(form.Width, form.Height);
            using (var g = Graphics.FromImage(image)) { var hdc = g.GetHdc(); try { if (!PrintWindow(form.Handle, hdc, 2)) throw new Exception("Custom tracks: preview capture failed"); } finally { g.ReleaseHdc(hdc); } }
            image.Save(Path.Combine(repository, "build", $"launcher-custom-tracks-{state.Name}.png"));
            form.Close();
        }
    }
}
