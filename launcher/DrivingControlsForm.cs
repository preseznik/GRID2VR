using System.Diagnostics;
using System.Runtime.InteropServices;

namespace GRID2VR;

internal sealed class DrivingControlsForm : LauncherForm
{
    private readonly string root;
    private DrivingControls settings;
    private readonly CheckBox enabled = new() { Text = "Launcher driving bindings", AutoSize = true };
    private readonly ComboBox transmission = new() { Name = "DrivingTransmission", AccessibleName = "Transmission", DropDownStyle = ComboBoxStyle.DropDownList, Width = 260 };
    private readonly TrackBar steeringLinearity = new() { Name = "SteeringLinearitySlider", AccessibleName = "Wheel steering linearity", Minimum = -10, Maximum = 10,
        SmallChange = 1, LargeChange = 1, TickFrequency = 1, TickStyle = TickStyle.BottomRight, AutoSize = false, Dock = DockStyle.Fill, Margin = Padding.Empty };
    private readonly Label linearityValue = new() { Text = "0", AutoSize = true, Anchor = AnchorStyles.Right, Margin = new(8, 0, 4, 0) };
    private readonly TrackBar clutchShiftThreshold = new() { Name = "ClutchShiftThresholdSlider", AccessibleName = "Clutch shift threshold percent", Minimum = 10, Maximum = 100,
        SmallChange = 1, LargeChange = 5, TickFrequency = 10, TickStyle = TickStyle.BottomRight, AutoSize = false, Dock = DockStyle.Fill, Margin = Padding.Empty };
    private readonly Label clutchShiftValue = new() { Name = "ClutchShiftThresholdValue", Text = "50%", AutoSize = true, Anchor = AnchorStyles.Right, Margin = new(8, 0, 4, 0) };
    private readonly ComboBox directDevice = new() { Name = "DrivingDirectLaunchDevice", AccessibleName = "Practice and Race controls device", DropDownStyle = ComboBoxStyle.DropDownList, Width = 260 };
    private sealed record DeviceChoice(string Value, string Label) { public override string ToString() => Label; }
    private readonly ListView rows = new() { View = View.Details, FullRowSelect = true, MultiSelect = false, Dock = DockStyle.Fill, HideSelection = false, ShowItemToolTips = true };
    private readonly Button bindKeyboard, bindDevice, calibrate, removeBinding, resetAction;
    public DrivingControlsForm(string workspaceRoot)
    {
        root = workspaceRoot; settings = DrivingControls.Load(root);
        Text = "GRID2VR — Driving controls"; ClientSize = new(980, 740); MinimumSize = new(760, 580);
        StartPosition = FormStartPosition.CenterParent; AutoScaleMode = AutoScaleMode.Dpi;
        enabled.Checked = settings.Enabled;
        transmission.Items.AddRange(["Use game setting", "Automatic", "Manual sequential", "Manual H-pattern", "Manual H-pattern + clutch"]);
        transmission.SelectedIndex = Array.IndexOf(DrivingControls.Transmissions, settings.Transmission);
        steeringLinearity.ValueChanged += (_, _) => linearityValue.Text = steeringLinearity.Value.ToString();
        steeringLinearity.Value = settings.SteeringLinearity;
        clutchShiftThreshold.ValueChanged += (_, _) => clutchShiftValue.Text = clutchShiftThreshold.Value + "%";
        clutchShiftThreshold.Value = settings.ClutchShiftThreshold;
        void UpdateClutchThreshold() => clutchShiftThreshold.Enabled = clutchShiftValue.Enabled = transmission.SelectedIndex == Array.IndexOf(DrivingControls.Transmissions, "hpattern_clutch");
        transmission.SelectedIndexChanged += (_, _) => UpdateClutchThreshold(); UpdateClutchThreshold();
        rows.Columns.Add("Action", 160); rows.Columns.Add("Device", 280); rows.Columns.Add("Input", 230);
        rows.Columns.Add("Deadzone", 100); rows.Columns.Add("Saturation", 110);
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new(16), RowCount = 6, ColumnCount = 1 };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100));
        for (var i = 0; i < 6; ++i) layout.RowStyles.Add(new(i == 3 ? SizeType.Percent : SizeType.AutoSize, i == 3 ? 100 : 0));
        var options = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true, Margin = Padding.Empty };
        options.Controls.Add(enabled);
        options.Controls.Add(new Label { Text = "Transmission", AutoSize = true, Margin = new(14, 6, 6, 0) });
        options.Controls.Add(transmission);
        options.Controls.Add(new Label { Text = "Driving device", AutoSize = true, Margin = new(14, 6, 6, 0) });
        options.Controls.Add(directDevice);
        var linearity = new TableLayoutPanel { Dock = DockStyle.Fill, Height = 42, MinimumSize = new(0, 42), ColumnCount = 3, RowCount = 1, Margin = Padding.Empty };
        linearity.ColumnStyles.Add(new(SizeType.AutoSize)); linearity.ColumnStyles.Add(new(SizeType.Percent, 100)); linearity.ColumnStyles.Add(new(SizeType.AutoSize));
        linearity.RowStyles.Add(new(SizeType.Percent, 100));
        linearity.Controls.Add(new Label { Text = "Wheel steering linearity (-10..10)", AutoSize = true, Anchor = AnchorStyles.Left });
        linearity.Controls.Add(steeringLinearity); linearity.Controls.Add(linearityValue);
        var clutchThreshold = new TableLayoutPanel { Name = "ClutchShiftThresholdRow", Dock = DockStyle.Fill, Height = 42, MinimumSize = new(0, 42), ColumnCount = 3, RowCount = 1, Margin = Padding.Empty };
        clutchThreshold.ColumnStyles.Add(new(SizeType.AutoSize)); clutchThreshold.ColumnStyles.Add(new(SizeType.Percent, 100)); clutchThreshold.ColumnStyles.Add(new(SizeType.AutoSize));
        clutchThreshold.RowStyles.Add(new(SizeType.Percent, 100));
        clutchThreshold.Controls.Add(new Label { Text = "Clutch shift threshold (%)", AutoSize = true, Anchor = AnchorStyles.Left });
        clutchThreshold.Controls.Add(clutchShiftThreshold); clutchThreshold.Controls.Add(clutchShiftValue);
        var header = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 1, RowCount = 6, Margin = Padding.Empty };
        header.ColumnStyles.Add(new(SizeType.Percent, 100));
        for (var i = 0; i < 6; ++i) header.RowStyles.Add(new(SizeType.AutoSize));
        header.Controls.Add(options);
        header.Controls.Add(new Label { Text = "When the game's Controls preset is Keyboard, this device is claimed for driving so its rumble/force feedback works, in every launch mode. A wheel or controller preset you chose in the game is kept.",
            AutoSize = true, MaximumSize = new(900, 0), Tag = "hint" });
        header.Controls.Add(linearity);
        header.Controls.Add(new Label { Text = "Assigned wheel steering only. 0 is linear; negative values respond faster near the centre.", AutoSize = true, MaximumSize = new(900, 0), Tag = "hint" });
        header.Controls.Add(clutchThreshold);
        header.Controls.Add(new Label { Text = "A held gear engages at this clutch level. Analogue response is unchanged.", AutoSize = true, MaximumSize = new(900, 0), Tag = "hint" });
        layout.Controls.Add(header, 0, 0);
        layout.Controls.Add(new Label { Text = DrivingControls.RuntimeNotice, AutoSize = true, MaximumSize = new(900, 0), Margin = new(0, 8, 0, 12), Tag = "hint" }, 0, 1);
        Button AddButton(FlowLayoutPanel panel, string name, string label, Action action)
        {
            var button = new Button { Name = name, Text = label, AutoSize = true };
            button.Click += (_, _) => Guard(action); panel.Controls.Add(button); return button;
        }
        static (TableLayoutPanel Row, FlowLayoutPanel Actions) Toolbar(string name, string caption)
        {
            var row = new TableLayoutPanel { Name = name, Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 2, RowCount = 1, Margin = Padding.Empty };
            row.ColumnStyles.Add(new(SizeType.AutoSize)); row.ColumnStyles.Add(new(SizeType.Percent, 100));
            var actions = new FlowLayoutPanel { Name = name + "Actions", Dock = DockStyle.Fill, AutoSize = true, WrapContents = false, Margin = Padding.Empty };
            row.Controls.Add(new Label { Text = caption, AutoSize = true, MinimumSize = new(80, 0), Anchor = AnchorStyles.Left, Margin = new(0, 0, 8, 0), Tag = "section" });
            row.Controls.Add(actions); return (row, actions);
        }
        var setup = Toolbar("DrivingSetup", "Setup");
        AddButton(setup.Actions, "DrivingWizard", "Binding wizard…", () => CaptureBinding(null, false));
        AddButton(setup.Actions, "DrivingXboxPreset", "Xbox preset", () => { settings.ApplyXboxPreset(); enabled.Checked = true; RefreshRows(); });
        AddButton(setup.Actions, "DrivingLiveInput", "Live input…", () => { using var monitor = new DrivingInputMonitor(settings.Bindings); monitor.ShowDialog(this); });
        setup.Row.Margin = new(0, 0, 0, 8); layout.Controls.Add(setup.Row, 0, 2);
        layout.Controls.Add(rows, 0, 3);
        var selected = Toolbar("DrivingSelection", "Selected");
        bindKeyboard = AddButton(selected.Actions, "DrivingBindKeyboard", "Key…", () => CaptureBinding(SelectedAction(), true));
        bindKeyboard.AccessibleName = "Bind keyboard key to selected action";
        bindDevice = AddButton(selected.Actions, "DrivingBindDevice", "Device…", () => CaptureBinding(SelectedAction(), false));
        calibrate = AddButton(selected.Actions, "DrivingCalibrate", "Calibrate…", Calibrate);
        removeBinding = AddButton(selected.Actions, "DrivingRemoveBinding", "Remove", () => { settings.Bindings.Remove(SelectedBinding()); RefreshRows(); });
        resetAction = AddButton(selected.Actions, "DrivingResetAction", "Reset action", () => { var action = SelectedAction(); settings.Bindings.RemoveAll(b => b.Action == action); RefreshRows(); });
        selected.Row.Margin = new(0, 8, 0, 0); layout.Controls.Add(selected.Row, 0, 4);
        var profiles = Toolbar("DrivingProfiles", "Profiles");
        AddButton(profiles.Actions, "DrivingSaveProfile", "Save…", SaveProfile);
        AddButton(profiles.Actions, "DrivingLoadProfile", "Load…", LoadProfile);
        var dialogButtons = new FlowLayoutPanel { Name = "DrivingDialogButtons", AutoSize = true, WrapContents = false, Dock = DockStyle.Fill, Margin = Padding.Empty };
        AddButton(dialogButtons, "DrivingSave", "Save", Save);
        var cancel = new Button { Name = "DrivingCancel", Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel };
        dialogButtons.Controls.Add(cancel); CancelButton = cancel;
        var footer = new TableLayoutPanel { Name = "DrivingFooter", Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 2, RowCount = 1, Margin = new(0, 12, 0, 0) };
        footer.ColumnStyles.Add(new(SizeType.Percent, 100)); footer.ColumnStyles.Add(new(SizeType.AutoSize));
        footer.Controls.Add(profiles.Row); footer.Controls.Add(dialogButtons); layout.Controls.Add(footer, 0, 5);
        rows.SelectedIndexChanged += (_, _) => UpdateSelection();
        Controls.Add(layout); RefreshRows();
    }
    private void Guard(Action action) { try { action(); } catch (Exception ex) { MessageBox.Show(this, ex.Message, "Driving controls", MessageBoxButtons.OK, MessageBoxIcon.Warning); } }
    private string SelectedAction() => rows.SelectedItems.Count == 1 ? rows.SelectedItems[0].Text : throw new IOException("Select an action first.");
    private DrivingBinding SelectedBinding() => rows.SelectedItems.Count == 1 && rows.SelectedItems[0].Tag is DrivingBinding binding
        ? binding : throw new IOException("Select a saved binding first.");
    private void RefreshRows()
    {
        var selected = rows.SelectedItems.Count == 1 ? rows.SelectedItems[0].Text : DrivingControls.Actions[0];
        var selectedDevice = rows.SelectedItems.Count == 1 ? (rows.SelectedItems[0].Tag as DrivingBinding)?.DeviceId : null;
        rows.BeginUpdate(); rows.Items.Clear();
        foreach (var action in DrivingControls.Actions)
        {
            var bindings = settings.Bindings.Where(b => b.Action == action).OrderBy(b => !b.Keyboard).ToArray();
            foreach (var binding in bindings.Length == 0 ? new DrivingBinding?[] { null } : bindings)
            {
                var row = new ListViewItem(action) { Tag = binding };
                row.SubItems.Add(binding?.Device ?? "Use game binding");
                row.SubItems.Add(binding is null ? "—" : DrivingInput.InputDescription(binding));
                row.SubItems.Add(binding is null || binding.Keyboard ? "—" : binding.DeadZone.ToString("P0"));
                row.SubItems.Add(binding is null || binding.Keyboard ? "—" : binding.Saturation.ToString("P0"));
                row.ToolTipText = binding is null ? "No launcher override for this action." : DrivingInput.Description(binding) + Environment.NewLine + binding.DeviceId;
                rows.Items.Add(row);
                row.Selected = action == selected && StringComparer.OrdinalIgnoreCase.Equals(binding?.DeviceId, selectedDevice);
            }
        }
        rows.EndUpdate();
        if (rows.SelectedItems.Count == 0 && rows.Items.Cast<ListViewItem>().FirstOrDefault(r => r.Text == selected) is { } fallback) fallback.Selected = true;
        UpdateSelection();
        RefreshDirectDevices((directDevice.SelectedItem as DeviceChoice)?.Value ?? settings.DirectLaunchDevice);
    }
    // Only bound devices are useful: on a keyboard-only Controls preset any other
    // device has no game binding until it is claimed. A removed device stays selectable.
    private void RefreshDirectDevices(string selected)
    {
        var choices = new List<DeviceChoice> { new("auto", "Auto (steering device, then Xbox)"), new("keyboard", "Keyboard only"), new("xinput_pad", "Xbox controller") };
        foreach (var binding in settings.Bindings.Where(b => !b.Keyboard && b.DeviceType != "xinput_pad"))
            if (!choices.Any(c => StringComparer.OrdinalIgnoreCase.Equals(c.Value, binding.DeviceType))) choices.Add(new(binding.DeviceType, binding.Device));
        if (!choices.Any(c => StringComparer.OrdinalIgnoreCase.Equals(c.Value, selected))) choices.Add(new(selected, "Saved device"));
        directDevice.BeginUpdate(); directDevice.Items.Clear(); directDevice.Items.AddRange([.. choices]);
        directDevice.SelectedItem = choices.First(c => StringComparer.OrdinalIgnoreCase.Equals(c.Value, selected)); directDevice.EndUpdate();
    }
    private void UpdateSelection()
    {
        var selected = rows.SelectedItems.Count == 1 ? rows.SelectedItems[0] : null;
        var binding = selected?.Tag as DrivingBinding;
        bindKeyboard.Enabled = bindDevice.Enabled = selected is not null;
        calibrate.Enabled = binding is { Keyboard: false };
        removeBinding.Enabled = binding is not null;
        resetAction.Enabled = selected is not null && settings.Bindings.Any(b => b.Action == selected.Text);
    }
    private void CaptureBinding(string? action, bool keyboard)
    {
        using var dialog = new DrivingBindingForm(action, keyboard, listenAllDevices: settings.ListenAllDevices);
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        settings.ListenAllDevices = dialog.ListenAllDevices;
        foreach (var binding in dialog.Bindings) settings.Assign(binding);
        if (dialog.Bindings.Count > 0) enabled.Checked = true;
        RefreshRows();
    }
    private void Calibrate()
    {
        var binding = SelectedBinding();
        if (binding.Keyboard) throw new IOException("Select a controller, wheel or pedal binding to calibrate.");
        using var dialog = new DrivingCalibrationForm(binding);
        if (dialog.ShowDialog(this) == DialogResult.OK) { settings.Assign(dialog.Binding); RefreshRows(); }
    }
    private void SaveProfile()
    {
        var directory = DrivingControls.ProfilesPath(root); Directory.CreateDirectory(directory);
        using var dialog = new SaveFileDialog { Title = "Save driving bindings profile", Filter = "Driving bindings (*.json)|*.json", DefaultExt = "json",
            AddExtension = true, InitialDirectory = directory, FileName = "Driving controls.json", OverwritePrompt = true };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        ReadOptions(); settings.SaveFile(dialog.FileName);
    }
    private void LoadProfile()
    {
        using var dialog = new OpenFileDialog { Title = "Load driving bindings profile", Filter = "Driving bindings (*.json)|*.json", InitialDirectory = DrivingControls.ProfilesPath(root) };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        var loaded = DrivingControls.LoadFile(dialog.FileName);
        settings = loaded; enabled.Checked = loaded.Enabled;
        transmission.SelectedIndex = Array.IndexOf(DrivingControls.Transmissions, loaded.Transmission);
        steeringLinearity.Value = loaded.SteeringLinearity; clutchShiftThreshold.Value = loaded.ClutchShiftThreshold; RefreshRows();
        RefreshDirectDevices(loaded.DirectLaunchDevice);
    }
    private void ReadOptions()
    {
        settings.Enabled = enabled.Checked;
        settings.Transmission = DrivingControls.Transmissions[transmission.SelectedIndex];
        settings.SteeringLinearity = steeringLinearity.Value;
        settings.ClutchShiftThreshold = clutchShiftThreshold.Value;
        settings.DirectLaunchDevice = ((DeviceChoice)directDevice.SelectedItem!).Value;
    }
    private void Save()
    {
        foreach (var name in new[] { "grid2", "grid2_avx" })
        {
            var processes = Process.GetProcessesByName(name);
            try { if (processes.Length > 0) throw new IOException("Close GRID 2 before saving driving controls."); }
            finally { foreach (var process in processes) process.Dispose(); }
        }
        ReadOptions(); settings.Save(root); DialogResult = DialogResult.OK; Close();
    }
}

