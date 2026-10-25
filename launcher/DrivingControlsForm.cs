using System.Diagnostics;
using System.Runtime.InteropServices;

namespace GRID2VR;

internal sealed class DrivingControlsForm : LauncherForm
{
    private readonly string root;
    private readonly DrivingControls settings;
    private readonly CheckBox enabled = new() { Text = "Launcher driving bindings", AutoSize = true };
    private readonly ListView rows = new() { View = View.Details, FullRowSelect = true, MultiSelect = false, Dock = DockStyle.Fill, HideSelection = false };
    public DrivingControlsForm(string workspaceRoot)
    {
        root = workspaceRoot; settings = DrivingControls.Load(root);
        Text = "GRID2VR — Driving controls"; ClientSize = new(980, 680); MinimumSize = new(760, 520);
        StartPosition = FormStartPosition.CenterParent; AutoScaleMode = AutoScaleMode.Dpi;
        enabled.Checked = settings.Enabled;
        rows.Columns.Add("Action", 160); rows.Columns.Add("Keyboard", 170); rows.Columns.Add("Controller / wheel / pedals", 340);
        rows.Columns.Add("Deadzone", 100); rows.Columns.Add("Saturation", 110);
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new(16), RowCount = 4, ColumnCount = 1 };
        layout.RowStyles.Add(new(SizeType.AutoSize)); layout.RowStyles.Add(new(SizeType.AutoSize));
        layout.RowStyles.Add(new(SizeType.Percent, 100)); layout.RowStyles.Add(new(SizeType.AutoSize));
        layout.Controls.Add(enabled, 0, 0);
        layout.Controls.Add(new Label { Text = DrivingControls.RuntimeNotice, AutoSize = true, MaximumSize = new(900, 0), Margin = new(0, 8, 0, 12), Tag = "hint" }, 0, 1);
        layout.Controls.Add(rows, 0, 2);
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true, Margin = new(0, 12, 0, 0) };
        void Button(string label, Action action) { var button = new Button { Text = label, AutoSize = true }; button.Click += (_, _) => Guard(action); buttons.Controls.Add(button); }
        Button("Binding wizard…", () => CaptureBinding(null, false));
        Button("Xbox preset", () => { settings.Bindings.RemoveAll(b => !b.Keyboard); settings.Bindings.AddRange(DrivingControls.XboxPreset()); enabled.Checked = true; RefreshRows(); });
        Button("Bind keyboard…", () => CaptureBinding(SelectedAction(), true));
        Button("Bind device…", () => CaptureBinding(SelectedAction(), false));
        Button("Calibration…", Calibrate);
        Button("Use game binding", () => { var action = SelectedAction(); settings.Bindings.RemoveAll(b => b.Action == action); RefreshRows(); });
        Button("Save", Save);
        var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel }; buttons.Controls.Add(cancel); CancelButton = cancel;
        layout.Controls.Add(buttons, 0, 3); Controls.Add(layout); RefreshRows();
    }
    private void Guard(Action action) { try { action(); } catch (Exception ex) { MessageBox.Show(this, ex.Message, "Driving controls", MessageBoxButtons.OK, MessageBoxIcon.Warning); } }
    private string SelectedAction() => rows.SelectedItems.Count == 1 ? rows.SelectedItems[0].Text : throw new IOException("Select an action first.");
    private void RefreshRows()
    {
        var selected = rows.SelectedItems.Count == 1 ? rows.SelectedItems[0].Text : DrivingControls.Actions[0];
        rows.BeginUpdate(); rows.Items.Clear();
        foreach (var action in DrivingControls.Actions)
        {
            var keyboard = settings.Bindings.FirstOrDefault(b => b.Action == action && b.Keyboard);
            var device = settings.Bindings.FirstOrDefault(b => b.Action == action && !b.Keyboard);
            var row = new ListViewItem(action); row.SubItems.Add(DrivingInput.Description(keyboard)); row.SubItems.Add(DrivingInput.Description(device));
            row.SubItems.Add(device is null ? "—" : device.DeadZone.ToString("P0")); row.SubItems.Add(device is null ? "—" : device.Saturation.ToString("P0"));
            rows.Items.Add(row); row.Selected = action == selected;
        }
        rows.EndUpdate();
    }
    private void CaptureBinding(string? action, bool keyboard)
    {
        using var dialog = new DrivingBindingForm(action, keyboard);
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        foreach (var binding in dialog.Bindings) settings.Assign(binding);
        if (dialog.Bindings.Count > 0) enabled.Checked = true;
        RefreshRows();
    }
    private void Calibrate()
    {
        var action = SelectedAction(); var binding = settings.Bindings.FirstOrDefault(b => b.Action == action && !b.Keyboard);
        if (binding is null) throw new IOException("Bind a controller, wheel or pedal first.");
        using var dialog = new DrivingCalibrationForm(binding);
        if (dialog.ShowDialog(this) == DialogResult.OK) { settings.Assign(dialog.Binding); RefreshRows(); }
    }
    private void Save()
    {
        foreach (var name in new[] { "grid2", "grid2_avx" })
        {
            var processes = Process.GetProcessesByName(name);
            try { if (processes.Length > 0) throw new IOException("Close GRID 2 before saving driving controls."); }
            finally { foreach (var process in processes) process.Dispose(); }
        }
        settings.Enabled = enabled.Checked; settings.Save(root); DialogResult = DialogResult.OK; Close();
    }
}

