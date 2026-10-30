using System.Runtime.InteropServices;
using System.Text.Json;

namespace GRID2VR;

internal sealed record GpuInfo(string Name, uint VendorId, uint DeviceId, long DedicatedMb);

// The renderer's performance-summary.json from the last VR session, with the
// quality choices and VR MSAA that session ran. Measured tuning applies only
// while the launcher still holds those same choices; render scale may differ.
internal sealed record MeasuredPerformance(double TargetFps, double FpsMedian, double FpsLow, int Windows, double? GpuPercent,
    string Limit, int HeadsetWidth, int HeadsetHeight, int Scale, string Adapter, IReadOnlyDictionary<string, string> Quality, string Msaa)
{
    internal const int MinimumWindows = 10;
    internal bool Usable => Windows >= MinimumWindows && Limit is "at_target" or "gpu" or "cpu" or "mixed" && TargetFps > 0 && FpsMedian > 0;
    // Median render-thread time blocked on the headset or Present; under 1 ms it had no idle time.
    internal double? BlockedMedian { get; init; }
    // Share of seconds below 95% of the headset rate, and what limited them.
    // Receipts from before dip tracking have neither; their 5th percentile decides.
    internal double? DipShare { get; init; }
    internal string DipLimit { get; init; } = "unknown";
    internal bool Dips => DipShare is { } share ? share >= .05 : FpsLow < TargetFps * .95;
    internal string DipText => (DipShare is { } share ? $"{share * 100:0}% of seconds" : "some seconds") + $" dropped (low {FpsLow:0} FPS)";

    internal bool SameSettings(GraphicsOptions current) => Msaa == current.VrMsaa &&
        GraphicsCatalog.Categories.All(c => Quality.TryGetValue(c.Field, out var choice) && choice == GraphicsCatalog.Get(current, c.Field));

    internal static MeasuredPerformance? Parse(JsonElement summary, JsonElement graphics)
    {
        if (summary.ValueKind != JsonValueKind.Object || Number(summary, "schema") != 1) return null;
        var fps = MainForm.Child(summary, "fps"); var headset = MainForm.Child(summary, "headset");
        var adapter = MainForm.TextValue(MainForm.Child(summary, "adapter"), "name", "");
        var choices = MainForm.Child(graphics, "qualityChoices");
        var quality = GraphicsCatalog.Categories.Select(c => (c.Field, Choice: MainForm.TextValue(choices, c.Field, "")))
            .Where(p => p.Choice.Length > 0).ToDictionary(p => p.Field, p => p.Choice);
        var dips = MainForm.Child(summary, "dips");
        return new(Number(summary, "targetFps") ?? 0, Number(fps, "p50") ?? 0, Number(fps, "p5") ?? 0, (int)(Number(fps, "windows") ?? 0),
            Number(MainForm.Child(summary, "gpuPercent"), "p50"), MainForm.TextValue(summary, "limit", "insufficient"),
            (int)(Number(headset, "width") ?? 0), (int)(Number(headset, "height") ?? 0), (int)(Number(headset, "scale") ?? 0),
            adapter.Length <= 256 ? adapter : "", quality, MainForm.TextValue(MainForm.Child(graphics, "resolution"), "multisampling", ""))
        {
            BlockedMedian = Number(MainForm.Child(summary, "blockedMs"), "p50"),
            DipShare = Number(dips, "share"),
            DipLimit = MainForm.TextValue(dips, "limit", "unknown"),
        };
    }

    private static double? Number(JsonElement value, string key) =>
        value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var item) && item.ValueKind == JsonValueKind.Number && item.TryGetDouble(out var number) && double.IsFinite(number) ? number : null;

    internal string Describe()
    {
        if (!Usable) return "Too short to measure. At least 10 seconds of uninterrupted driving in VR are needed.";
        var gpu = GpuPercent is { } percent ? $"GPU {percent:0}%" : "GPU usage unavailable";
        var limit = Limit switch
        {
            "at_target" when Dips => DipLimit switch
            {
                "gpu" => "mostly holding the headset rate, GPU-limited dips",
                "cpu" => "mostly holding the headset rate, CPU-limited dips",
                _ => "mostly holding the headset rate, with dips",
            } + (DipShare is { } share ? $" in {share * 100:0}% of seconds" : ""),
            "at_target" => "holding the headset rate",
            "gpu" => "limited by the GPU",
            "cpu" => "limited by the CPU",
            _ => "below the headset rate",
        };
        return $"{FpsMedian:0} FPS median, {FpsLow:0} low (headset {TargetFps:0}) · {gpu} · {limit} · {Scale}% render scale.";
    }
}

