using System.Text.Json;

namespace GRID2VR;

internal static class ResponsiveLayoutTests
{
    internal static void Run(string root, SessionClient fixture)
    {
        var evidence = new List<object>();
        using (var preparation = new PreparationForm(fixture.Root))
        {
            var content = preparation.Content; preparation.Controls.Remove(content);
            content.Dock = DockStyle.None; content.Visible = true; content.Size = preparation.ClientSize;
            preparation.DisplayProgress(new(54, "Preparing cockpit camera 23 of 51: Honda S2000"));
            content.PerformLayout();
            using var image = new Bitmap(content.Width, content.Height);
            content.DrawToBitmap(image, content.ClientRectangle);
            image.Save(Path.Combine(root, "build", "launcher-preparation.png"));
            Require(!preparation.Visible, "Preparation fixture displayed a window.");
            Require(content.Controls.OfType<ProgressBar>().Single().Bounds.Top >= 55, "Preparation progress overlaps the step label.");
            preparation.Controls.Add(content);
        }
        foreach (var modern in new[] { false, true })
        {
            using(var about=new AboutForm(fixture.Root,121,0))
            {
                LauncherAppearance.Modern=modern;LauncherAppearance.Apply(about);
                var helpTabs=(TabControl)about.Controls.Find("HelpTabs",true).Single();
                var aboutContent=helpTabs.TabPages[0].Controls[0];helpTabs.TabPages[0].Controls.Remove(aboutContent);
                try {
                    aboutContent.Dock=DockStyle.None;aboutContent.Visible=true;aboutContent.Font=about.Font;
                    foreach(var width in new[]{520,820}) {
                        aboutContent.Width=width;aboutContent.PerformLayout();aboutContent.Height=aboutContent.GetPreferredSize(new Size(width,0)).Height;aboutContent.PerformLayout();
                        Require(Descendants(aboutContent).OfType<Button>().Any(b=>b.Text=="Release notes"),"About release notes missing.");
                        Require(Descendants(aboutContent).OfType<CheckBox>().Any(b=>b.Text=="Include experimental releases"),"About release channel control missing.");
                        using var image=new Bitmap(width,aboutContent.Height);aboutContent.DrawToBitmap(image,aboutContent.ClientRectangle);
                        image.Save(Path.Combine(root,"build",$"launcher-about-{width}-{(modern?"modern":"classic")}.png"));
                        Require(!about.Visible,"About fixture displayed a window.");
                    }
                } finally{helpTabs.TabPages[0].Controls.Add(aboutContent);}
            }
            var profileStore = new ProfileStore(fixture.Root, () => { }, () => null);
            using var form = new MainForm(fixture, profileStore);
            ((ProfilePanel)form.Controls.Find("ProfilePanel", true).Single()).Reload();
            LauncherAppearance.Modern = modern; LauncherAppearance.Apply(form);
            LaunchModeTests.RunForm(form);
            var tabs = (TabControl)form.Controls.Find("LauncherTabs", true).Single();
            var controlTabs = (TabControl)form.Controls.Find("ControlTabs", true).Single();
            Require(controlTabs.TabPages.Cast<TabPage>().Select(p => p.Text).SequenceEqual(new[] { "General", "VR", "Driving", "Seat" }), "Controls subtabs changed.");
            Require(controlTabs.TabPages[1].Controls.Find("KeepHorizonLevel", true).Single() is CheckBox { Checked: true }, "Horizon default missing from VR controls.");
            Require(controlTabs.TabPages[2].Controls.Find("DrivingControls", true).Length == 1, "Driving settings are in the wrong subtab.");
            Require(Descendants(controlTabs.TabPages[3]).OfType<SeatAlignmentPanel>().Count() == 1, "Seat settings are in the wrong subtab.");
            var controlParent = controlTabs.Parent!;
            controlParent.Controls.Remove(controlTabs);
            try
            {
                controlTabs.Visible = true; controlTabs.Dock = DockStyle.None;
                controlTabs.Font = form.Font; controlTabs.SelectedIndex = 1; controlTabs.CreateControl();
                foreach (var width in new[] { 460, 1320 })
                {
                    controlTabs.Size = new Size(width, 600); controlTabs.PerformLayout();
                    for (var i = 0; i < controlTabs.TabCount; ++i)
                        Require(controlTabs.GetTabRect(i).Right <= width, "Controls subtab is outside the compact window.");
                    using var bitmap = new Bitmap(width, 600);
                    controlTabs.DrawToBitmap(bitmap, controlTabs.ClientRectangle);
                    bitmap.Save(Path.Combine(root, "build", $"launcher-control-tabs-{width}-{(modern ? "modern" : "classic")}.png"));
                    Require(!form.Visible, "Controls preview opened a window.");
                }
            }
            finally { controlTabs.Dock = DockStyle.Fill; controlParent.Controls.Add(controlTabs); }
            foreach (TabPage page in tabs.TabPages.Cast<TabPage>().Where(p => p.Text != "Controls").Concat(controlTabs.TabPages.Cast<TabPage>()))
            {
                var content = page.Controls[0]; content.Font = form.Font; page.Controls.Remove(content);
                try
                {
                    content.Visible = true; content.Dock = DockStyle.None;
                    var controls = Descendants(content).ToArray();
                    var choices = controls.OfType<ComboBox>().Select(c => (Control: c, Value: c.SelectedItem)).ToArray();
                    var compactHeight = 0;
                    foreach (var width in new[] { 460, 820, 1320, 460 })
                    {
                        content.Size = new Size(width, page.Text == "Advanced" ? 600 : content.Height);
                        content.PerformLayout();
                        AssertBounds(content);
                        foreach (var choice in choices) Require(ReferenceEquals(choice.Control.SelectedItem, choice.Value), "Resizing changed a choice.");
                        Require(Descendants(content).SequenceEqual(controls), "Resizing recreated/reordered controls.");
                        var columns = controls.OfType<ResponsiveColumns>().SingleOrDefault();
                        if (columns != null)
                        {
                            var wide = width >= ResponsiveColumns.WideAt;
                            Require(wide ? columns.Second.Left > columns.First.Right && columns.Second.Top == 0
                                : columns.Second.Left == 0 && columns.Second.Top == columns.First.Bottom, "Wrong column flow.");
                        }
                        if (width == 460)
                        {
                            if (compactHeight == 0) compactHeight = content.Height;
                            else Require(content.Height == compactHeight, "Returning to compact retained stale row heights.");
                        }
                        var rows = controls.OfType<SettingRow>().ToArray();
                        evidence.Add(new { page = page.Text, modern, width, height = content.Height,
                            columns = columns?.Second.Left > 0 ? 2 : 1, rows = rows.Length,
                            minimumEditorWidth = rows.Select(r => r.Controls[1].Width).DefaultIfEmpty(0).Min(), boundsValid = true });
                        if (width is 460 or 1320 && page.Text is "Launcher" or "Profile" or "Graphics" or "Settings" or "Advanced" or "VR" or "Seat" or "Driving" or "General")
                        {
                            // Native combo text is omitted by this offscreen capture;
                            // RunForm checks the selected text and state separately.
                            using var bitmap = new Bitmap(content.Width, content.Height);
                            content.DrawToBitmap(bitmap, content.ClientRectangle);
                            bitmap.Save(Path.Combine(root, "build", $"launcher-responsive-{page.Text.ToLowerInvariant()}-{width}-{(modern ? "modern" : "classic")}.png"));
                        }
                    }
                }
                finally { page.Controls.Add(content); }
            }
            var footer = Descendants(form).OfType<LauncherFooter>().Single();
            var footerParent = footer.Parent!; var footerIndex = footerParent.Controls.GetChildIndex(footer);
            footer.Font = form.Font; footerParent.Controls.Remove(footer); footer.Dock = DockStyle.None;
            try
            {
                foreach (var width in new[] { 460, 820, 1320, 460 })
                {
                    footer.Width = width; footer.PerformLayout();
                    var buttons = footer.Controls.Cast<Control>().ToArray();
                    foreach (var button in buttons)
                        Require(button.Left >= 0 && button.Top >= 0 && button.Right <= width && button.Bottom <= footer.Height,
                            $"Footer action {button.Text} clipped in {footer.ClientSize}: {button.Bounds}");
                    for (var a = 0; a < buttons.Length; ++a)
                        for (var b = a + 1; b < buttons.Length; ++b)
                            Require(!buttons[a].Bounds.IntersectsWith(buttons[b].Bounds), "Footer actions overlap.");
                }
            }
            finally { footer.Dock = DockStyle.Fill; footerParent.Controls.Add(footer); footerParent.Controls.SetChildIndex(footer, footerIndex); }
            Require(!form.Visible, "The layout fixture must never show a window.");
            using var profileDialog = new CreateProfileForm(profileStore);
            LauncherAppearance.Apply(profileDialog);
            var profileBody = profileDialog.Controls[0];
            profileBody.Font = profileDialog.Font; profileDialog.Controls.Remove(profileBody);
            try
            {
                profileBody.Visible = true; profileBody.Width = profileDialog.ClientSize.Width; profileBody.PerformLayout();
                profileBody.CreateControl();
                foreach (var child in Descendants(profileBody)) child.CreateControl();
                var profileName = (TextBox)profileBody.Controls.Find("ProfileName", true).Single();
                profileName.Text = "Driver1";
                Require(profileName.Visible && profileName.Width > 100 && profileName.Height > 15, "Profile name editor is not usable.");
                AssertBounds(profileBody);
                using var profilePreview = new Bitmap(profileDialog.ClientSize.Width, profileBody.Height);
                profileBody.DrawToBitmap(profilePreview, profileBody.ClientRectangle);
                profilePreview.Save(Path.Combine(root, "build", $"launcher-profile-create-{(modern ? "modern" : "classic")}.png"));
            }
            finally { profileDialog.Controls.Add(profileBody); }
            Require(!profileDialog.Visible, "The profile dialog fixture must never show a window.");
        }
        // Explicit DPI-scaled component layout: no display changes or window messages.
        foreach (var dpi in new[] { 96, 144, 192 })
        {
            using var font = new Font("Segoe UI", 10f * dpi / 96f);
            using var columns = new DpiColumns(dpi) { Font = font };
            var slider = new RenderScaleSlider { Value = 237 };
            var editor = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
            editor.Items.Add("A deliberately long course and route name"); editor.SelectedIndex = 0;
            columns.First.Controls.Add(new DpiRow("Render resolution", slider, dpi));
            columns.Second.Controls.Add(new DpiRow("Track", editor, dpi));
            foreach (var logicalWidth in new[] { 460, 820, 1320, 460 })
            {
                columns.Width = (int)Math.Round(logicalWidth * dpi / 96.0); columns.PerformLayout();
                AssertBounds(columns);
                Require(slider.Value == 237 && editor.SelectedIndex == 0, "DPI reflow changed values.");
                Require((columns.Second.Left > 0) == (logicalWidth >= ResponsiveColumns.WideAt), "Column breakpoint did not scale with DPI.");
                var row = columns.First.Controls[0]; var caption = row.Controls[0];
                Require(row.Width < 590 * dpi / 96.0 ? slider.Top >= caption.Bottom : slider.Left > caption.Right, "Editor breakpoint did not scale with DPI.");
                evidence.Add(new { dpi, logicalWidth, width = columns.Width, height = columns.Height, boundsValid = true });
            }
            var readout = new Label { Text = "Waiting" };
            using var statusRow = new DpiRow("Measured scene", readout, dpi) { Font = font, Width = (int)Math.Round(460 * dpi / 96.0) };
            statusRow.PerformLayout(); var shortHeight = statusRow.Height;
            readout.Text = string.Join(" ", Enumerable.Repeat("Current session: scene mismatch requested 3840 × 2160.", 6));
            AssertBounds(statusRow); Require(statusRow.Height > shortHeight, "Updated status text did not reflow.");
            readout.Text = "Waiting";
            Require(statusRow.Height == shortHeight, "Shorter status text retained stale height.");
        }
        File.WriteAllText(Path.Combine(root, "build", "launcher-responsive-layout.json"), JsonSerializer.Serialize(evidence, new JsonSerializerOptions { WriteIndented = true }));
    }

