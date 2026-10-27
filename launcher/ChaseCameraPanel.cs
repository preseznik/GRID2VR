namespace GRID2VR;

internal sealed class ChaseCameraPanel : VerticalStack
{
    private readonly CheckBox freeLook = new() { Name = "ChaseFreeLook", Text = "On (Experimental)", AutoSize = true };
    private readonly CheckBox keepAngle = new() { Name = "ChaseKeepAngle", Text = "Keep camera angle", AutoSize = true };
    private readonly MouseLookPicker mouseMode = new();
    private readonly ValueSlider maxZoom = new("ChaseMaxZoomOut", 180, 1000, 180, 10, "%");
    private readonly ValueSlider mouse = new("ChaseMouseSensitivity", 25, 300, 100, suffix: "%");
    private readonly ValueSlider stick = new("ChaseStickSensitivity", 25, 300, 100, suffix: "%");
    private readonly CheckBox invert = new() { Name = "ChaseInvertVertical", Text = "Mouse and right stick", AutoSize = true };
    internal const string Instructions = "Enable Free look for close and far chase views while driving or watching an offline race replay, on desktop or in VR. " +
        "Choose Hold left or Hold right to require a mouse button, or Always on to look around without holding a button. You can also use the controller's right stick. Scroll to zoom without holding a mouse button. " +
        "Hold R3 to look behind; release returns to the ordinary chase angle, even with Keep camera angle enabled. " +
        "Keep camera angle retains your selected orbit while driving. Invert vertical applies to mouse and stick. " +
        "Zoom changes distance while preserving the native field of view; obstructions can keep the camera closer. " +
        "Menus and loss of focus suspend camera input. Release camera inputs before resuming. " +
        "You can move the camera during paused replay. Pause and rewind suspend automatic camera return; native replay controls still work. " +
        "Orbit and zoom reset when the camera or car changes, after seeking, or on leaving replay. Flashback camera controls are not supported. " +
        "Changes apply on the next launch; the existing VR replay presentation setting still applies.";

    public ChaseCameraPanel()
    {
        Name = "ChaseCameraControls";
        Controls.Add(new Label { Text = "Chase camera", Tag = "section", AutoSize = true, Font = new Font(SystemFonts.MessageBoxFont!, FontStyle.Bold), Margin = new Padding(0, 12, 0, 8) });
        var options = new FlowLayoutPanel { Name = "ChaseFreeLookOptions", AutoSize = true, WrapContents = true, Margin = Padding.Empty };
        options.Controls.AddRange([freeLook, keepAngle]);
        Controls.Add(new SettingRow("Free look", options));
        Controls.Add(new SettingRow("Mouse look", mouseMode));
        Controls.Add(new Label { Text = "Scroll to zoom in / out — no mouse button needed.", AutoSize = true, Margin = new Padding(0, 0, 0, 8) });
        Controls.Add(new SettingRow("Maximum zoom out", maxZoom));
        Controls.Add(new SettingRow("Mouse sensitivity", mouse));
        Controls.Add(new SettingRow("Right-stick sensitivity", stick));
        Controls.Add(new SettingRow("Invert vertical", invert));
        Controls.Add(new Label { Text = "Save settings or launch to keep your choices. Changes apply on the next launch.", AutoSize = true });
        var instructions = new LinkLabel { Name = "ChaseInstructions", Text = "Instructions", AutoSize = true, Margin = new Padding(0, 8, 0, 12) };
        instructions.LinkClicked += (_, _) => MessageBox.Show(FindForm(), Instructions, "Chase camera", MessageBoxButtons.OK, MessageBoxIcon.Information);
        Controls.Add(instructions);
        freeLook.CheckedChanged += (_, _) => RefreshEnabled();
        RefreshEnabled();
    }

    private void RefreshEnabled()
    {
        foreach (var control in new Control[] { keepAngle, mouseMode, maxZoom, mouse, stick, invert }) control.Enabled = freeLook.Checked;
    }

    internal void LoadChoices(Choices value)
    {
        freeLook.Checked = value.ChaseFreeLook; keepAngle.Checked = value.ChaseKeepAngle;
        mouseMode.SelectedMode = value.ChaseMouseMode; maxZoom.Value = value.ChaseMaxZoomOut;
        mouse.Value = value.ChaseMouseSensitivity; stick.Value = value.ChaseStickSensitivity; invert.Checked = value.ChaseInvertVertical;
        RefreshEnabled();
    }

    internal Choices ApplyTo(Choices value) => value with
    {
        ChaseFreeLook = freeLook.Checked, ChaseKeepAngle = keepAngle.Checked, ChaseMouseMode = mouseMode.SelectedMode,
        ChaseMaxZoomOut = maxZoom.Value, ChaseMouseSensitivity = mouse.Value, ChaseStickSensitivity = stick.Value,
        ChaseInvertVertical = invert.Checked
    };
}