// Preset is Keep (current quality and VR MSAA, new scale only) or a preset id.
internal sealed record Recommendation(string Preset, int Scale, string Reason);

// A starting point, never a silent change: the launcher applies it to the form
// with Undo, and the player saves or launches to keep it.
internal static class GraphicsRecommendation
{
    internal const string Preset = "vrbalanced", Keep = "keep";
    // Pixels per view that VR Balanced should sustain at 90 Hz on an RTX 4080
    // class GPU, with margin for opponents and heavier tracks. Calibrated from
    // one 4080 SUPER session (High, 100% of 3072 × 3264, 2× MSAA, Brands Hatch,
    // no opponents): 90 FPS at 59% GPU. GRID2VR renders three views per frame.
    private const double BudgetPixels = 10.5e6;
    private const double LaptopFactor = .6;
    // Approximate desktop raster throughput relative to an RTX 4080. More
    // specific names come first because matching is by substring.
    private static readonly (string Name, double Index)[] Gpus =
    [
        ("RTX 5090", 1.55), ("RTX 4090", 1.30), ("RTX 5080", 1.10), ("RTX 4080 SUPER", 1.03), ("RTX 4080", 1.00),
        ("RTX 5070 Ti", .90), ("RTX 5070", .70), ("RTX 4070 Ti SUPER", .85), ("RTX 4070 Ti", .76), ("RTX 4070 SUPER", .70), ("RTX 4070", .60),
        ("RTX 5060 Ti", .50), ("RTX 5060", .42), ("RTX 4060 Ti", .43), ("RTX 4060", .35),
        ("RTX 3090 Ti", .80), ("RTX 3090", .72), ("RTX 3080 Ti", .70), ("RTX 3080", .64), ("RTX 3070 Ti", .50), ("RTX 3070", .46),
        ("RTX 3060 Ti", .40), ("RTX 3060", .32), ("RTX 3050", .20),
        ("RTX 2080 Ti", .45), ("RTX 2080 SUPER", .38), ("RTX 2080", .36), ("RTX 2070 SUPER", .33), ("RTX 2070", .29),
        ("RTX 2060 SUPER", .28), ("RTX 2060", .24), ("GTX 1080 Ti", .32), ("GTX 1080", .25), ("GTX 1070", .20),
        ("RX 9070 XT", .88), ("RX 9070", .78), ("RX 7900 XTX", .97), ("RX 7900 XT", .83), ("RX 7900 GRE", .70),
        ("RX 7800 XT", .62), ("RX 7700 XT", .50), ("RX 7600", .34), ("RX 6950 XT", .70), ("RX 6900 XT", .64),
        ("RX 6800 XT", .60), ("RX 6800", .52), ("RX 6750 XT", .45), ("RX 6700 XT", .44), ("RX 6600 XT", .31), ("RX 6600", .27),
        ("B580", .36), ("A770", .33), ("A750", .30),
    ];

    internal static double? GpuIndex(string name)
    {
        foreach (var (pattern, index) in Gpus)
            if (name.Contains(pattern, StringComparison.OrdinalIgnoreCase))
                return name.Contains("Laptop", StringComparison.OrdinalIgnoreCase) ? index * LaptopFactor : index;
        return null;
    }

    private static int RoundFive(double scale) => (int)Math.Round(scale / 5, MidpointRounding.AwayFromZero) * 5;