    private static IEnumerable<Control> Descendants(Control parent)
    {
        foreach (Control child in parent.Controls)
        {
            yield return child;
            foreach (var descendant in Descendants(child)) yield return descendant;
        }
    }
    private static void AssertBounds(Control parent)
    {
        foreach (Control child in parent.Controls)
        {
            if (parent is VerticalStack or ResponsiveColumns or SettingRow)
                Require(child.Left >= 0 && child.Top >= 0 && child.Right <= parent.ClientSize.Width && child.Bottom <= parent.ClientSize.Height,
                    $"{parent.GetType().Name}/{child.Name} escaped {parent.ClientSize}: {child.Bounds}");
            if (child is SettingRow row)
                Require(!row.Controls[0].Bounds.IntersectsWith(row.Controls[1].Bounds), "Caption overlaps its editor.");
            AssertBounds(child);
        }
        if (parent is VerticalStack)
        {
            var previousBottom = 0;
            foreach (Control child in parent.Controls)
            {
                Require(child.Top >= previousBottom, "Stacked controls overlap."); previousBottom = child.Bottom;
            }
        }
    }
    private sealed class DpiRow(string title, Control editor, int dpi) : SettingRow(title, editor)
    { protected override int LayoutDpi => dpi > 0 ? dpi : 96; }
    private sealed class DpiColumns(int dpi) : ResponsiveColumns
    { protected override int LayoutDpi => dpi > 0 ? dpi : 96; }
    private static void Require(bool condition, string reason) { if (!condition) throw new InvalidOperationException(reason); }
}