internal sealed class DrivingCalibrationForm : LauncherForm
{
    private readonly DrivingBinding original;
    private readonly ComboBox calibration = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 220 };
    private readonly NumericUpDown dead = new() { Name = "DeadzonePercent", AccessibleName = "Deadzone percent", Minimum = 0, Maximum = 90, DecimalPlaces = 2, Width = 92, Anchor = AnchorStyles.Right };
    private readonly NumericUpDown saturation = new() { Name = "SaturationPercent", AccessibleName = "Sensitivity / saturation percent", Minimum = 10, Maximum = 100, DecimalPlaces = 2, Width = 92, Anchor = AnchorStyles.Right };
    public DrivingBinding Binding => original with { Calibration = calibration.SelectedItem!.ToString()!, DeadZone = dead.Value / 100m, Saturation = saturation.Value / 100m };
    public DrivingCalibrationForm(DrivingBinding binding)
    {
        original = binding; Text = "Calibration — " + binding.Action; ClientSize = new(650, binding.Action == "Clutch" ? 470 : 420); MinimumSize = Size;
        StartPosition = FormStartPosition.CenterParent; AutoScaleMode = AutoScaleMode.Dpi;
        calibration.Items.AddRange(DrivingControls.Calibrations); calibration.SelectedItem = binding.Calibration;
        dead.Value = binding.DeadZone * 100m; saturation.Value = binding.Saturation * 100m;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new(20), ColumnCount = 2, RowCount = 5 };
        layout.ColumnStyles.Add(new(SizeType.AutoSize)); layout.ColumnStyles.Add(new(SizeType.Percent, 100));
        for (var i = 0; i < 5; ++i) layout.RowStyles.Add(new(SizeType.AutoSize));
        layout.Controls.Add(new Label { Text = "Axis direction", AutoSize = true }, 0, 0); layout.Controls.Add(calibration, 1, 0);
        layout.Controls.Add(new Label { Text = "Deadzone (%)", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 1); layout.Controls.Add(Slider(dead, "Deadzone"), 1, 1);
        layout.Controls.Add(new Label { Text = "Sensitivity / saturation (%)", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 2); layout.Controls.Add(Slider(saturation, "Saturation"), 1, 2);
        var clutchHelp = binding.Action == "Clutch" ? "\n\nManual H-pattern + clutch treats the final 2% of calibrated travel as fully pressed." : "";
        var help = new Label { Text = "Deadzone ignores input near rest. Saturation is the travel needed for full output: 100% uses all travel; lowering it makes the control more sensitive. Defaults are 0% and 100%. Deadzone must stay below saturation." + clutchHelp + "\n\nbiDir uses half of a centered axis; uniDir uses the full travel of a pedal or lever.", AutoSize = true, MaximumSize = new(600, 0), Margin = new(3, 10, 3, 12), Tag = "hint" };
        layout.Controls.Add(help, 0, 3); layout.SetColumnSpan(help, 2);
        var ok = new Button { Text = "Apply", AutoSize = true }; ok.Click += (_, _) =>
        {
            if (dead.Value >= saturation.Value) { MessageBox.Show(this, "Deadzone must be below saturation."); return; }
            var check = new DrivingControls { Bindings = [Binding] }; check.Validate();
            if (check.Problems() is { Count: > 0 } problems) { MessageBox.Show(this, string.Join(Environment.NewLine, problems)); return; }
            DialogResult = DialogResult.OK; Close();
        };
        var reset = new Button { Text = "Reset to 0% / 100%", AutoSize = true }; reset.Click += (_, _) => { dead.Value = 0; saturation.Value = 100; };
        var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel }; CancelButton = cancel;
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true };
        buttons.Controls.AddRange([ok, reset, cancel]); layout.Controls.Add(buttons, 0, 4); layout.SetColumnSpan(buttons, 2); Controls.Add(layout);
    }
    private static Control Slider(NumericUpDown number, string name)
    {
        var track = new TrackBar { Name = name + "Slider", AccessibleName = number.AccessibleName,
            Minimum = (int)number.Minimum, Maximum = (int)number.Maximum, Value = (int)Math.Round(number.Value),
            SmallChange = 1, LargeChange = 10, TickStyle = TickStyle.None, AutoSize = false, Dock = DockStyle.Fill, Margin = Padding.Empty };
        var panel = new TableLayoutPanel { Dock = DockStyle.Fill, Height = 38, MinimumSize = new(240, 38), ColumnCount = 2, RowCount = 1, Margin = Padding.Empty };
        panel.ColumnStyles.Add(new(SizeType.Percent, 100)); panel.ColumnStyles.Add(new(SizeType.AutoSize));
        panel.RowStyles.Add(new(SizeType.Percent, 100));
        panel.Controls.Add(track); panel.Controls.Add(number);
        var synchronizing = false;
        track.ValueChanged += (_, _) => { if (synchronizing) return; number.Value = track.Value; };
        number.ValueChanged += (_, _) =>
        {
            synchronizing = true;
            try { track.Value = (int)Math.Round(number.Value); }
            finally { synchronizing = false; }
        };
        return panel;
    }
}

