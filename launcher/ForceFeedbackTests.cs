namespace GRID2VR;

internal static class ForceFeedbackTests
{
    internal static void Run(string root, SessionClient client)
    {
        var saved = client.Load();
        try
        {
            var defaults = new Choices();
            Require(!defaults.ForceFeedbackOverride && defaults.ForceFeedbackStrength == 100 && defaults.ForceFeedbackWeight == 50 && defaults.ForceFeedbackEffects == 50,
                "Force feedback defaults no longer match GRID 2's (off; strength 100%, weight 50%, vibration 50%).");
            Require(SessionClient.ParseChoices("{}") == (defaults with { ModernInterface = false, Graphics = new() }), "Older settings did not gain force feedback defaults.");
            var chosen = defaults with { ForceFeedbackOverride = true, ForceFeedbackStrength = 0, ForceFeedbackWeight = 1, ForceFeedbackEffects = 100 };
            client.Save(chosen); Require(client.Load() == chosen, "Force feedback preferences did not persist.");
            // Every mode, VR and Desktop; nothing at all while the override is off.
            foreach (var mode in new[] { "Normal", "Online", "Practice", "Race" })
                foreach (var vr in new[] { false, true })
                {
                    var arguments = client.LaunchCommand(chosen with { LaunchMode = mode }, vr).ArgumentList;
                    var at = arguments.IndexOf("-ForceFeedback");
                    Require(at >= 0 && arguments[at + 1] == "0,1,100" && arguments.Count(a => a == "-ForceFeedback") == 1, $"Force feedback lost in {mode} {(vr ? "VR" : "Desktop")}.");
                    Require(!client.LaunchCommand(chosen with { LaunchMode = mode, ForceFeedbackOverride = false }, vr).ArgumentList.Contains("-ForceFeedback"),
                        "Force feedback sent while launcher settings are off.");
                }
            foreach (var invalid in new[] { chosen with { ForceFeedbackStrength = -1 }, chosen with { ForceFeedbackStrength = 101 }, chosen with { ForceFeedbackWeight = 0 },
                chosen with { ForceFeedbackWeight = 101 }, chosen with { ForceFeedbackEffects = -1 }, chosen with { ForceFeedbackEffects = 101 } })
            {
                var rejected = false; try { SessionClient.Validate(invalid); } catch (ArgumentException) { rejected = true; }
                Require(rejected, "Invalid force feedback settings accepted.");
            }
            foreach (var modern in new[] { false, true })
                foreach (var dpi in new[] { 96, 144, 192 })
                {
                    using var panel = new ForceFeedbackPanel();
                    using var font = new Font("Segoe UI", 10f * dpi / 96f);
                    panel.Font = font; panel.Dock = DockStyle.None; panel.Visible = true;
                    LauncherAppearance.Modern = modern; LauncherAppearance.Apply(panel); panel.LoadChoices(chosen);
                    var toggle = (CheckBox)panel.Controls.Find("ForceFeedbackOverride", true).Single();
                    Require(toggle.Text == "Use launcher force feedback", "Force feedback toggle renamed.");
                    toggle.Checked = false;
                    Require(panel.ApplyTo(defaults) == (chosen with { ForceFeedbackOverride = false }), "Turning launcher force feedback off discarded its values.");
                    foreach (var name in new[] { "ForceFeedbackWeight", "ForceFeedbackStrength", "ForceFeedbackEffects" })
                        Require(!panel.Controls.Find(name, true).Single().Enabled, "Force feedback slider enabled while off: " + name);
                    toggle.Checked = true;
                    ((ValueSlider)panel.Controls.Find("ForceFeedbackWeight", true).Single()).Value = 75;
                    ((ValueSlider)panel.Controls.Find("ForceFeedbackStrength", true).Single()).Value = 60;
                    ((ValueSlider)panel.Controls.Find("ForceFeedbackEffects", true).Single()).Value = 20;
                    client.Save(panel.ApplyTo(defaults));
                    using (var reopened = new ForceFeedbackPanel())
                    {
                        reopened.LoadChoices(client.Load());
                        var restored = reopened.ApplyTo(defaults);
                        Require(restored == (defaults with { ForceFeedbackOverride = true, ForceFeedbackWeight = 75, ForceFeedbackStrength = 60, ForceFeedbackEffects = 20 }),
                            "Force feedback UI choices did not survive saving and reopening.");
                        var arguments = client.LaunchCommand(restored, true).ArgumentList;
                        Require(arguments[arguments.IndexOf("-ForceFeedback") + 1] == "60,75,20", "Saved force feedback did not reach the session worker in strength,weight,effects order.");
                    }
                    panel.LoadChoices(chosen);
                    foreach (var logicalWidth in new[] { 460, 820, 1320 })
                    {
                        panel.Width = logicalWidth * dpi / 96; panel.PerformLayout();
                        foreach (var row in panel.Controls.OfType<SettingRow>())
                            Require(row.Bottom <= panel.Height && row.Width <= panel.Width && !row.Controls[0].Bounds.IntersectsWith(row.Controls[1].Bounds), "Force feedback row clipped or overlapped.");
                        var notice = panel.Controls.Find("ForceFeedbackNotice", true).Single();
                        Require(notice.Right <= panel.Width && notice.Bottom <= panel.Height && !((Label)notice).UseMnemonic, "Force feedback notice clipped, or its & hidden.");
                        using var image = new Bitmap(panel.Width, panel.Height); panel.DrawToBitmap(image, panel.ClientRectangle);
                        if (logicalWidth is 460 or 1320) image.Save(Path.Combine(root, "build", $"launcher-force-feedback-{logicalWidth}-{dpi}-{(modern ? "modern" : "classic")}.png"));
                    }
                    Require(panel.ApplyTo(defaults) == chosen, "Layout changed force feedback preferences.");
                }
        }
        finally { client.Save(saved); }
    }
    private static void Require(bool condition, string reason) { if (!condition) throw new InvalidOperationException(reason); }
}
