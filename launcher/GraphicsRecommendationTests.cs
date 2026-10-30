using System.Reflection;
using System.Text.Json;

namespace GRID2VR;

internal static class GraphicsRecommendationTests
{
    private const BindingFlags Flags = BindingFlags.Instance | BindingFlags.NonPublic;
    private static void Check(bool value, [System.Runtime.CompilerServices.CallerArgumentExpression(nameof(value))] string? text = null)
    { if (!value) throw new InvalidOperationException("Graphics recommendation regression: " + text); }

    private static readonly HeadsetResolution Quest3 = new("VDXR", "Quest 3", 3072, 3264);
    private static readonly GraphicsOptions Balanced = GraphicsCatalog.ApplyPreset(new(), "vrbalanced") with { VrRenderScale = 80 };
    // The two PC4080S sessions of 2026-10-30: 20261030-233501-198 (Ultra) and -233627-513 (High).
    private static readonly GraphicsOptions UltraMsaa = GraphicsCatalog.ApplyPreset(new(), "ultra") with { VrMsaa = "2xmsaa", VrRenderScale = 100 };
    private static readonly GraphicsOptions HighMsaa = GraphicsCatalog.ApplyPreset(new(), "high") with { VrMsaa = "2xmsaa", VrRenderScale = 100 };
    private static GpuInfo Gpu(string name) => new(name, 0x10de, 1, 16384);
    private static MeasuredPerformance Measured(GraphicsOptions settings, string limit, double fps, double? gpu, int scale = 80, int windows = 120,
        double? low = null, double blocked = 3, double? dipShare = 0, string dipLimit = "none") =>
        new(90, fps, low ?? fps - 1, windows, gpu, limit, 3072, 3264, scale, "NVIDIA GeForce RTX 4080 SUPER",
            GraphicsCatalog.Categories.ToDictionary(c => c.Field, c => GraphicsCatalog.Get(settings, c.Field)), settings.VrMsaa)
        { BlockedMedian = blocked, DipShare = dipShare, DipLimit = dipLimit };
    private static Recommendation Recommend(HeadsetResolution? headset, GpuInfo? gpu, MeasuredPerformance? last, GraphicsOptions? current = null) =>
        GraphicsRecommendation.Recommend(headset, gpu, last, current ?? Balanced);

