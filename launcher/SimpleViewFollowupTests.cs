using System.Reflection;
using System.Text.Json;

namespace GRID2VR;

internal static class SimpleViewFollowupTests
{
    private const BindingFlags Flags = BindingFlags.Instance | BindingFlags.NonPublic;
    private static void Check(bool value, [System.Runtime.CompilerServices.CallerArgumentExpression(nameof(value))] string? text = null)
    { if (!value) throw new InvalidOperationException("Simple view follow-up regression: " + text); }
    private static void Click(Control button) => typeof(Button).GetMethod("OnClick", Flags)!.Invoke(button, [EventArgs.Empty]);

    internal static void Run(string root, SessionClient fixture)
    {
        var evidence = new List<object>();
        foreach (var modern in new[] { false, true })
        {
            var client = new SessionClient(Path.Combine(fixture.Root, "simple-confirmation-" + modern));
            var approved = false; var confirmations = 0;
            using var form = new MainForm(client, new ProfileStore(client.Root, () => { }, () => null),
                approveAdvancedView: () => { ++confirmations; return approved; });
            LauncherAppearance.Modern = modern; LauncherAppearance.Apply(form);
            LaunchModeTests.RunForm(form);
            object? Call(string name, params object[] args) => typeof(MainForm).GetMethod(name, Flags)!.Invoke(form, args);
            Choices Current() => (Choices)Call("Current")!;
            var picker = (LauncherViewPicker)form.Controls.Find("LauncherView", true).Single();
            var tabs = (TabControl)form.Controls.Find("LauncherTabs", true).Single();
            var driver = (Button)form.Controls.Find("DrivingControls", true).Single();
            var driverMargin = driver.Margin; var driverAutoSize = driver.AutoSize;
            picker.Advanced = false; picker.Advanced = true; picker.Advanced = false;
            Check(confirmations == 0); // Restoring initial preferences never prompts.
            var seat = (Button)form.Controls.Find("SimpleSeat", true).Single();
            var profiles = (Button)form.Controls.Find("SimpleProfiles", true).Single();
            var actions = (UniformButtonColumn)form.Controls.Find("SimpleComfortActions", true).Single();
            Check(actions.Controls.Cast<Control>().SequenceEqual(new Control[] { driver, seat, profiles }));
            var before = Current(); client.Save(before); client.SaveWindow(new(AdvancedView: false));
            var settingsPath = Path.Combine(client.Artifacts, "launcher-settings.json");
            var windowPath = Path.Combine(client.Artifacts, "launcher-window.json");
            typeof(MainForm).GetField("windowLoaded", Flags)!.SetValue(form, true);

            foreach (var origin in new[] { "header", "seat", "profiles" })
            {
                void Request() { if (origin == "header") picker.Advanced = true; else Click(origin == "seat" ? seat : profiles); }
                var settings = File.ReadAllBytes(settingsPath); var view = File.ReadAllBytes(windowPath);
                var page = tabs.SelectedTab; var count = confirmations;
                approved = false; Request();
                Check(confirmations == count + 1 && !picker.Advanced && tabs.TabCount == 1 && ReferenceEquals(tabs.SelectedTab, page));
                Check(Current() == before && settings.SequenceEqual(File.ReadAllBytes(settingsPath)) && view.SequenceEqual(File.ReadAllBytes(windowPath)));
                Check(actions.Contains(driver) && actions.Contains(seat) && actions.Contains(profiles));
                approved = true; Request();
                Check(confirmations == count + 2 && picker.Advanced && tabs.TabCount == 6 && Current() == before);
                Check(client.LoadWindow().AdvancedView == true && settings.SequenceEqual(File.ReadAllBytes(settingsPath)));
                Check(driver.Margin == driverMargin && driver.AutoSize == driverAutoSize);
                if (origin == "seat")
                {
                    Check(tabs.SelectedTab!.Text == "Controls");
                    Check(((TabControl)form.Controls.Find("ControlTabs", true).Single()).SelectedTab!.Text == "Seat");
                }
                if (origin == "profiles") Check(tabs.SelectedTab!.Text == "Profile");
                picker.Advanced = false;
                Check(confirmations == count + 2 && !client.LoadWindow().AdvancedView!.Value && Current() == before);
                evidence.Add(new { modern, origin, cancelPreservesState = true, approvalNavigates = true, promptsOnce = true });
            }

            var parent = actions.Parent!; var index = parent.Controls.GetChildIndex(actions);
            actions.Font = form.Font; parent.Controls.Remove(actions);
            try
            {
                actions.Visible = true; actions.Dock = DockStyle.None;
                foreach (var width in new[] { 120, 240, 460 })
                {
                    actions.Width = width; actions.PerformLayout(); AssertButtons(actions);
                }
                actions.Width = 240; actions.PerformLayout(); actions.CreateControl();
                using var bitmap = new Bitmap(actions.Width, actions.Height);
                actions.DrawToBitmap(bitmap, actions.ClientRectangle);
                bitmap.Save(Path.Combine(root, "build", $"launcher-comfort-buttons-{(modern ? "modern" : "classic")}.png"));
            }
            finally { parent.Controls.Add(actions); parent.Controls.SetChildIndex(actions, index); }
            Check(!form.Visible);

            using var dialog = new AdvancedViewConfirmationForm();
            LauncherAppearance.Apply(dialog);
            Check(((Button)dialog.AcceptButton!).Text == "Approve" && dialog.AcceptButton.DialogResult == DialogResult.OK);
            Check(((Button)dialog.CancelButton!).Text == "Cancel" && dialog.CancelButton.DialogResult == DialogResult.Cancel);
            Check(dialog.Controls.Find("AdvancedViewMessage", true).Single().Text.StartsWith("This switches to Advanced mode."));
            var body = dialog.Content; body.Font = dialog.Font; dialog.Controls.Remove(body);
            try
            {
                body.Dock = DockStyle.None; body.Visible = true; body.Size = dialog.ClientSize; body.PerformLayout(); body.CreateControl();
                var buttons = body.Controls.Find("AdvancedViewConfirmationActions", true).Single();
                Check(buttons.Controls.Cast<Control>().All(c => buttons.ClientRectangle.Contains(c.Bounds)));
                using var image = new Bitmap(body.Width, body.Height);
                body.DrawToBitmap(image, body.ClientRectangle);
                image.Save(Path.Combine(root, "build", $"launcher-advanced-confirmation-{(modern ? "modern" : "classic")}.png"));
            }
            finally { dialog.Controls.Add(body); }
            Check(!dialog.Visible);
        }

        foreach (var dpi in new[] { 96, 144, 192 })
        {
            using var font = new Font("Segoe UI", 10f * dpi / 96f);
            using var actions = new DpiButtons(dpi) { Font = font, Width = 240 * dpi / 96 };
            actions.Controls.AddRange(new Control[] { new Button { Text = "Driving bindings…", AutoSize = true },
                new Button { Text = "Seat adjustment…", AutoSize = true }, new Button { Text = "Career profiles…", AutoSize = true } });
            actions.PerformLayout(); AssertButtons(actions);
            actions.Controls[2].Text = "A much longer career profiles label"; actions.PerformLayout(); AssertButtons(actions);
            evidence.Add(new { dpi, equalSize = true, aligned = true, boundsValid = true });
        }
        File.WriteAllText(Path.Combine(root, "build", "launcher-simple-followup.json"), JsonSerializer.Serialize(new
        { windowsShown = 0, gameLaunched = false, checks = evidence }, new JsonSerializerOptions { WriteIndented = true }));
    }

    private sealed class DpiButtons(int dpi) : UniformButtonColumn("DpiComfort") { protected override int LayoutDpi => dpi; }

    private static void AssertButtons(UniformButtonColumn actions)
    {
        var buttons = actions.Controls.Cast<Control>().ToArray();
        Check(buttons.Length == 3 && buttons.Select(b => b.Size).Distinct().Count() == 1 && buttons.Select(b => b.Left).Distinct().Count() == 1);
        Check(buttons.All(b => actions.ClientRectangle.Contains(b.Bounds)));
        Check(buttons[0].Bottom < buttons[1].Top && buttons[1].Bottom < buttons[2].Top);
    }
}