    internal static Recommendation Recommend(HeadsetResolution? headset, GpuInfo? gpu, MeasuredPerformance? last, GraphicsOptions current)
    {
        var width = headset?.Width ?? last?.HeadsetWidth ?? 0; var height = headset?.Height ?? last?.HeadsetHeight ?? 0;
        var maximum = Math.Clamp(headset?.MaximumRenderScale ?? 100, 50, 100);
        int Clamp(int scale) => Math.Clamp(scale, 50, maximum);
        var sameHeadset = headset is null || last is null || (headset.Width == last.HeadsetWidth && headset.Height == last.HeadsetHeight);
        var measured = last is { Usable: true, Scale: >= 50 and <= 300 } && sameHeadset;
        if (measured && last!.SameSettings(current))
        {
            var rate = $"{last.FpsMedian:0} of {last.TargetFps:0} FPS";
            var gpuText = last.GpuPercent is { } percent ? $"{percent:0}%" : "an unknown load";
            switch (last.Limit)
            {
                // A held median can hide slow seconds; tune for those instead.
                case "at_target" when last.Dips:
                    var dips = $"Your last VR session mostly held {last.TargetFps:0} FPS, but {last.DipText}";
                    return last.DipLimit switch
                    {
                        "gpu" => new(Keep, Clamp(Math.Min(RoundFive(last.Scale * Math.Sqrt(last.FpsLow / last.TargetFps)), last.Scale - 5)),
                            dips + $" because the GPU was the limit, so render scale goes down from {last.Scale}%."),
                        "cpu" => new(Keep, Clamp(last.Scale), dips + $" because the CPU was the limit, so render scale stays at {last.Scale}%: lowering it won't help. Fewer opponents and lower Crowd detail, Car reflections or Object detail reduce CPU work."),
                        _ => new(Keep, Clamp(last.Scale), dips + $" without one clear limit, so render scale stays at {last.Scale}%. Fewer opponents or a lower render scale can smooth them."),
                    };
                case "at_target" when last.GpuPercent is < 70 && last.Scale < maximum:
                    return new(Keep, Clamp(last.Scale + 10), $"Your last VR session held {last.TargetFps:0} FPS with these settings and the GPU at {gpuText}, so render scale goes up 10%. Press Recommend again after the next session.");
                case "at_target":
                    // Room for more only when both the GPU and the render thread had spare time.
                    return new(Keep, Clamp(last.Scale), $"Your last VR session held {last.TargetFps:0} FPS at {last.Scale}% with these settings, with the GPU at {gpuText}. Keep them." +
                        (last.GpuPercent is < 70 && last.BlockedMedian is >= 1 ? " The spare GPU and CPU time leaves room for more opponents or heavier tracks." : ""));
                // Measured on a 4080 SUPER: Ultra to High at the same 100% and
                // 2× MSAA cut each view's GPU time from 4.1 to 1.9 ms.
                case "gpu" when GraphicsCatalog.PresetId(current) is "ultra" or "working":
                    return new(Preset, Clamp(last.Scale), $"Ultra-level effects were the GPU limit at {rate}. High-level effects take about half the GPU time per view, so this switches to VR Balanced and keeps {Clamp(last.Scale)}% render scale.");
                case "gpu":
                    var lower = RoundFive(last.Scale * Math.Sqrt(last.FpsMedian / last.TargetFps) * .95);
                    if (lower < 60) return new("potato", 70, $"The GPU was the limit at {rate}, even at {last.Scale}%. Potato is the lightest preset.");
                    return new(Keep, Clamp(Math.Min(lower, last.Scale - 5)), $"The GPU was the limit at {rate} with these settings, so render scale goes down from {last.Scale}%.");
                case "cpu":
                    return new(Keep, Clamp(last.Scale), $"The CPU was the limit at {rate} with the GPU at {gpuText}, so render scale stays at {last.Scale}%: lowering it won't help. Fewer opponents and lower Crowd detail, Car reflections or Object detail reduce CPU work.");
                default:
                    return new(Keep, Clamp(RoundFive(last.Scale * .9)), $"Below the headset rate at {rate} without one clear limit, so render scale goes down 10%.");
            }
        }
        var name = !string.IsNullOrWhiteSpace(last?.Adapter) ? last.Adapter : gpu?.Name;
        var index = name is null ? null : GpuIndex(name);
        var basis = measured ? " Your last VR session used different quality or anti-aliasing settings, so its measurement was not used." : "";
        const string follow = " Launch VR once, then press Recommend again to fine-tune from the measured session.";
        if (index is null)
            return new(Preset, Clamp(80), (name is null ? "No graphics card was detected" : $"{name} has no estimate yet") + ", so this starts at 80%." + basis + follow);
        if (width <= 0 || height <= 0)
            return new(Preset, Clamp(80), $"Headset resolution unknown, so this starts at 80% for {name}. Connect the headset and select Refresh headset for a better estimate." + basis + follow);
        var estimate = RoundFive(100 * Math.Sqrt(BudgetPixels * index.Value / ((double)width * height)));
        if (estimate < 60)
            return new("potato", 70, $"{name} at {width} × {height} per eye is below the VR Balanced range, so this picks Potato, the lightest preset." + basis + follow);
        return new(Preset, Clamp(estimate), $"Estimated for {name} at {width} × {height} per eye." + basis + follow);
    }

