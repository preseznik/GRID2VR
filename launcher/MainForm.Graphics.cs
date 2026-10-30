using System.Text.Json;

namespace GRID2VR;

internal sealed partial class MainForm
{
    private readonly ComboBox graphicsPreset = Choice("GraphicsPreset");
    private readonly Button potatoPreset = Button("Potato / Low-End", "PotatoPreset");
    private readonly Button recommendPreset = Button("Recommend for this PC", "RecommendGraphics");
    private readonly Button undoPreset = Button("Undo preset", "UndoGraphicsPreset");
    private readonly FlowLayoutPanel presetActions = new() { Name = "GraphicsPresetActions", AutoSize = true, Dock = DockStyle.Top, WrapContents = true };
    // Recommendation reasons are worth quoting, so the hint is selectable text.
    private readonly SelectableStatusText presetHint = new() { AccessibleName = "Graphics preset explanation", Margin = new Padding(0, 4, 0, 10) };
    private readonly Label lastPerformance = TextLabel("None measured yet. Launch VR and drive for at least 10 seconds.");
    private readonly Dictionary<string, ComboBox> qualityEditors = new();
    private GraphicsOptions? presetUndo;
    private MeasuredPerformance? measuredPerformance;
    private GpuInfo? detectedGpu;
    private bool gpuDetected;

    private void InitializeGraphicsPresets(Control parent)
    {
        Section(parent, "Graphics preset");
        graphicsPreset.Items.Add(new ContentItem("working", "Working VR defaults (Custom)"));
        graphicsPreset.Items.Add(new ContentItem(GraphicsRecommendation.Preset, "VR Balanced"));
        graphicsPreset.Items.Add(new ContentItem("potato", "Potato / Low-End"));
        graphicsPreset.Items.AddRange(GraphicsCatalog.Presets.Select(p => new ContentItem(p.Id, p.Label)).ToArray());
        graphicsPreset.Items.Add(new ContentItem("custom", "Custom"));
        graphicsPreset.SelectedIndex = 0;
        Field(parent, "Quality preset", graphicsPreset);
        presetActions.Controls.AddRange([recommendPreset, potatoPreset, undoPreset]); parent.Controls.Add(presetActions);
        presetHint.Name = "GraphicsPresetHint"; parent.Controls.Add(presetHint);
        lastPerformance.Name = "LastVrPerformance"; lastPerformance.Tag = "hint";
        Field(parent, "Last VR session", lastPerformance);
        undoPreset.Enabled = false;
        graphicsPreset.SelectionChangeCommitted += (_, _) => SelectGraphicsPreset(ChoiceId(graphicsPreset));
        potatoPreset.Click += (_, _) => SelectGraphicsPreset("potato");
        recommendPreset.Click += (_, _) => RecommendGraphics();
        undoPreset.Click += (_, _) =>
        {
            if (presetUndo is not { } previous) return;
            presetUndo = null; ApplyGraphics(previous); undoPreset.Enabled = false;
            notice.Text = "Previous graphics restored. Save settings or launch to keep them.";
        };
        tips.SetToolTip(potatoPreset, "One click: Ultra Low quality, VR MSAA Off and 70% VR render scale. Headset output, desktop resolution, bindings, careers and seat positions are unchanged. Save or launch to keep it. Performance depends on your PC and headset.");
        tips.SetToolTip(recommendPreset, "Suggest a VR render scale, and VR Balanced or Potato when needed. Uses your last measured VR session when it used the same quality and VR anti-aliasing as now; otherwise estimates from this PC's graphics card and the headset resolution. Nothing is saved until you Save or launch; Undo preset restores your previous choices.");
        tips.SetToolTip(lastPerformance, "Measured by the renderer while driving in VR: frame rate in one-second windows, GPU usage, and whether the GPU or the CPU limited the frame rate. Menus and the flat screen are excluded.");
        tips.SetToolTip(graphicsPreset, "Quality presets mirror the game's DX11 settings, without its first-boot resolution or physics changes. MSAA and render scale stay independent except for Potato. In-game edits last only for that session.");
    }

    private void InitializeQualityEditors(Control parent)
    {
        qualityEditors.Add("TreeDetail", treeDetail); qualityEditors.Add("ObjectDetail", objectDetail);
        qualityEditors.Add("CrowdDetail", crowdDetail); qualityEditors.Add("TextureFiltering", textureFiltering);
        foreach (var category in GraphicsCatalog.Categories)
        {
            if (!qualityEditors.TryGetValue(category.Field, out var editor))
            {
                editor = Choice(category.Field); qualityEditors.Add(category.Field, editor);
                Field(parent, category.Label, editor);
            }
            editor.Items.Clear();
            editor.Items.AddRange(category.Choices.Where(c => category.Available || c.Id == category.Default)
                .Select(c => new ContentItem(c.Id, c.Label)).ToArray());
            SelectChoice(editor, category.Default);
            editor.SelectedIndexChanged += (_, _) => GraphicsEdited();
            if (!category.Available) tips.SetToolTip(editor, "Requires Intel-specific graphics extensions. Kept Off until capability detection is qualified; presets do not enable it.");
        }
        parent.Controls.Add(TextLabel("Intel-specific extensions remain Off until capability detection is verified. Automatic hardware detection is not a fixed preset and is not offered here. VR keeps motion blur suppressed and the low-detail cockpit wheel animated."));
        parent.Controls.Add(TextLabel("Launcher settings are applied on every launch. Changes made inside GRID 2 affect that session only; use these controls to keep your preferences."));
    }

