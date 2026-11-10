namespace GRID2VR;

// Controls -> Driving -> Force Feedback. With "Use launcher force feedback" on, every
// launch (VR and Desktop) shapes the force GRID 2 sends to the wheel: weight scales
// the steady steering force, vibration the kerbs, surface and impacts, strength
// both. 100% passes GRID 2's force unchanged. GRID 2's own settings, profile and
// save are not changed.
internal sealed class ForceFeedbackPanel : VerticalStack
{
    private readonly CheckBox enabled = new() { Name = "ForceFeedbackOverride", Text = "Use launcher force feedback", AutoSize = true };
    private readonly ValueSlider weight = new("ForceFeedbackWeight", 1, 100, 100, suffix: "%");
    private readonly ValueSlider strength = new("ForceFeedbackStrength", 0, 100, 100, suffix: "%");
    private readonly ValueSlider effects = new("ForceFeedbackEffects", 0, 100, 100, suffix: "%");
    internal const string Notice = "When on, GRID2VR adjusts the force GRID 2 sends to your wheel on every Desktop or VR launch. " +
        "Wheel weight scales the steady steering force, Vibration the kerbs, road surface and impacts, and Strength both. " +
        "100% is GRID 2's force unchanged. GRID 2's own Vibration & Feedback settings still apply and are not changed. " +
        "Changes apply on the next launch.";

    public ForceFeedbackPanel()
    {
        Name = "ForceFeedbackControls";
        Controls.Add(new SettingRow("Launcher settings", enabled));
        Controls.Add(new SettingRow("Wheel weight", weight));
        Controls.Add(new SettingRow("Strength", strength));
        Controls.Add(new SettingRow("Vibration", effects));
        Controls.Add(new Label { Name = "ForceFeedbackNotice", Text = Notice, AutoSize = true, UseMnemonic = false, Margin = new Padding(0, 4, 0, 8) });
        enabled.CheckedChanged += (_, _) => RefreshEnabled();
        RefreshEnabled();
    }

    private void RefreshEnabled()
    {
        foreach (var control in new Control[] { weight, strength, effects }) control.Enabled = enabled.Checked;
    }

    internal void LoadChoices(Choices value)
    {
        enabled.Checked = value.ForceFeedbackOverride;
        weight.Value = value.ForceFeedbackWeight; strength.Value = value.ForceFeedbackStrength; effects.Value = value.ForceFeedbackEffects;
        RefreshEnabled();
    }

    internal Choices ApplyTo(Choices value) => value with
    {
        ForceFeedbackOverride = enabled.Checked, ForceFeedbackWeight = weight.Value,
        ForceFeedbackStrength = strength.Value, ForceFeedbackEffects = effects.Value
    };
}
