namespace GRID2VR;

internal static class DrivingControlsLayoutTests
{
    internal static void Run(string root)
    {
        static void Check(bool value, string message) { if (!value) throw new InvalidOperationException(message); }
        using (var form = new DrivingControlsForm(root))
        {
            var rows = form.Controls[0].Controls.OfType<ListView>().Single();
            _ = rows.Handle; // Hidden native control state; never show the form.
            Button Button(string name) => (Button)form.Controls.Find(name, true).Single();
            void Select(ListViewItem? row)
            {
                rows.SelectedIndices.Clear();
                if (row is not null) row.Selected = true;
            }
            void State(bool action, bool saved, bool calibrate)
            {
                Check(Button("DrivingBindKeyboard").Enabled == action && Button("DrivingBindDevice").Enabled == action,
                    "Binding buttons must follow the selected action.");
                Check(Button("DrivingRemoveBinding").Enabled == saved && Button("DrivingResetAction").Enabled == saved &&
                    Button("DrivingCalibrate").Enabled == calibrate, "Saved-binding actions must follow the selected binding.");
            }
            Select(null); State(false, false, false);
            Select(rows.Items.Cast<ListViewItem>().First(row => row.Tag is null)); State(true, false, false);
            Select(rows.Items.Cast<ListViewItem>().First(row => row.Tag is DrivingBinding { Keyboard: true })); State(true, true, false);
            Select(rows.Items.Cast<ListViewItem>().First(row => row.Tag is DrivingBinding { Keyboard: false })); State(true, true, true);
            Check(!form.Visible, "Selection fixture must not show a window.");
        }
        var previousAppearance = LauncherAppearance.Modern;
        try
        {
            foreach (var modern in new[] { false, true }) foreach (var dpi in new[] { 96, 144, 192 })
                foreach (var size in new[] { new Size(760, 580), new Size(980, 740) })
                {
                    using var form = new DrivingControlsForm(root);
                    LauncherAppearance.Modern = modern; LauncherAppearance.Apply(form);
                    var layout = form.Controls[0]; form.Controls.Remove(layout);
                    var scale = dpi / 96f;
                    using var font = new Font(form.Font.FontFamily, form.Font.Size * scale, form.Font.Style);
                    try
                    {
                        layout.Visible = true; layout.Dock = DockStyle.None;
                        layout.Scale(new SizeF(scale, scale)); layout.Font = font;
                        layout.Size = new((int)(size.Width * scale), (int)(size.Height * scale)); layout.PerformLayout();
                        foreach (var name in new[] { "DrivingSetupActions", "DrivingSelectionActions", "DrivingProfilesActions", "DrivingDialogButtons" })
                        {
                            var panel = (FlowLayoutPanel)layout.Controls.Find(name, true).Single();
                            Check(!panel.WrapContents, name + " must remain one toolbar row.");
                            Check(panel.Right <= panel.Parent!.ClientSize.Width && panel.Bottom <= panel.Parent.ClientSize.Height, name + " clipped its parent.");
                            foreach (Control child in panel.Controls)
                                Check(child.Left >= 0 && child.Top >= 0 && child.Right <= panel.ClientSize.Width && child.Bottom <= panel.ClientSize.Height,
                                    $"{name}/{child.Text} {child.Bounds} clipped in {panel.ClientSize} at {size.Width}px / {dpi} DPI / modern={modern}; font={child.Font.Size}.");
                        }
                        var footer = layout.Controls.Find("DrivingFooter", true).Single();
                        var threshold = layout.Controls.Find("ClutchShiftThresholdRow", true).Single();
                        Check(threshold.Right <= threshold.Parent!.ClientSize.Width && threshold.Bottom <= threshold.Parent.ClientSize.Height, "Clutch threshold row clipped its parent.");
                        foreach (Control child in threshold.Controls)
                            Check(child.Left >= 0 && child.Top >= 0 && child.Right <= threshold.ClientSize.Width && child.Bottom <= threshold.ClientSize.Height,
                                "Clutch threshold editor or readout clipped at scaled DPI.");
                        Check(footer.Bottom <= layout.Height && footer.Right <= layout.Width, "Dialog footer escaped the window.");
                        var save = layout.Controls.Find("DrivingSave", true).Single(); var cancel = layout.Controls.Find("DrivingCancel", true).Single();
                        Check(save.Top == cancel.Top && save.Right <= cancel.Left && save.Parent == cancel.Parent, "Save and Cancel must stay together.");
                        Check(layout.Controls.OfType<ListView>().Single().Height >= 60 * scale,
                            $"The toolbar layout left no usable binding list: {layout.Controls.OfType<ListView>().Single().Height}px at {size.Width}px / {dpi} DPI / modern={modern}.");
                    }
                    finally { form.Controls.Add(layout); }
                    Check(!form.Visible, "DPI layout fixture must not show a window.");
                }
        }
        finally { LauncherAppearance.Modern = previousAppearance; }
    }
}
