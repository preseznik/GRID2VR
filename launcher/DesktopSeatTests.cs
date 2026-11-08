using System.Text.Json;

namespace GRID2VR;

internal static class DesktopSeatTests
{
    internal static void Run(string fixture)
    {
        static void Check(bool value, string reason) { if (!value) throw new Exception("Desktop seat: " + reason); }
        var root = Path.Combine(fixture, "desktop-seat");
        Directory.CreateDirectory(AppPaths.Data(root));
        var path = Path.Combine(AppPaths.Data(root), "seat-positions.json");
        File.WriteAllText(path, "{\"Version\":1,\"Cars\":{\"s20\":{\"Height\":-10,\"Forward\":2,\"Right\":3}}}");
        var seats = SeatAlignment.Load(root);
        Check(!seats.ApplyToDesktop && seats.DesktopFov == 0 && seats.InGameSeat && seats.Cars["s20"].Height == -10, "legacy VR settings retain their values and Desktop stays off");
        var client = new SessionClient(root);
        var choices = new Choices();
        var vrBefore = client.LaunchCommand(choices, true).ArgumentList.ToArray();
        Check(!client.LaunchCommand(choices, false).ArgumentList.Contains("-CockpitMode"), "default Desktop launch retains original cameras");
        using (var panel = new SeatAlignmentPanel())
        {
            panel.LoadCars([new("s20", "Honda"), new("pag", "Pagani")], "s20", seats);
            ((CheckBox)panel.Controls.Find("DesktopSeatPosition", true).Single()).Checked = true;
            ((ValueSlider)panel.Controls.Find("Desktop cockpit FOV", true).Single()).Value = 10;
            ((CheckBox)panel.Controls.Find("InGameSeatAdjustment", true).Single()).Checked = false;
            var car = (ComboBox)panel.Controls.Find("SeatCar", true).Single();
            car.SelectedIndex = 1; car.SelectedIndex = 0;
            panel.Save(root);
            var committed = SeatAlignment.Load(root); committed.Cars["s20"] = new SeatOffset(-12, 2, 3); committed.DesktopFov = 15; committed.Save(root);
            panel.Reload(root); panel.Save(root);
            Check(SeatAlignment.Load(root).Cars["s20"].Height == -12 && SeatAlignment.Load(root).DesktopFov == 15, "session commit reload is retained by the next Save settings");
        }
        seats = SeatAlignment.Load(root);
        Check(seats.ApplyToDesktop && seats.DesktopFov == 15 && !seats.InGameSeat && seats.Cars["s20"] == new SeatOffset(-12, 2, 3), "Desktop options and in-game preference persist independently of the selected car");
        seats.DesktopFov = 10; seats.Save(root);
        foreach (var mode in new[] { "Normal", "Online", "Practice", "Race" })
        {
            var command = client.LaunchCommand(choices with { LaunchMode = mode, VrCockpit = false }, false).ArgumentList.ToArray();
            string Value(string name) => command[Array.IndexOf(command, name) + 1];
            Check(Value("-CockpitMode") == "HeadOnly" && Value("-DesktopSeatPosition") == "1" && Value("-DesktopCockpitFov") == "10", "Desktop options reach " + mode + " even with VR cockpit off");
        }
        Check(client.LaunchCommand(choices, true).ArgumentList.SequenceEqual(vrBefore), "Desktop options do not change any VR launch argument");
        seats.ApplyToDesktop = false; seats.DesktopFov = -10; seats.Save(root);
        var fovOnly = client.LaunchCommand(choices, false).ArgumentList.ToArray();
        Check(fovOnly[Array.IndexOf(fovOnly, "-DesktopSeatPosition") + 1] == "0" && fovOnly.Contains("-DesktopCockpitFov"), "FOV works without seat offsets");
        seats.DesktopFov = 0; seats.Save(root);
        Check(!client.LaunchCommand(choices, false).ArgumentList.Contains("-CockpitMode"), "turning both off restores the original Desktop route");
        using (var form = new MainForm(client, new ProfileStore(root, () => { }, () => null)))
        {
            var cockpit = (CheckBox)form.Controls.Find("VrCockpit", true).Single();
            var seatPanel = form.Controls.Find("CockpitAlignment", true).Single();
            Check(!seatPanel.Enabled, "seat tool waits for a loaded, idle session");
            cockpit.Enabled = seatPanel.Enabled = true;
            cockpit.Checked = false;
            Check(seatPanel.Enabled, "turning VR cockpit off keeps the Desktop seat tool enabled");
            cockpit.Checked = true;
            Check(seatPanel.Enabled, "turning VR cockpit on keeps the Desktop seat tool enabled");
        }
        foreach (var value in new[] { "31", "-31", "1.5", "true", "\"10\"" })
        {
            File.WriteAllText(path, "{\"Version\":1,\"Cars\":{},\"DesktopFov\":" + value + "}");
            var refused = false;
            try { SeatAlignment.Load(root); } catch (Exception error) when (error is IOException or JsonException) { refused = true; }
            Check(refused, "invalid Desktop FOV rejected: " + value);
        }
    }
}