    internal static void Run(SessionClient client)
    {
        Check(GraphicsRecommendation.GpuIndex("NVIDIA GeForce RTX 4080 SUPER") == 1.03 && GraphicsRecommendation.GpuIndex("NVIDIA GeForce RTX 4080") == 1.00);
        Check(GraphicsRecommendation.GpuIndex("NVIDIA GeForce RTX 4070 Ti SUPER") == .85 && GraphicsRecommendation.GpuIndex("AMD Radeon RX 7900 XTX") == .97);
        Check(GraphicsRecommendation.GpuIndex("NVIDIA GeForce RTX 4080 Laptop GPU") == .6 && GraphicsRecommendation.GpuIndex("Imaginary Accelerator") is null);
        Check(GraphicsCatalog.PresetId(UltraMsaa) == "ultra" && GraphicsCatalog.PresetId(HighMsaa) == "high");

        // First-launch estimates for a Quest 3 through Virtual Desktop (3072 × 3264 per eye).
        Check(Recommend(Quest3, Gpu("NVIDIA GeForce RTX 4080"), null) is { Preset: "vrbalanced", Scale: 100 });
        Check(Recommend(Quest3, Gpu("NVIDIA GeForce RTX 4070"), null) is { Preset: "vrbalanced", Scale: 80 });
        Check(Recommend(Quest3, Gpu("NVIDIA GeForce RTX 3070"), null) is { Preset: "vrbalanced", Scale: 70 });
        Check(Recommend(Quest3, Gpu("NVIDIA GeForce RTX 3060"), null) is { Preset: "vrbalanced", Scale: 60 });
        Check(Recommend(Quest3, Gpu("NVIDIA GeForce GTX 1070"), null) is { Preset: "potato", Scale: 70 });
        Check(Recommend(new("VDXR", "Quest 2", 2016, 2112), Gpu("NVIDIA GeForce RTX 5090"), null).Scale == 100);
        Check(Recommend(Quest3, Gpu("Imaginary Accelerator"), null) is { Scale: 80 } unknown && unknown.Reason.Contains("no estimate"));
        Check(Recommend(Quest3, null, null).Reason.StartsWith("No graphics card"));
        Check(Recommend(null, Gpu("NVIDIA GeForce RTX 4090"), null) is { Scale: 80 } blind && blind.Reason.Contains("Refresh headset"));
        // A renderer limit below 100% caps every recommendation (9000 px wide allows at most 91%).
        Check(new HeadsetResolution("VDXR", "Wide", 9000, 1000).MaximumRenderScale == 91);
        Check(Recommend(new("VDXR", "Wide", 9000, 1000), Gpu("NVIDIA GeForce RTX 5090"), null).Scale == 91);

        // A measured session with the same quality and VR MSAA tunes those settings.
        var high = Recommend(Quest3, null, Measured(HighMsaa, "at_target", 90, 59, 100), HighMsaa);
        Check(high is { Preset: GraphicsRecommendation.Keep, Scale: 100 } && high.Reason.Contains("Keep them") && high.Reason.Contains("room for more opponents"));
        Check(GraphicsRecommendation.Apply(HighMsaa, high) == HighMsaa);
        var ultra = Recommend(Quest3, null, Measured(UltraMsaa, "gpu", 69.1, 94, 100), UltraMsaa);
        Check(ultra is { Preset: "vrbalanced", Scale: 100 } && ultra.Reason.Contains("Ultra-level"));
        Check(GraphicsRecommendation.Apply(UltraMsaa, ultra) is { VrRenderScale: 100, VrMsaa: "off" } switched && GraphicsCatalog.PresetId(switched) == "vrbalanced");
        Check(Recommend(Quest3, null, Measured(HighMsaa, "gpu", 75, 97, 100), HighMsaa) is { Preset: GraphicsRecommendation.Keep, Scale: 85 });
        Check(Recommend(Quest3, null, Measured(Balanced, "gpu", 72, 98)) is { Preset: GraphicsRecommendation.Keep, Scale: 70 } gpu && gpu.Reason.Contains("GPU was the limit"));
        Check(Recommend(Quest3, null, Measured(Balanced, "gpu", 40, 99, 60)).Preset == "potato");
        Check(Recommend(Quest3, null, Measured(Balanced, "gpu", 88, 95)).Scale == 75); // Always at least one step down.
        Check(Recommend(Quest3, null, Measured(Balanced, "cpu", 78, 60)) is { Preset: GraphicsRecommendation.Keep, Scale: 80 } cpu && cpu.Reason.Contains("CPU was the limit"));
        Check(Recommend(Quest3, null, Measured(Balanced, "mixed", 80, 84)).Scale == 70);
        Check(Recommend(Quest3, null, Measured(Balanced, "at_target", 90, 60)).Scale == 90);
        Check(Recommend(Quest3, null, Measured(Balanced, "at_target", 90, 85)).Scale == 80);
        Check(Recommend(Quest3, null, Measured(Balanced, "at_target", 90, null)).Scale == 80);
        // The 7-AI Chicago race (20261030-235955-837): the median held 90 while slow
        // seconds were CPU-limited, so neither more scale nor "room for opponents".
        var chicago = Measured(HighMsaa, "at_target", 89.8, 60, 100, low: 80.7, blocked: .15, dipShare: .12, dipLimit: "cpu");
        Check(chicago.Describe().Contains("CPU-limited dips in 12% of seconds"));
        var cpuDips = Recommend(Quest3, null, chicago, HighMsaa);
        Check(cpuDips is { Preset: GraphicsRecommendation.Keep, Scale: 100 } && cpuDips.Reason.Contains("CPU was the limit") && !cpuDips.Reason.Contains("room"));
        Check(Recommend(Quest3, null, chicago with { BlockedMedian = .15, DipShare = .12, DipLimit = "cpu", Scale = 80 }, HighMsaa).Scale == 80);
        // A receipt from before dip tracking falls back to its 5th percentile.
        var older = Recommend(Quest3, null, chicago with { DipShare = null, DipLimit = "unknown" }, HighMsaa);
        Check(older is { Scale: 100 } && older.Reason.Contains("without one clear limit") && !older.Reason.Contains("room"));
        Check(Recommend(Quest3, null, Measured(Balanced, "at_target", 89.5, 75, low: 78, dipShare: .1, dipLimit: "gpu")) is { Scale: 75 } gpuDips && gpuDips.Reason.Contains("GPU was the limit"));
        // Few slow seconds: steady, but no "room" claim while the render thread had no idle time.
        Check(Recommend(Quest3, null, chicago with { DipShare = .02, DipLimit = "none" }, HighMsaa) is { Scale: 100 } steady && steady.Reason.Contains("Keep them") && !steady.Reason.Contains("room"));
        // Other quality or MSAA now, too short, or another headset: back to the estimate.
        var changed = Recommend(Quest3, null, Measured(HighMsaa, "gpu", 60, 99, 100), Balanced);
        Check(changed is { Preset: "vrbalanced", Scale: 100 } && changed.Reason.Contains("different quality or anti-aliasing"));
        Check(!Recommend(Quest3, null, Measured(HighMsaa, "at_target", 90, 59, 100), HighMsaa with { VrMsaa = "4xmsaa" }).Reason.Contains("Keep them"));
        Check(Recommend(Quest3, null, Measured(Balanced, "gpu", 60, 99, windows: 5)) is { Scale: 100 } brief && !brief.Reason.Contains("different"));
        Check(Recommend(new("VDXR", "Quest 3", 2496, 2592), null, Measured(Balanced, "gpu", 60, 99)).Reason.StartsWith("Estimated for NVIDIA GeForce RTX 4080 SUPER at 2496 × 2592"));

        var legacy = new GraphicsOptions(237, false, "8xmsaa", "medium", "high", "low", "high", 175, "4xmsaa") { VrRenderScale = 137 };
        var balanced = GraphicsRecommendation.Apply(legacy, new("vrbalanced", 85, ""));
        Check(GraphicsCatalog.PresetId(balanced) == "vrbalanced" && balanced.VrRenderScale == 85 && balanced.VrMsaa == "off");
        Check(balanced.CrowdDetail == "medium" && balanced.Shadows == "high" && balanced.CarReflections == "high" && balanced.AmbientOcclusion == "high" && balanced.SoftAmbientOcclusion == "off");
        Check(balanced.EffectiveDesktopRenderScale == 237 && balanced.DesktopMsaa == "8xmsaa" && balanced.HeadsetScale == 175);
        Check(GraphicsRecommendation.Apply(legacy, new(GraphicsRecommendation.Keep, 90, "")) == legacy with { VrRenderScale = 90 });
        Check(GraphicsCatalog.PresetId(GraphicsRecommendation.Apply(legacy, new("potato", 70, ""))) == "potato");
        Check(GraphicsCatalog.PresetId(balanced with { VrMsaa = "4xmsaa" }) == "custom");

        // The renderer's receipt and the session's graphics, as the Status call returns them.
        JsonElement Graphics(GraphicsOptions value) => JsonSerializer.SerializeToElement(new
        {
            qualityChoices = GraphicsCatalog.Categories.ToDictionary(c => c.Field, c => GraphicsCatalog.Get(value, c.Field)),
            resolution = new { multisampling = value.VrMsaa },
        });
        JsonElement Summary(string limit, double fps, double gpuPercent, int scale, double dipShare = 0, string dipLimit = "none") => JsonSerializer.SerializeToElement(new
        {
            schema = 1, targetFps = 90.0, adapter = new { name = "NVIDIA GeForce RTX 4080 SUPER" }, headset = new { width = 3072, height = 3264, scale },
            fps = new { windows = 240, p5 = fps - 8, p50 = fps }, gpuPercent = new { samples = 240, p50 = gpuPercent }, limit,
            blockedMs = new { p50 = .15 }, dips = new { share = dipShare, limit = dipLimit },
        });
        var measured = MeasuredPerformance.Parse(Summary("gpu", 74, 97, 80), Graphics(HighMsaa))!;
        Check(measured is { Usable: true, Scale: 80, Limit: "gpu", HeadsetWidth: 3072, Msaa: "2xmsaa", BlockedMedian: .15, DipShare: 0, DipLimit: "none" } && measured.Describe().Contains("limited by the GPU"));
        Check(MeasuredPerformance.Parse(Summary("at_target", 89.8, 60, 100, .12, "cpu"), Graphics(HighMsaa)) is { Dips: true, DipLimit: "cpu" } dipping &&
            Recommend(Quest3, null, dipping, HighMsaa).Reason.Contains("CPU was the limit"));
        Check(MeasuredPerformance.Parse(JsonSerializer.SerializeToElement(new { schema = 1, limit = "at_target" }), Graphics(HighMsaa)) is { DipShare: null, DipLimit: "unknown", BlockedMedian: null });
        Check(measured.SameSettings(HighMsaa with { VrRenderScale = 63 }) && !measured.SameSettings(HighMsaa with { VrMsaa = "off" }) && !measured.SameSettings(Balanced));
        Check(!MeasuredPerformance.Parse(Summary("gpu", 74, 97, 80), JsonSerializer.SerializeToElement(new { }))!.SameSettings(HighMsaa));
        Check(MeasuredPerformance.Parse(JsonSerializer.SerializeToElement(new { }), Graphics(HighMsaa)) is null);
        Check(!MeasuredPerformance.Parse(JsonSerializer.SerializeToElement(new { schema = 1, limit = "insufficient" }), Graphics(HighMsaa))!.Usable);

        var detected = GraphicsRecommendation.DetectGpu();
        Check(detected is null || (!string.IsNullOrWhiteSpace(detected.Name) && detected.DedicatedMb >= 0));

        using var form = new MainForm(client, new ProfileStore(client.Root, () => { }, () => null), approveAdvancedView: () => true);
        object? Call(string name, params object?[] args) => typeof(MainForm).GetMethod(name, Flags)!.Invoke(form, args);
        GraphicsOptions Current() => (GraphicsOptions)Call("CurrentGraphics")!;
        void Click(Button button) => typeof(Button).GetMethod("OnClick", Flags)!.Invoke(button, [EventArgs.Empty]);
        var recommend = (Button)form.Controls.Find("RecommendGraphics", true).Single();
        var undo = (Button)form.Controls.Find("UndoGraphicsPreset", true).Single();
        var label = (Label)form.Controls.Find("LastVrPerformance", true).Single();
        var notice = (Control)typeof(MainForm).GetField("notice", Flags)!.GetValue(form)!;
        foreach (var state in new[] { "loaded", "known" }) typeof(MainForm).GetField(state, Flags)!.SetValue(form, true);
        typeof(MainForm).GetField("headsetResolution", Flags)!.SetValue(form, null);
        Call("ApplyGraphics", legacy); Call("EnableActions"); var before = Current();
        Check(recommend.Enabled);
        Call("UpdateMeasuredPerformance", JsonSerializer.SerializeToElement(new { }), Graphics(before), false);
        Check(label.Text.StartsWith("None measured yet"));
        // GPU-limited with the settings still on the form: only render scale changes.
        Call("UpdateMeasuredPerformance", Summary("gpu", 74, 97, 80), Graphics(before), true);
        Check(label.Text.StartsWith("Current session so far: 74 FPS median"));
        Click(recommend);
        Check(Current() == before with { VrRenderScale = 70 } && GraphicsCatalog.PresetId(Current()) == GraphicsCatalog.PresetId(before) && undo.Enabled);
        // The reason can be selected and copied like the session status.
        var hint = (SelectableStatusText)form.Controls.Find("GraphicsPresetHint", true).Single();
        Check(hint.Text.Contains("GPU was the limit") && hint.ReadOnly && hint.ShortcutsEnabled && hint.TabStop);
        _ = hint.Handle; hint.SelectAll(); Check(hint.SelectedText == hint.Text);
        Click(undo);
        Check(Current() == before && !undo.Enabled);
        // Settings that already held the headset rate stay exactly as they are.
        Call("ApplyGraphics", before with { VrRenderScale = 90 }); var holding = Current();
        Call("UpdateMeasuredPerformance", Summary("at_target", 90, 85, 90), Graphics(holding), false);
        Click(recommend);
        Check(Current() == holding && !undo.Enabled && notice.Text.Contains("already the recommendation"));
        Call("SelectGraphicsPreset", "vrbalanced");
        Check(GraphicsCatalog.PresetId(Current()) == "vrbalanced" && Current().VrRenderScale == 90 && undo.Enabled);
        Check(!form.Visible);
    }
}