internal sealed class DrivingBindingForm : LauncherForm
{
    private readonly string[] actions;
    private int step;
    private readonly Label heading = new() { AutoSize = true, Font = new((SystemFonts.MessageBoxFont ?? SystemFonts.DefaultFont).FontFamily, 22, FontStyle.Bold) };
    private readonly Label instruction = new() { AutoSize = true, MaximumSize = new(620, 0) };
    private readonly Label status = new() { AutoSize = true, MaximumSize = new(620, 0), Tag = "hint" };
    private readonly ComboBox source = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 250 };
    private readonly ComboBox devices = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 500 };
    private readonly CheckBox allDevices = new() { Text = "Detect inputs from all connected devices", Checked = true, AutoSize = true };
    private readonly ComboBox filter = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 200 };
    private readonly System.Windows.Forms.Timer timer = new() { Interval = 40 };
    private readonly Dictionary<string, DrivingCapture> captures = [];
    private readonly List<DrivingBinding> bindings = [];
    private DrivingInput? input;
    private DrivingCapture? active;
    private Keys held;
    private long releaseAt = -1;
    private DrivingBinding? pendingKey;
    private bool updating;
    private readonly bool deviceButtons;
    public IReadOnlyList<DrivingBinding> Bindings => bindings;
    public bool ListenAllDevices => allDevices.Checked;
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
    public DrivingBindingForm(string? action, bool keyboard, bool deviceButtonOnly = false, bool listenAllDevices = true)
    {
        deviceButtons = deviceButtonOnly;
        actions = action is null ? DrivingControls.WizardActions : [action];
        allDevices.Checked = listenAllDevices;
        Text = "GRID2VR — Bind driving controls"; ClientSize = new(680, 470); MinimumSize = new(600, 470);
        StartPosition = FormStartPosition.CenterParent; AutoScaleMode = AutoScaleMode.Dpi;
        source.Items.AddRange(["Controller / wheel / pedals", "Keyboard"]); source.SelectedIndex = keyboard ? 1 : 0;
        filter.Items.AddRange(["Automatic input", "Axis only", "Button / hat only"]); filter.SelectedIndex = 0;
        if (deviceButtonOnly) { source.SelectedIndex = 0; source.Visible = false; filter.SelectedIndex = 2; filter.Visible = false; Text = "GRID2VR — Bind recenter"; }
        var panel = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true, Padding = new(20) };
        var refresh = new Button { Text = "Refresh devices", AutoSize = true }; refresh.Click += (_, _) => RefreshDevices();
        var buttons = new FlowLayoutPanel { AutoSize = true };
        var skip = new Button { Text = "Skip / keep current", AutoSize = true }; skip.Click += (_, _) => Next(null);
        var retry = new Button { Text = "Try again", AutoSize = true }; retry.Click += (_, _) => BeginStep();
        // Escape is a valid game binding. Do not assign Form.CancelButton.
        var cancel = new Button { Text = "Cancel", AutoSize = true }; cancel.Click += (_, _) => { DialogResult = DialogResult.Cancel; Close(); };
        buttons.Controls.AddRange([skip, retry, cancel]);
        panel.Controls.AddRange([heading, source, allDevices, devices, filter, refresh, instruction, status, buttons]); Controls.Add(panel);
        source.SelectedIndexChanged += (_, _) => BeginStep(); devices.SelectedIndexChanged += (_, _) =>
        {
            if (updating) return;
            if (devices.SelectedIndex == 0 && !allDevices.Checked) allDevices.Checked = true;
            else BeginStep();
        };
        allDevices.CheckedChanged += (_, _) =>
        {
            updating = true;
            if (allDevices.Checked && devices.Items.Count > 0) devices.SelectedIndex = 0;
            else if (devices.SelectedIndex <= 0 && devices.Items.Count > 1) devices.SelectedIndex = 1;
            updating = false; BeginStep();
        };
        filter.SelectedIndexChanged += (_, _) => BeginStep(); timer.Tick += (_, _) => Poll();
        Shown += (_, _) => RefreshDevices(); BeginStep();
    }
    private void RefreshDevices()
    {
        timer.Stop(); input?.Dispose(); input = null; updating = true; devices.Items.Clear(); devices.Items.Add("All connected devices");
        string? error = null;
        try { input = new DrivingInput(Handle); foreach (var device in input.Devices) devices.Items.Add(device); }
        catch (Exception ex) { error = ex.Message; }
        finally { devices.SelectedIndex = !allDevices.Checked && devices.Items.Count > 1 ? 1 : 0; updating = false; BeginStep(); timer.Start(); }
        if (error is not null) status.Text = error;
    }
    private void BeginStep()
    {
        captures.Clear(); active = null; held = Keys.None; pendingKey = null; releaseAt = -1;
        if (step >= actions.Length) return;
        heading.Text = actions[step].ToUpperInvariant() + (actions.Length > 1 ? $"  ({step + 1}/{actions.Length})" : "");
        filter.Enabled = allDevices.Enabled = source.SelectedIndex == 0;
        devices.Enabled = source.SelectedIndex == 0 && !allDevices.Checked;
        instruction.Text = source.SelectedIndex == 1 ? "Press and release a key. Escape can be assigned; use Cancel to leave." : "Center steering and release pedals. Keep still for a moment, then move the desired input and return it to rest.";
        if (deviceButtons) instruction.Text = "Release buttons and hats, wait a moment, then press and release the button or hat direction you want to use for recentering.";
        else if (actions[step].StartsWith("Gear ", StringComparison.Ordinal) && actions[step] is not ("Gear Up" or "Gear Down") && source.SelectedIndex == 0)
            instruction.Text = "Leave the shifter in neutral, select this gear, then return to neutral. Skip gears your shifter does not have. A separate USB shifter works with all-device detection.";
        status.Text = source.SelectedIndex == 1 ? "Ready" : "Waiting for stable rest…";
    }
    private void Next(DrivingBinding? binding)
    {
        if (binding is not null) bindings.Add(binding);
        if (++step >= actions.Length) { DialogResult = DialogResult.OK; Close(); } else BeginStep();
    }
    protected override bool ProcessCmdKey(ref Message message, Keys keyData)
    {
        if (source.SelectedIndex != 1 || step >= actions.Length) return base.ProcessCmdKey(ref message, keyData);
        if (held != Keys.None) return true;
        var key = keyData & Keys.KeyCode;
        if (key == Keys.Tab) return base.ProcessCmdKey(ref message, keyData);
        key = key switch { Keys.ShiftKey => Keys.LShiftKey, Keys.ControlKey => Keys.LControlKey, Keys.Menu => Keys.LMenu, _ => key };
        var native = DrivingControls.KeyInput(key);
        if (native is null) { status.Text = "That key is not supported. Try another."; return true; }
        held = key; releaseAt = -1;
        pendingKey = new() { Action = actions[step], Device = "Keyboard", DeviceType = "keyboard", DeviceId = "keyboard", Input = native };
        status.Text = DrivingInput.Description(pendingKey) + " — release to continue."; return true;
    }
    private void Poll()
    {
        if (!ContainsFocus || step >= actions.Length) return;
        var now = Environment.TickCount64;
        if (source.SelectedIndex == 1)
        {
            if (held == Keys.None) return;
            if (GetAsyncKeyState((int)held) < 0) { releaseAt = -1; return; }
            if (releaseAt < 0) releaseAt = now;
            if (now - releaseAt >= 160) Next(pendingKey);
            return;
        }
        if (input is null || input.Devices.Count == 0) { status.Text = "Connect a device and Refresh, or choose Keyboard."; return; }
        var selected = allDevices.Checked ? null : devices.SelectedItem as DrivingInput.Device; var ready = 0; var axesReady = 0; var connected = 0;
        if (!allDevices.Checked && selected is null) { status.Text = "Select a device, or enable all-device detection."; return; }
        foreach (var device in input.Devices)
        {
            if (selected is not null && selected.Id != device.Id) continue;
            if (!captures.TryGetValue(device.Id, out var capture)) captures[device.Id] = capture = new(device, actions[step]) { AxesOnly = filter.SelectedIndex == 1, ButtonsOnly = filter.SelectedIndex == 2 };
            var sample = input.Read(device); capture.Update(sample, now);
            if (sample.Connected != 0) ++connected;
            if (ReferenceEquals(active, capture) && sample.Connected == 0) { BeginStep(); status.Text = "Device disconnected. Reconnect and Refresh."; return; }
            if (capture.Ready) ++ready;
            if (capture.AxesReady) ++axesReady;
        }
        // Sample every device before choosing a candidate. A deliberate button
        // press must win over unrelated pedal/wheel movement in the same poll.
        active = DrivingCapture.ChooseActive(captures.Values, active);
        if (active is not null)
        {
            status.Text = DrivingInput.Description(active.Detected) + " — release / return to rest.";
            if (active.Completed is not null) Next(active.Completed);
        }
        else status.Text = ready > 0 ? filter.SelectedIndex == 0 && axesReady == 0 ? "Buttons / hats ready — keep axes still briefly." : "Ready — move an axis or press a button."
            : connected > 0 ? "Waiting for stable rest…" : "Device disconnected. Reconnect and Refresh.";
    }
    protected override void OnDeactivate(EventArgs e) { base.OnDeactivate(e); BeginStep(); }
    protected override void Dispose(bool disposing) { if (disposing) { timer.Dispose(); input?.Dispose(); } base.Dispose(disposing); }
}