    private GraphicsOptions CurrentQuality(GraphicsOptions value) => GraphicsCatalog.WithQuality(value,
        qualityEditors.Where(p => !GraphicsCatalog.LegacyFields.Contains(p.Key))
            .Select(p => new KeyValuePair<string, string>(p.Key, ChoiceId(p.Value))));

    private void InitializeGraphicsEditTracking()
    {
        foreach (var editor in new[] { vrMsaa, desktopMsaa }) editor.SelectedIndexChanged += (_, _) => GraphicsEdited();
        foreach (var editor in new[] { desktopVSync, desktopBorderless, vrMirrorBorderless, hudFollow }) editor.CheckedChanged += (_, _) => GraphicsEdited();
        renderScale.ValueChanged += (_, _) => GraphicsEdited();
        desktopRenderScale.ValueChanged += (_, _) => GraphicsEdited();
        hudDistance.ValueChanged += (_, _) => GraphicsEdited();
    }

    private void ClearGraphicsPresetUndo() { presetUndo = null; undoPreset.Enabled = false; }

    private void GraphicsEdited()
    {
        if (updatingResolution) return;
        // An old whole-selection snapshot must never discard later manual edits.
        ClearGraphicsPresetUndo(); UpdateGraphicsPreset();
    }

    private void SelectGraphicsPreset(string id)
    {
        if (id == "custom") { UpdateGraphicsPreset(); return; }
        var previous = CurrentGraphics();
        var next = GraphicsCatalog.ApplyPreset(previous, id);
        if (next != previous) presetUndo = previous;
        ApplyGraphics(next); EnableActions();
        notice.Text = id == "potato" ? "Potato selected: Ultra Low, VR MSAA Off, 70% render scale. Save settings or launch to keep it."
            : id == GraphicsRecommendation.Preset ? "VR Balanced selected: High quality, Medium crowd, VR MSAA Off. Render scale is unchanged. Save settings or launch to keep it."
            : "Graphics preset selected. Save settings or launch to keep it. Resolution and anti-aliasing are unchanged.";
    }

    private void RecommendGraphics()
    {
        if (!gpuDetected) { detectedGpu = GraphicsRecommendation.DetectGpu(); gpuDetected = true; }
        var headset = headsetResolution is not null && headsetResolution.Runtime == runtime.SelectedItem as string ? headsetResolution : null;
        var previous = CurrentGraphics();
        var recommendation = GraphicsRecommendation.Recommend(headset, detectedGpu, measuredPerformance, previous);
        var next = GraphicsRecommendation.Apply(previous, recommendation);
        if (next != previous) presetUndo = previous;
        ApplyGraphics(next); EnableActions();
        presetHint.Text = recommendation.Reason;
        var preset = recommendation.Preset switch { "potato" => "Potato", GraphicsRecommendation.Keep => "your current quality", _ => "VR Balanced" };
        notice.Text = next == previous ? "Your current graphics settings are already the recommendation."
            : $"Recommended {preset} at {next.VrRenderScale}% VR render scale. Save settings or launch to keep it, or select Undo preset.";
    }

    private void UpdateMeasuredPerformance(JsonElement summary, JsonElement graphics, bool current)
    {
        measuredPerformance = MeasuredPerformance.Parse(summary, graphics);
        lastPerformance.Text = measuredPerformance is null ? "None measured yet. Launch VR and drive for at least 10 seconds."
            : (current ? "Current session so far: " : "") + measuredPerformance.Describe();
    }

    private void UpdateGraphicsPreset()
    {
        if (updatingResolution || qualityEditors.Count != GraphicsCatalog.Categories.Count) return;
        var id = GraphicsCatalog.PresetId(CurrentGraphics());
        SelectChoice(graphicsPreset, id);
        presetHint.Text = id == "potato"
            ? "Potato: Ultra Low quality · VR MSAA Off · 70% VR scale (49% of the pixels at 100%). Headset output stays unchanged."
            : id == GraphicsRecommendation.Preset ? "VR Balanced: High quality · Medium crowd · VR MSAA Off. Recommend for this PC also picks a VR render scale."
            : id == "custom" ? "Custom quality settings are active, including options hidden in Simple view. Switching views never resets them."
            : id == "working" ? "The original working VR quality profile. Presets change quality only; resolution and MSAA have their own controls."
            : "Native DX11 quality preset. Intel-only extensions stay Off. Resolution and MSAA are unchanged.";
    }

    private void EnableGraphicsEditors(bool enabled)
    {
        graphicsPreset.Enabled = potatoPreset.Enabled = recommendPreset.Enabled = enabled;
        undoPreset.Enabled = enabled && presetUndo is not null;
        foreach (var category in GraphicsCatalog.Categories) qualityEditors[category.Field].Enabled = enabled && category.Available;
    }
}
