using System.Text.Json;

namespace GRID2VR;

internal static class VrResolutionTests
{
    private static void Check(bool value, [System.Runtime.CompilerServices.CallerArgumentExpression(nameof(value))] string? expression = null)
    { if (!value) throw new InvalidOperationException("VR resolution regression: " + expression); }

    internal static void Run(SessionClient client)
    {
        var original = client.Load();
        Check(new Choices().Graphics.VrRenderScale == 100);
        foreach (var (render, output) in new[] { (100, 100), (200, 200), (300, 200), (237, 175), (50, 25) })
        {
            var legacy = SessionClient.ParseChoices(JsonSerializer.Serialize(new { Graphics = new { RenderScale = render, HeadsetScale = output, UseVrResolutionPreset = true } }));
            Check(legacy.Graphics.VrRenderScale == 100 && legacy.Graphics.RenderScale == render && legacy.Graphics.HeadsetScale == output && legacy.Graphics.EffectiveDesktopRenderScale == render);
            client.Save(legacy); Check(client.Load() == legacy);
        }
        foreach (var scale in new[] { 50, 75, 100, 137, 200, 300 })
        {
            var selected = original with { Graphics = original.Graphics with { VrRenderScale = scale, DesktopRenderScale = 237 } };
            client.Save(selected); Check(client.Load() == selected);
            foreach (var mode in new[] { "Normal", "Practice", "Race" })
                foreach (var vr in new[] { false, true })
                {
                    var args = client.LaunchCommand(selected with { LaunchMode = mode }, vr).ArgumentList;
                    Check(!args.Contains("-HeadsetScale") && args.Contains("-VrRenderScale") == vr);
                    if (vr) Check(args[args.IndexOf("-VrRenderScale") + 1] == scale.ToString());
                    else Check(args[args.IndexOf("-RenderScale") + 1] == "237");
                }
        }
        var headset = HeadsetResolution.Parse("{\"runtime\":\"VDXR\",\"headset\":\"Test headset\",\"width\":2400,\"height\":2600}", "VDXR");
        Check(headset.Summary(50).Contains("Render: 1200 × 1300 per eye") && headset.Summary(100).Contains("Render: 2400 × 2600 per eye"));
        Check(headset.Summary(150).Contains("Render: 3600 × 3900 per eye") && headset.Summary(150).Contains("Output: 2400 × 2600 per eye"));
        Check(!headset.Summary(200).Contains("Exceeds renderer limits"));
        var quest = new HeadsetResolution("VDXR", "Meta Quest 3", 3072, 3264);
        Check(quest.Summary(150).Contains("Render: 4608 × 4896 per eye") && quest.Summary(200).Contains("Render: 6144 × 6528 per eye"));
        Check(quest.MaximumRenderScale == 251 && !quest.Summary(251).Contains("Exceeds renderer limits") && quest.Summary(252).Contains("Exceeds renderer limits"));
        foreach (var scale in new[] { 49, 301 })
        {
            var rejected = false;
            try { client.Save(original with { Graphics = original.Graphics with { VrRenderScale = scale } }); }
            catch (ArgumentException) { rejected = true; }
            Check(rejected);
        }
        foreach (var invalid in new[] { headset with { Runtime = "SteamVR" }, headset with { Width = 0 }, headset with { Width = 8193 }, headset with { Width = 8192, Height = 8192 } })
        {
            var rejected = false;
            try { HeadsetResolution.Parse(JsonSerializer.Serialize(invalid), "VDXR"); } catch (IOException) { rejected = true; }
            Check(rejected);
        }
        client.Save(original);
    }

    internal static void RunForm(MainForm form, SessionClient client)
    {
        const System.Reflection.BindingFlags flags = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        object? Call(string name, params object[] args) => typeof(MainForm).GetMethod(name, flags)!.Invoke(form, args);
        void Apply(GraphicsOptions value) => Call("ApplyGraphics", value);
        GraphicsOptions Current() => (GraphicsOptions)Call("CurrentGraphics")!;
        var render = (RenderScaleSlider)form.Controls.Find("VrRenderScale", true).Single();
        var desktop = (RenderScaleSlider)form.Controls.Find("DesktopRenderScale", true).Single();
        var summary = (Label)form.Controls.Find("VrResolutionSummary", true).Single();
        var tabs = (TabControl)form.Controls.Find("LauncherTabs", true).Single();
        Check(tabs.TabPages.Cast<TabPage>().Single(p => p.Text == "Graphics").Contains(render));
        Check(form.Controls.Find("HeadsetScale", true).Length == 0 && form.Controls.Find("VrResolution", true).Length == 0);
        var original = client.Load();
        var legacy = new GraphicsOptions(237, HeadsetScale: 175) { VrMsaa = "4xmsaa", HudDistance = 3m };
        Apply(legacy); Check(Current() == legacy && render.Value == 100 && desktop.Value == 237);
        var runtime = (ComboBox)typeof(MainForm).GetField("runtime", flags)!.GetValue(form)!;
        typeof(MainForm).GetField("headsetResolution", flags)!.SetValue(form, new HeadsetResolution((string)runtime.SelectedItem!, "Test headset", 2400, 2600));
        foreach (var scale in new[] { 50, 100, 137, 150 })
        {
            render.Value = scale;
            Check(Current() == (legacy with { VrRenderScale = scale }) && desktop.Value == 237);
            Check(summary.Text.Contains("Output: 2400 × 2600 per eye") && summary.Text.Contains($"Render: {2400 * scale / 100} × {2600 * scale / 100} per eye"));
            client.Save(original with { Graphics = Current() }); Apply(client.Load().Graphics);
            Check(Current().VrRenderScale == scale && Current().VrMsaa == "4xmsaa");
        }
        desktop.Value = 300; Check(Current().VrRenderScale == 150 && Current().EffectiveDesktopRenderScale == 300);
        foreach (var state in new[] { "active", "pending", "busy", "capturingRecenter" })
        {
            typeof(MainForm).GetField(state, flags)!.SetValue(form, true); Call("EnableActions");
            Check(!render.Enabled && !desktop.Enabled && !form.Controls.Find("RefreshHeadset", true).Single().Enabled);
            typeof(MainForm).GetField(state, flags)!.SetValue(form, false); Call("EnableActions");
            Check(render.Enabled && desktop.Enabled);
        }
        Apply(GraphicsOptions.Defaults); Check(Current() == GraphicsOptions.Defaults && !form.Visible);
        client.Save(original);
    }
}
