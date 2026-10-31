namespace GRID2VR;

// Same choice as DiRT2VR: name what stops working, default to Cancel, and remember only by choice.
internal sealed class VrLayerRecoveryForm : LauncherForm
{
    private readonly CheckBox remember = new() { Text = "Remember for this GRID2VR installation", Name = "RememberLayerBypass", AutoSize = true };
    internal Control Content { get; }
    internal bool RememberChoice => remember.Checked;

    internal VrLayerRecoveryForm(VrLayerRecovery request)
    {
        Text = "VR compatibility"; Font = new Font("Segoe UI", 10);
        AutoScaleMode = AutoScaleMode.Dpi; AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink; AutoScroll = true;
        MaximumSize = Screen.PrimaryScreen?.WorkingArea.Size ?? Size.Empty;
        FormBorderStyle = FormBorderStyle.FixedDialog; StartPosition = FormStartPosition.CenterParent; ShowInTaskbar = false;
        MinimizeBox = MaximizeBox = false;
        var layout = new TableLayoutPanel { AutoSize = true, Padding = new Padding(20), ColumnCount = 1 };
        Label Line(string text, string name = "") => new() { Text = text, Name = name, AutoSize = true, MaximumSize = new Size(560, 0), UseMnemonic = false };
        var title = Line("An OpenXR layer is preventing VR from starting", "LayerFailureTitle"); title.Font = new Font(Font, FontStyle.Bold);
        layout.Controls.Add(title);
        var consequences = Line(string.Join("\n\n", request.Layers.Select(layer => layer.DisplayName + ": " + layer.Consequence)), "LayerConsequences");
        consequences.Margin = new Padding(3, 12, 3, 12); layout.Controls.Add(consequences);
        layout.Controls.Add(Line("Retry with these layers disabled for GRID2VR? Other applications keep their current settings."));
        remember.Margin = new Padding(3, 12, 3, 6); layout.Controls.Add(remember);
        layout.Controls.Add(Line("Remembered choices can be removed in Settings → VR compatibility."));
        var details = new TextBox { Name = "LayerFailureDetails", Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false,
            Width = 560, Height = 150, Visible = false, Text = request.Message + "\r\n\r\n" + request.Report };
        layout.Controls.Add(details);
        var buttons = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.RightToLeft, Dock = DockStyle.Fill, WrapContents = false, Margin = new Padding(0, 16, 0, 0) };
        var retry = new Button { Text = "Disable and retry", Name = "RetryWithoutLayers", AutoSize = true, DialogResult = DialogResult.OK, Padding = new Padding(8, 3, 8, 3) };
        var cancel = new Button { Text = "Cancel", Name = "CancelLayerRecovery", AutoSize = true, DialogResult = DialogResult.Cancel, Padding = retry.Padding };
        var show = new Button { Text = "Show details", Name = "ShowLayerDetails", AutoSize = true, Padding = retry.Padding };
        var expanded = false;
        show.Click += (_, _) => { expanded = !expanded; details.Visible = expanded; show.Text = expanded ? "Hide details" : "Show details"; };
        buttons.Controls.AddRange([retry, cancel, show]); layout.Controls.Add(buttons);
        Controls.Add(layout); Content = layout;
        AcceptButton = cancel; CancelButton = cancel; ActiveControl = cancel;
    }

    internal static VrLayerRecoveryChoice Confirm(IWin32Window owner, VrLayerRecovery request)
    {
        using var dialog = new VrLayerRecoveryForm(request);
        return new(dialog.ShowDialog(owner) == DialogResult.OK, dialog.RememberChoice);
    }
}

internal sealed class VrCompatibilityForm : LauncherForm
{
    private readonly string data;
    private readonly ListBox layers = new() { Name = "RememberedLayerBypasses", Dock = DockStyle.Fill, IntegralHeight = false, HorizontalScrollbar = true };
    private readonly Label summary = new() { Name = "LayerBypassSummary", AutoSize = true, MaximumSize = new Size(520, 0), UseMnemonic = false };
    private readonly Button reenable = new() { Text = "Re-enable selected", Name = "ReenableLayer", AutoSize = true, Padding = new Padding(8, 3, 8, 3) };

    internal VrCompatibilityForm(string data)
    {
        this.data = data;
        Text = "VR compatibility"; Font = new Font("Segoe UI", 10); AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(580, 340); MinimumSize = new Size(460, 300);
        StartPosition = FormStartPosition.CenterParent; ShowInTaskbar = false; MinimizeBox = MaximizeBox = false;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(16), ColumnCount = 1, RowCount = 4 };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100));
        foreach (var size in new[] { SizeType.AutoSize, SizeType.Percent, SizeType.AutoSize, SizeType.AutoSize })
            layout.RowStyles.Add(new(size, size == SizeType.Percent ? 100 : 0));
        layout.Controls.Add(summary, 0, 0); layout.Controls.Add(layers, 0, 1);
        layout.Controls.Add(new Label { Text = "Changes apply to the next VR launch. Other applications are unaffected. A changed layer registration needs approval again if it still prevents VR from starting.",
            AutoSize = true, MaximumSize = new Size(520, 0), UseMnemonic = false }, 0, 2);
        var buttons = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.RightToLeft, Dock = DockStyle.Fill, WrapContents = false, Margin = new Padding(0, 12, 0, 0) };
        var close = new Button { Text = "Close", Name = "CloseVrCompatibility", AutoSize = true, DialogResult = DialogResult.Cancel, Padding = reenable.Padding };
        buttons.Controls.AddRange([close, reenable]); layout.Controls.Add(buttons, 0, 3);
        layers.SelectedIndexChanged += (_, _) => reenable.Enabled = layers.SelectedItem is not null;
        reenable.Click += (_, _) =>
        {
            if (layers.SelectedItem is not VrLayerBypass selected) return;
            try { VrApiLayers.Reenable(data, selected.LayerName); RefreshLayers(); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            { MessageBox.Show(this, error.Message, "VR compatibility", MessageBoxButtons.OK, MessageBoxIcon.Error); }
        };
        Controls.Add(layout); CancelButton = close;
        RefreshLayers();
    }

    private void RefreshLayers()
    {
        layers.Items.Clear();
        layers.Items.AddRange([.. VrApiLayers.Remembered(data)]);
        summary.Text = layers.Items.Count == 0 ? "No OpenXR layers are disabled by remembered choices." : "Remembered OpenXR layer bypasses for this installation:";
        reenable.Enabled = false;
        if (layers.Items.Count > 0) layers.SelectedIndex = 0;
    }
}