internal sealed class DrivingCalibrationForm : LauncherForm
{
    private readonly DrivingBinding original;
    private readonly ComboBox calibration = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 220 };
    private readonly NumericUpDown dead = new() { Minimum = 0, Maximum = 90, Width = 100 };
    private readonly NumericUpDown saturation = new() { Minimum = 10, Maximum = 100, Width = 100 };
    public DrivingBinding Binding => original with { Calibration = calibration.SelectedItem!.ToString()!, DeadZone = dead.Value / 100m, Saturation = saturation.Value / 100m };
    public DrivingCalibrationForm(DrivingBinding binding)
    {
        original = binding; Text = "Calibration — " + binding.Action; ClientSize = new(480, 260); MinimumSize = Size;
        StartPosition = FormStartPosition.CenterParent; AutoScaleMode = AutoScaleMode.Dpi;
        calibration.Items.AddRange(DrivingControls.Calibrations); calibration.SelectedItem = binding.Calibration;
        dead.Value = binding.DeadZone * 100m; saturation.Value = binding.Saturation * 100m;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new(20), ColumnCount = 2, RowCount = 5 };
        layout.ColumnStyles.Add(new(SizeType.Percent, 45)); layout.ColumnStyles.Add(new(SizeType.Percent, 55));
        layout.Controls.Add(new Label { Text = "Axis direction", AutoSize = true }, 0, 0); layout.Controls.Add(calibration, 1, 0);
        layout.Controls.Add(new Label { Text = "Deadzone (%)", AutoSize = true }, 0, 1); layout.Controls.Add(dead, 1, 1);
        layout.Controls.Add(new Label { Text = "Saturation (%)", AutoSize = true }, 0, 2); layout.Controls.Add(saturation, 1, 2);
        var help = new Label { Text = "biDir uses one half of a centered axis. uniDir uses the full travel of a pedal or lever. Deadzone must be below saturation.", AutoSize = true, MaximumSize = new(430, 0), Tag = "hint" };
        layout.Controls.Add(help, 0, 3); layout.SetColumnSpan(help, 2);
        var ok = new Button { Text = "Apply", AutoSize = true }; ok.Click += (_, _) =>
        {
            if (dead.Value >= saturation.Value) { MessageBox.Show(this, "Deadzone must be below saturation."); return; }
            var check = new DrivingControls { Bindings = [Binding] }; check.Validate();
            if (check.Problems() is { Count: > 0 } problems) { MessageBox.Show(this, string.Join(Environment.NewLine, problems)); return; }
            DialogResult = DialogResult.OK; Close();
        };
        var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel }; CancelButton = cancel;
        layout.Controls.Add(ok, 0, 4); layout.Controls.Add(cancel, 1, 4); Controls.Add(layout);
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
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
    public DrivingBindingForm(string? action, bool keyboard, bool deviceButtonOnly = false)
    {
        deviceButtons = deviceButtonOnly;
        actions = action is null ? ["Steer Left", "Steer Right", "Accelerate", "Brake", "Clutch", "Handbrake", "Gear Up", "Gear Down", "Change View", "Look Back", "Pause"] : [action];
        Text = "GRID2VR — Bind driving controls"; ClientSize = new(680, 430); MinimumSize = new(600, 430);
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
        panel.Controls.AddRange([heading, source, devices, filter, refresh, instruction, status, buttons]); Controls.Add(panel);
        source.SelectedIndexChanged += (_, _) => BeginStep(); devices.SelectedIndexChanged += (_, _) => { if (!updating) BeginStep(); };
        filter.SelectedIndexChanged += (_, _) => BeginStep(); timer.Tick += (_, _) => Poll();
        Shown += (_, _) => RefreshDevices(); BeginStep();
    }
    private void RefreshDevices()
    {
        timer.Stop(); input?.Dispose(); input = null; updating = true; devices.Items.Clear(); devices.Items.Add("All connected devices");
        string? error = null;
        try { input = new DrivingInput(Handle); foreach (var device in input.Devices) devices.Items.Add(device); }
        catch (Exception ex) { error = ex.Message; }
        finally { devices.SelectedIndex = 0; updating = false; BeginStep(); timer.Start(); }
        if (error is not null) status.Text = error;
    }
    private void BeginStep()
    {
        captures.Clear(); active = null; held = Keys.None; pendingKey = null; releaseAt = -1;
        if (step >= actions.Length) return;
        heading.Text = actions[step].ToUpperInvariant() + (actions.Length > 1 ? $"  ({step + 1}/{actions.Length})" : "");
        devices.Enabled = filter.Enabled = source.SelectedIndex == 0;
        instruction.Text = source.SelectedIndex == 1 ? "Press and release a key. Escape can be assigned; use Cancel to leave." : "Center steering and release pedals. Keep still for a moment, then move the desired input and return it to rest.";
        if (deviceButtons) instruction.Text = "Release buttons and hats, wait a moment, then press and release the button or hat direction you want to use for recentering.";
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
        var selected = devices.SelectedItem as DrivingInput.Device; var ready = 0; var connected = 0;
        foreach (var device in input.Devices)
        {
            if (selected is not null && selected.Id != device.Id) continue;
            if (!captures.TryGetValue(device.Id, out var capture)) captures[device.Id] = capture = new(device, actions[step]) { AxesOnly = filter.SelectedIndex == 1, ButtonsOnly = filter.SelectedIndex == 2 };
            var sample = input.Read(device); capture.Update(sample, now);
            if (sample.Connected != 0) ++connected;
            if (ReferenceEquals(active, capture) && sample.Connected == 0) { BeginStep(); status.Text = "Device disconnected. Reconnect and Refresh."; return; }
            if (capture.Ready) ++ready;
            if (active is null && capture.WaitingForRelease) active = capture;
        }
        if (active is not null)
        {
            status.Text = DrivingInput.Description(active.Detected) + " — release / return to rest.";
            if (active.Completed is not null) Next(active.Completed);
        }
        else status.Text = ready > 0 ? "Ready — move an axis or press a button." : connected > 0 ? "Waiting for stable rest…" : "Device disconnected. Reconnect and Refresh.";
    }
    protected override void OnDeactivate(EventArgs e) { base.OnDeactivate(e); BeginStep(); }
    protected override void Dispose(bool disposing) { if (disposing) { timer.Dispose(); input?.Dispose(); } base.Dispose(disposing); }
}
