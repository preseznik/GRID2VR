namespace GRID2VR;

// Reads every device without assigning bindings or synthesizing input. Raw axis
// values intentionally show the rest endpoint before any binding calibration.
internal sealed class DrivingInputMonitor : LauncherForm
{
    private readonly IReadOnlyList<DrivingBinding> bindings;
    private readonly Label status = new() { AutoSize = true, Dock = DockStyle.Fill, Tag = "hint" };
    private readonly TableLayoutPanel devices = new() { AutoSize = true, Dock = DockStyle.Top, ColumnCount = 1 };
    private readonly List<DeviceView> views = [];
    private readonly System.Windows.Forms.Timer timer = new() { Interval = 50 };
    private DrivingInput? input;

    public DrivingInputMonitor(IReadOnlyList<DrivingBinding>? savedBindings = null)
    {
        bindings = savedBindings?.ToArray() ?? [];
        Text = "GRID2VR — Live driving inputs"; ClientSize = new(900, 720); MinimumSize = new(660, 440);
        StartPosition = FormStartPosition.CenterParent; AutoScaleMode = AutoScaleMode.Dpi;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new(16), ColumnCount = 1, RowCount = 4 };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100));
        layout.RowStyles.Add(new(SizeType.AutoSize)); layout.RowStyles.Add(new(SizeType.AutoSize));
        layout.RowStyles.Add(new(SizeType.Percent, 100)); layout.RowStyles.Add(new(SizeType.AutoSize));
        layout.Controls.Add(new Label { Text = "All devices are shown together. Move the wheel, pedals, shifter or controller to see their Windows inputs. Axis values are before calibration; a released pedal may read −100% or +100%. Refresh after connecting a new device.", AutoSize = true, MaximumSize = new(820, 0), Margin = new(0, 0, 0, 10) }, 0, 0);
        layout.Controls.Add(status, 0, 1);
        var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true, Margin = new(0, 10, 0, 10) };
        devices.ColumnStyles.Add(new(SizeType.Percent, 100)); scroll.Controls.Add(devices); layout.Controls.Add(scroll, 0, 2);
        var buttons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill };
        var refresh = new Button { Text = "Refresh devices", AutoSize = true }; refresh.Click += (_, _) => RefreshDevices();
        var close = new Button { Text = "Close", AutoSize = true, DialogResult = DialogResult.OK }; CancelButton = close;
        buttons.Controls.AddRange([refresh, close]); layout.Controls.Add(buttons, 0, 3); Controls.Add(layout);
        timer.Tick += (_, _) => Poll(); Shown += (_, _) => RefreshDevices();
    }

    private void RefreshDevices()
    {
        timer.Stop(); input?.Dispose(); input = null; views.Clear();
        devices.SuspendLayout();
        try
        {
            while (devices.Controls.Count > 0) { var control = devices.Controls[0]; devices.Controls.RemoveAt(0); control.Dispose(); }
            devices.RowStyles.Clear(); devices.RowCount = 0;
            input = new DrivingInput(Handle);
            foreach (var device in input.Devices)
            {
                var view = new DeviceView(device); views.Add(view); Add(view.Panel);
            }
            foreach (var saved in MissingDevices(bindings, input.Devices))
                Add(new Label { Text = $"Saved device not connected: {saved.Device}\n{saved.DeviceId}\nReconnect it and refresh. Changing a wheel's compatibility mode may change its identity.", AutoSize = true, Dock = DockStyle.Top, MaximumSize = new(800, 0), Padding = new(10), Margin = new(0, 0, 0, 12), Tag = "hint" });
            LauncherAppearance.Apply(devices);
            Poll(); timer.Start();
        }
        catch (Exception ex) { input?.Dispose(); input = null; status.Text = ex.Message; }
        finally { devices.ResumeLayout(true); }
    }

    private void Add(Control control) { devices.RowStyles.Add(new(SizeType.AutoSize)); devices.Controls.Add(control, 0, devices.RowCount++); }

    private void Poll()
    {
        if (input is null) return;
        var connected = 0;
        foreach (var view in views)
        {
            var sample = input.Read(view.Device); view.Update(sample);
            if (sample.Connected != 0) ++connected;
        }
        status.Text = views.Count == 0 ? "No controllers detected. Connect a device, then refresh." : $"Live — {connected} of {views.Count} detected devices connected";
    }

    internal static IReadOnlyList<DrivingBinding> MissingDevices(IEnumerable<DrivingBinding> saved, IEnumerable<DrivingInput.Device> detected) =>
        saved.Where(b => !b.Keyboard && !detected.Any(d => string.Equals(d.Id, b.DeviceId, StringComparison.OrdinalIgnoreCase) && string.Equals(d.Product, b.DeviceType, StringComparison.OrdinalIgnoreCase)))
            .DistinctBy(b => (b.DeviceId.ToUpperInvariant(), b.DeviceType.ToUpperInvariant())).ToArray();

    internal static string ButtonsText(DrivingInput.Device device, DrivingInput.Sample sample)
    {
        if (sample.Connected == 0) return "Disconnected";
        var pressed = new List<string>();
        for (var i = 0; i < 128; ++i)
            if (sample.Buttons[i] != 0)
            {
                if (device.Xbox)
                {
                    if (i < DrivingControls.XboxButtons.Length && DrivingControls.XboxButtons[i].Length != 0)
                        pressed.Add(DrivingControls.XboxButtons[i].Replace("xinput_", "").Replace('_', ' '));
                }
                else pressed.Add(i.ToString());
            }
        return pressed.Count == 0 ? "None" : string.Join(", ", pressed);
    }

    internal static string HatsText(DrivingInput.Sample sample)
    {
        if (sample.Connected == 0) return "Disconnected";
        return string.Join("   ", Enumerable.Range(0, 4).Select(i => $"{i}: " + (DrivingInput.PovNeutral(sample.Pov[i]) ? "center" : (sample.Pov[i] / 100m).ToString("0.##") + "°")));
    }

    protected override void OnDeactivate(EventArgs e) { timer.Stop(); status.Text = "Paused while this window is inactive."; base.OnDeactivate(e); }
    protected override void OnActivated(EventArgs e) { base.OnActivated(e); if (input is not null) { Poll(); timer.Start(); } }
    protected override void Dispose(bool disposing) { if (disposing) { timer.Dispose(); input?.Dispose(); } base.Dispose(disposing); }

    private sealed class DeviceView
    {
        public DrivingInput.Device Device { get; }
        public GroupBox Panel { get; }
        private readonly Label connection = new() { AutoSize = true };
        private readonly Label buttons = new() { AutoSize = true, MaximumSize = new(560, 0) };
        private readonly Label hats = new() { AutoSize = true };
        private readonly Label[] values = new Label[8];
        private readonly AxisMeter[] bars = new AxisMeter[8];

        public DeviceView(DrivingInput.Device device)
        {
            Device = device;
            Panel = new() { Text = device.Name, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Dock = DockStyle.Top, Padding = new(10, 22, 10, 10), Margin = new(0, 0, 0, 12) };
            var layout = new TableLayoutPanel { AutoSize = true, Dock = DockStyle.Top, ColumnCount = 3 };
            layout.ColumnStyles.Add(new(SizeType.Absolute, 170)); layout.ColumnStyles.Add(new(SizeType.Percent, 100)); layout.ColumnStyles.Add(new(SizeType.Absolute, 100));
            connection.Text = device.Id; layout.Controls.Add(connection, 0, 0); layout.SetColumnSpan(connection, 3);
            var names = device.Xbox ? new[] { "Left stick X", "Left stick Y", "Right stick X", "Right stick Y", "Left trigger", "Right trigger", "—", "—" } : new[] { "X axis", "Y axis", "Z axis", "X rotation", "Y rotation", "Z rotation", "Slider 0", "Slider 1" };
            for (var i = 0; i < 8; ++i)
            {
                layout.Controls.Add(new Label { Text = names[i], AutoSize = true, Anchor = AnchorStyles.Left }, 0, i + 1);
                bars[i] = new(device.Xbox && i >= 4) { Dock = DockStyle.Fill, Height = 18, Margin = new(3, 6, 3, 6) }; layout.Controls.Add(bars[i], 1, i + 1);
                values[i] = new() { Text = "—", AutoSize = true, Anchor = AnchorStyles.Right }; layout.Controls.Add(values[i], 2, i + 1);
            }
            layout.Controls.Add(new Label { Text = device.Xbox ? "Buttons" : "Buttons (zero based)", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 9);
            layout.Controls.Add(buttons, 1, 9); layout.SetColumnSpan(buttons, 2);
            layout.Controls.Add(new Label { Text = "Hats (zero based)", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 10);
            layout.Controls.Add(hats, 1, 10); layout.SetColumnSpan(hats, 2);
            Panel.Controls.Add(layout);
        }

        public void Update(DrivingInput.Sample sample)
        {
            connection.Text = (sample.Connected != 0 ? "Connected · " : "Disconnected · ") + Device.Id;
            for (var i = 0; i < 8; ++i)
            {
                var available = sample.Connected != 0 && (sample.Axes & (1u << i)) != 0 && float.IsFinite(sample.Values[i]);
                bars[i].Enabled = available;
                if (!available) { bars[i].SetValue(0); values[i].Text = "—"; continue; }
                var value = Math.Clamp(sample.Values[i], -1, 1);
                bars[i].SetValue(value);
                values[i].Text = value.ToString("+0.0%;−0.0%;0.0%");
            }
            buttons.Text = ButtonsText(Device, sample); hats.Text = Device.Xbox ? "Shown under buttons" : HatsText(sample);
        }
    }

    // Native progress bars animate toward their target, which lags a wheel's
    // live position. Paint the current sample directly, with a neutral marker.
    private sealed class AxisMeter(bool positive) : Control
    {
        private float value;
        public void SetValue(float sample) { if (value == sample) return; value = sample; Invalidate(); }
        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var bounds = new Rectangle(0, 0, Math.Max(0, Width - 1), Math.Max(0, Height - 1));
            if (bounds.Width < 2 || bounds.Height < 2) return;
            var modern = LauncherAppearance.Modern && !SystemInformation.HighContrast;
            using var background = new SolidBrush(modern ? LauncherAppearance.Cell : SystemColors.ControlLight);
            using var line = new Pen(modern ? LauncherAppearance.Line : SystemColors.ControlDark);
            e.Graphics.FillRectangle(background, bounds); e.Graphics.DrawRectangle(line, bounds);
            var origin = positive ? 1 : bounds.Width / 2;
            if (!positive) e.Graphics.DrawLine(line, origin, 1, origin, bounds.Height - 1);
            if (!Enabled) return;
            var position = (int)Math.Round((positive ? Math.Max(0, value) : (value + 1) / 2) * (bounds.Width - 2)) + 1;
            using var fill = new SolidBrush(modern ? LauncherAppearance.Accent : SystemColors.Highlight);
            e.Graphics.FillRectangle(fill, Math.Min(origin, position), 2, Math.Max(1, Math.Abs(position - origin)), Math.Max(1, bounds.Height - 3));
        }
    }
}