    internal static GraphicsOptions Apply(GraphicsOptions value, Recommendation recommendation) => recommendation.Preset switch
    {
        Keep => value with { VrRenderScale = recommendation.Scale },
        "potato" => GraphicsCatalog.ApplyPreset(value, "potato"),
        _ => GraphicsCatalog.ApplyPreset(value, recommendation.Preset) with { VrRenderScale = recommendation.Scale },
    };

    // The hardware adapter with the most dedicated memory; software adapters are skipped.
    internal static GpuInfo? DetectGpu()
    {
        IDXGIFactory1? factory = null;
        try
        {
            if (CreateDXGIFactory1(typeof(IDXGIFactory1).GUID, out factory) < 0 || factory is null) return null;
            GpuInfo? best = null;
            for (uint index = 0; index < 16 && factory.EnumAdapters1(index, out var adapter) >= 0; ++index)
            {
                try
                {
                    if (adapter.GetDesc1(out var desc) < 0 || (desc.Flags & SoftwareAdapter) != 0 || desc.VendorId == MicrosoftVendor) continue;
                    var memory = (long)(ulong)desc.DedicatedVideoMemory / (1024 * 1024);
                    if (best is null || memory > best.DedicatedMb) best = new(desc.Description.Trim(), desc.VendorId, desc.DeviceId, memory);
                }
                finally { Marshal.ReleaseComObject(adapter); }
            }
            return best;
        }
        catch (Exception error) when (error is COMException or DllNotFoundException or EntryPointNotFoundException or InvalidCastException) { return null; }
        finally { if (factory is not null) Marshal.ReleaseComObject(factory); }
    }

    private const uint SoftwareAdapter = 2, MicrosoftVendor = 0x1414;
    [DllImport("dxgi.dll", ExactSpelling = true)]
    private static extern int CreateDXGIFactory1(in Guid riid, [MarshalAs(UnmanagedType.Interface)] out IDXGIFactory1 factory);

    // Vtable order only; the placeholder methods are never called.
    [ComImport, Guid("770aae78-f26f-4dba-a829-253c83d1b387"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDXGIFactory1
    {
        void SetPrivateData(); void SetPrivateDataInterface(); void GetPrivateData(); void GetParent();
        void EnumAdapters(); void MakeWindowAssociation(); void GetWindowAssociation(); void CreateSwapChain(); void CreateSoftwareAdapter();
        [PreserveSig] int EnumAdapters1(uint index, out IDXGIAdapter1 adapter);
        void IsCurrent();
    }

    [ComImport, Guid("29038f61-3839-4626-91fd-086879011a05"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDXGIAdapter1
    {
        void SetPrivateData(); void SetPrivateDataInterface(); void GetPrivateData(); void GetParent();
        void EnumOutputs(); void GetDesc(); void CheckInterfaceSupport();
        [PreserveSig] int GetDesc1(out AdapterDescription description);
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct AdapterDescription
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
        public uint VendorId, DeviceId, SubSysId, Revision;
        public nuint DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
        public uint LuidLow; public int LuidHigh; public uint Flags;
    }
}
