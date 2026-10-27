namespace GRID2VR;

internal static class ChaseCameraTests
{
    internal static void Run(string root, SessionClient client)
    {
        var saved = client.Load();
        try
        {
            var defaults = new Choices();
            Require(!defaults.ChaseFreeLook && !defaults.ChaseKeepAngle && !defaults.ChaseInvertVertical && defaults.ChaseMouseMode == "right" &&
                defaults.ChaseMaxZoomOut == 180 && defaults.ChaseMouseSensitivity == 100 && defaults.ChaseStickSensitivity == 100, "Chase defaults changed.");
            Require(SessionClient.ParseChoices("{}") == (defaults with { ModernInterface = false, Graphics = new() }), "Older settings did not gain disabled chase defaults.");
            var chosen = defaults with { ChaseFreeLook = true, ChaseKeepAngle = true, ChaseMouseMode = "always", ChaseMaxZoomOut = 1000,
                ChaseMouseSensitivity = 25, ChaseStickSensitivity = 300, ChaseInvertVertical = true };
            client.Save(chosen); Require(client.Load() == chosen, "Chase preferences did not persist.");
            foreach (var mode in new[] { "Normal", "Practice", "Race" })
                foreach (var vr in new[] { false, true })
                    foreach (var mouseMode in new[] { "left", "right", "always" })
                    {
                        var arguments = client.LaunchCommand(chosen with { LaunchMode = mode, ChaseMouseMode = mouseMode }, vr).ArgumentList;
                        foreach (var (key, value) in new[] { ("ChaseFreeLook", "1"), ("ChaseKeepAngle", "1"), ("ChaseMouseMode", mouseMode),
                            ("ChaseMaxZoomOut", "1000"), ("ChaseMouseSensitivity", "25"), ("ChaseStickSensitivity", "300"), ("ChaseInvertVertical", "1") })
                            Require(arguments[arguments.IndexOf("-" + key) + 1] == value, "Launch route lost " + key);
                    }
            foreach (var invalid in new[] { chosen with { ChaseMouseMode = "unknown" }, chosen with { ChaseMaxZoomOut = 170 }, chosen with { ChaseMaxZoomOut = 181 },
                chosen with { ChaseMaxZoomOut = 1010 }, chosen with { ChaseMouseSensitivity = 24 }, chosen with { ChaseMouseSensitivity = 301 },
                chosen with { ChaseStickSensitivity = 24 }, chosen with { ChaseStickSensitivity = 301 } })
            {
                var rejected = false; try { SessionClient.Validate(invalid); } catch (ArgumentException) { rejected = true; }
                Require(rejected, "Invalid chase preferences accepted.");
            }
            foreach (var modern in new[] { false, true })
                foreach (var dpi in new[] { 96, 144, 192 })
                {
                    using var panel = new ChaseCameraPanel();
                    using var font = new Font("Segoe UI", 10f * dpi / 96f);
                    panel.Font = font; panel.Dock = DockStyle.None; panel.Visible = true;
                    LauncherAppearance.Modern = modern; LauncherAppearance.Apply(panel); panel.LoadChoices(chosen);
                    var toggle = (CheckBox)panel.Controls.Find("ChaseFreeLook", true).Single();
                    toggle.Checked = false;
                    Require(panel.ApplyTo(defaults) == (chosen with { ChaseFreeLook = false }), "Disabling chase discarded subordinate preferences.");
                    foreach (var name in new[] { "ChaseKeepAngle", "ChaseMouseMode", "ChaseMaxZoomOut", "ChaseMouseSensitivity", "ChaseStickSensitivity", "ChaseInvertVertical" })
                        Require(!panel.Controls.Find(name, true).Single().Enabled, "Disabled chase control still enabled: " + name);
                    toggle.Checked = true;
                    var picker = (MouseLookPicker)panel.Controls.Find("ChaseMouseMode", true).Single();
                    var inversion = (CheckBox)panel.Controls.Find("ChaseInvertVertical", true).Single();
                    foreach (var value in new[] { "left", "right", "always" })
                    {
                        ((RadioButton)picker.Controls.Find("ChaseMouse_" + value, true).Single()).Checked = true;
                        foreach (var inverted in new[] { false, true })
                        {
                            inversion.Checked = inverted;
                            Require(picker.Controls.OfType<RadioButton>().Count(c => c.Checked) == 1 && picker.SelectedMode == value, "Mouse mode selection failed.");
                            client.Save(panel.ApplyTo(defaults));
                            using var reopened = new ChaseCameraPanel();
                            reopened.LoadChoices(client.Load());
                            var restored = reopened.ApplyTo(defaults);
                            Require(restored == (chosen with { ChaseMouseMode = value, ChaseInvertVertical = inverted }), "Mouse mode/inversion UI choices did not survive saving and reopening.");
                            var arguments = client.LaunchCommand(restored, false).ArgumentList;
                            Require(arguments[arguments.IndexOf("-ChaseMouseMode") + 1] == value &&
                                arguments[arguments.IndexOf("-ChaseInvertVertical") + 1] == (inverted ? "1" : "0"), "Saved mouse mode/inversion did not reach the session worker.");
                        }
                    }
                    foreach (var logicalWidth in new[] { 460, 820, 1320 })
                    {
                        panel.Width = logicalWidth * dpi / 96; panel.PerformLayout();
                        foreach (var row in panel.Controls.OfType<SettingRow>())
                            Require(row.Bottom <= panel.Height && row.Width <= panel.Width && !row.Controls[0].Bounds.IntersectsWith(row.Controls[1].Bounds), "Chase row clipped or overlapped.");
                        foreach (Control button in picker.Controls)
                            Require(button.Width > 0 && button.Right <= picker.Width && button.Bottom <= picker.Height && ((RadioButton)button).Image != null, "Mouse illustration clipped or missing.");
                        using var image = new Bitmap(panel.Width, panel.Height); panel.DrawToBitmap(image, panel.ClientRectangle);
                        if (logicalWidth is 460 or 1320) image.Save(Path.Combine(root, "build", $"launcher-chase-{logicalWidth}-{dpi}-{(modern ? "modern" : "classic")}.png"));
                    }
                    Require(panel.ApplyTo(defaults) == chosen, "Layout changed chase preferences.");
                }
        }
        finally { client.Save(saved); }
    }
    private static void Require(bool condition, string reason) { if (!condition) throw new InvalidOperationException(reason); }
}
