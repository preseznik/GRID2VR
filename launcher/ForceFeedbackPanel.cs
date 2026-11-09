namespace GRID2VR;

// Controls -> Driving -> Force Feedback. With "Use launcher force feedback" on, the
// three values replace GRID 2's Wheel Weight, Wheel Strength and Environmental
// Effects (Options -> Vibration & Feedback) in every launch, VR and Desktop. GRID 2's
// own settings, profile and save are not changed, so they return when it is off.
internal sealed class ForceFeedbackPanel : VerticalStack
{
    private readonly CheckBox enabled = new() { Name = "ForceFeedbackOverride", Text = "Use launcher force feedback", AutoSize = true };
    private readonly ValueSlider weight = new("ForceFeedbackWeight", 1, 100, 50, suffix: "%");
    private readonly ValueSlider strength = new("ForceFeedbackStrength", 0, 100, 100, suffix: "%");
    private readonly ValueSlider effects = new("ForceFeedbackEffects", 0, 100, 50, suffix: "%");
    internal const string Notice = "When on, these replace GRID 2's Wheel Weight, Wheel Strength and Environmental Effects " +
        "(Options → Vibration & Feedback) on every Desktop or VR launch. Vibration is the road, kerb and impact feel. " +
        "GRID 2's own settings are not changed and return when this is off. Defaults match GRID 2's: weight 50%, strength 100%, vibration 50%. " +
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
