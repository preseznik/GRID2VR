using System.Reflection;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;

namespace GRID2VR;

internal static class GraphicsPresetTests
{
    private const BindingFlags Flags = BindingFlags.Instance | BindingFlags.NonPublic;
    private static void Check(bool value, [System.Runtime.CompilerServices.CallerArgumentExpression(nameof(value))] string? text = null)
    { if (!value) throw new InvalidOperationException("Graphics preset regression: " + text); }
    private static void Reject(Action action)
    {
        var rejected = false;
        try { action(); } catch (ArgumentException) { rejected = true; }
        Check(rejected);
    }

    internal static void Run(string root, SessionClient fixture)
    {
        var client = new SessionClient(Path.Combine(fixture.Root, "expanded-graphics"));
        Check(!client.UseAdvancedView(new())); // Fresh install starts Simple.
        var original = fixture.Load();
        var legacy = new GraphicsOptions(237, false, "8xmsaa", "medium", "high", "low", "high", 175, "4xmsaa")
        { VrRenderScale = 137, DesktopRenderScale = 219, HudDistance = 4.5m, HudFollowView = true, DesktopBorderless = false };
        var choices = original with { Graphics = legacy };
        client.Save(choices);
        Check(client.UseAdvancedView(new())); // Existing install keeps Advanced.
        Check(!client.UseAdvancedView(new(AdvancedView: false)) && client.UseAdvancedView(new(AdvancedView: true)));
        Check(GraphicsCatalog.Categories.Count == 21 && GraphicsCatalog.Presets.Count == 5);
        Check(GraphicsCatalog.PresetId(new()) == "working");
        foreach (var category in GraphicsCatalog.Categories)
        {
            Check(GraphicsCatalog.Get(new(), category.Field) == category.Default);
            foreach (var choice in category.Choices)
            {
                if (!category.Available && choice.Id != category.Default)
                { Reject(() => GraphicsCatalog.WithQuality(legacy, new Dictionary<string, string> { [category.Field] = choice.Id })); continue; }
                var graphics = GraphicsCatalog.WithQuality(legacy, new Dictionary<string, string> { [category.Field] = choice.Id });
                client.Save(choices with { Graphics = graphics }); Check(client.Load().Graphics == graphics);
                Check(GraphicsCatalog.Get(graphics, category.Field) == choice.Id);
                Check(graphics.VrRenderScale == 137 && graphics.EffectiveDesktopRenderScale == 219 && graphics.VrMsaa == "4xmsaa" && graphics.HudDistance == 4.5m);
            }
            Reject(() => GraphicsCatalog.WithQuality(legacy, new Dictionary<string, string> { [category.Field] = "invented" }));
        }
        Reject(() => GraphicsCatalog.WithQuality(legacy, new Dictionary<string, string> { ["RenderScale"] = "100" }));
        foreach (var preset in GraphicsCatalog.Presets)
        {
            var graphics = GraphicsCatalog.ApplyPreset(legacy, preset.Id);
            Check(GraphicsCatalog.PresetId(graphics) == preset.Id);
            Check(graphics.VrRenderScale == legacy.VrRenderScale && graphics.EffectiveDesktopRenderScale == legacy.EffectiveDesktopRenderScale && graphics.VrMsaa == legacy.VrMsaa && graphics.DesktopMsaa == legacy.DesktopMsaa);
            Check(graphics.IntelAOIT == "off" && graphics.IntelAVSM == "off");
            client.Save(choices with { Graphics = graphics }); Check(client.Load().Graphics == graphics);
            foreach (var mode in new[] { "Normal", "Practice", "Race" })
            foreach (var vr in new[] { false, true })
            {
                var args = client.LaunchCommand(choices with { Graphics = graphics, LaunchMode = mode }, vr).ArgumentList;
                var payload = Convert.FromBase64String(args[args.IndexOf("-GraphicsQualityBase64") + 1]);
                var expanded = JsonSerializer.Deserialize<Dictionary<string, string>>(payload)!;
                Check(expanded.Count == 17 && expanded.All(p => GraphicsCatalog.Get(graphics, p.Key) == p.Value));
                Check(!expanded.Keys.Intersect(GraphicsCatalog.LegacyFields).Any());
                Check(args[args.IndexOf("-CrowdDetail") + 1] == graphics.CrowdDetail);
            }
        }
        var potato = GraphicsCatalog.ApplyPreset(legacy, "potato");
        Check(GraphicsCatalog.PresetId(potato) == "potato" && potato.VrRenderScale == 70 && potato.VrMsaa == "off");
        Check(potato.CrowdDetail == "off" && potato.VehicleDetail == "low" && potato.Shadows == "ultralow" && potato.SoftAmbientOcclusion == "off");
        Check(potato.EffectiveDesktopRenderScale == 219 && potato.DesktopMsaa == "8xmsaa" && potato.HeadsetScale == 175 && potato.HudDistance == 4.5m);
        var custom = potato with { Shadows = "medium" };
        Check(GraphicsCatalog.PresetId(custom) == "custom");
        var oldJson = "{\"Graphics\":{\"RenderScale\":237,\"TreeDetail\":\"medium\",\"CrowdDetail\":\"low\",\"VrMsaa\":\"4xmsaa\"}}";
        var upgraded = SessionClient.ParseChoices(oldJson).Graphics;
        Check(upgraded.RenderScale == 237 && upgraded.TreeDetail == "medium" && upgraded.CrowdDetail == "low" && upgraded.VrMsaa == "4xmsaa" && upgraded.SoftAmbientOcclusion == "on" && upgraded.Shadows == "ultra");
        RunProfilePipeline(root, client, legacy);
        RunForm(root, client, choices);
        fixture.Save(original);
    }

    private static void RunProfilePipeline(string root, SessionClient client, GraphicsOptions original)
    {
        using var catalogStream = typeof(GraphicsCatalog).Assembly.GetManifestResourceStream("GRID2VR.GraphicsOptions.json")!;
        var catalogHash = Convert.ToHexStringLower(SHA256.HashData(catalogStream));
        foreach (var preset in GraphicsCatalog.Presets.Select(p => p.Id).Append("potato"))
        foreach (var mode in new[] { "VR", "Desktop" })
        {
            var graphics = GraphicsCatalog.ApplyPreset(original, preset);
            var output = Path.Combine(client.Root, $"profile-{preset}-{mode}.xml");
            var info = new ProcessStartInfo(AppPaths.Python(root))
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = root };
            info.Environment.Remove("PYTHONHOME"); info.Environment.Remove("PYTHONPATH");
            info.Environment["PYTHONDONTWRITEBYTECODE"] = "1"; info.Environment["PYTHONNOUSERSITE"] = "1";
            foreach (var argument in new[] { "-B", Path.Combine(root, "tools", "graphics_profile.py"), "--output", output,
                "--game-root", Path.Combine(client.Root, "inert-game"), "--mode", mode, "--quality-base64", GraphicsCatalog.EncodeExpanded(graphics),
                "--tree-detail", graphics.TreeDetail, "--object-detail", graphics.ObjectDetail, "--crowd-detail", graphics.CrowdDetail,
                "--texture-filtering", graphics.TextureFiltering, "--vr-msaa", graphics.VrMsaa, "--desktop-msaa", graphics.DesktopMsaa,
                "--render-scale", graphics.EffectiveDesktopRenderScale.ToString(System.Globalization.CultureInfo.InvariantCulture) }) info.ArgumentList.Add(argument);
            if (mode == "VR")
                foreach (var argument in new[] { "--scene-width", (2400 * graphics.VrRenderScale / 100).ToString(), "--scene-height", (2600 * graphics.VrRenderScale / 100).ToString() })
                    info.ArgumentList.Add(argument);
            using var process = Process.Start(info)!;
            var stdout = process.StandardOutput.ReadToEndAsync(); var stderr = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(30000)) { process.Kill(entireProcessTree: true); throw new IOException("Offline graphics fixture timed out."); }
            var text = stdout.GetAwaiter().GetResult(); var error = stderr.GetAwaiter().GetResult();
            if (process.ExitCode != 0) throw new IOException("Offline graphics fixture failed: " + error);
            using var document = JsonDocument.Parse(text);
            var receipt = document.RootElement;
            Check(receipt.GetProperty("catalogSha256").GetString() == catalogHash);
            Check(receipt.GetProperty("sha256").GetString() == Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(output))));
            foreach (var category in GraphicsCatalog.Categories)
                Check(receipt.GetProperty("qualityChoices").GetProperty(category.Field).GetString() == GraphicsCatalog.Get(graphics, category.Field));
            Check(receipt.GetProperty("resolution").GetProperty("multisampling").GetString() == (mode == "VR" ? graphics.VrMsaa : graphics.DesktopMsaa));
            Check(receipt.GetProperty("installed").ValueKind == JsonValueKind.False);
        }
    }

    private static void RunForm(string root, SessionClient client, Choices choices)
    {
        foreach (var modern in new[] { false, true })
        {
            using var form = new MainForm(client, new ProfileStore(client.Root, () => { }, () => null), approveAdvancedView: () => true);
            LauncherAppearance.Modern = modern; LauncherAppearance.Apply(form);
            LaunchModeTests.RunForm(form);
            object? Call(string name, params object[] args) => typeof(MainForm).GetMethod(name, Flags)!.Invoke(form, args);
            GraphicsOptions Current() => (GraphicsOptions)Call("CurrentGraphics")!;
            var picker = (LauncherViewPicker)form.Controls.Find("LauncherView", true).Single();
            var tabs = (TabControl)form.Controls.Find("LauncherTabs", true).Single();
            var originalControls = Descendants(form).ToArray();
            var mode = (ComboBox)form.Controls.Find("LaunchMode", true).Single();
            var potato = (Button)form.Controls.Find("PotatoPreset", true).Single();
            var undo = (Button)form.Controls.Find("UndoGraphicsPreset", true).Single();
            var advancedShadow = (ComboBox)form.Controls.Find("Shadows", true).Single();
            var original = choices.Graphics;
            Call("ApplyGraphics", original); var before = Current();
            foreach (var state in new[] { "active", "pending", "busy", "capturingRecenter" })
            {
                typeof(MainForm).GetField(state, Flags)!.SetValue(form, true); Call("EnableActions");
                Check(!potato.Enabled && !advancedShadow.Enabled);
                typeof(MainForm).GetField(state, Flags)!.SetValue(form, false); Call("EnableActions");
            }
            Check(!form.Controls.Find("IntelAOIT", true).Single().Enabled && !form.Controls.Find("IntelAVSM", true).Single().Enabled);
            Call("SelectGraphicsPreset", "potato");
            Check(Current() == GraphicsCatalog.ApplyPreset(before, "potato") && undo.Enabled);
            // Invoke the actual button handler without displaying/activating a window.
            typeof(Button).GetMethod("OnClick", Flags)!.Invoke(undo, [EventArgs.Empty]);
            Check(Current() == before && !undo.Enabled);
            Call("SelectGraphicsPreset", "high");
            ((RenderScaleSlider)form.Controls.Find("DesktopRenderScale", true).Single()).Value = 218;
            Check(!undo.Enabled && Current().EffectiveDesktopRenderScale == 218);
            var editedDesktop = Current();
            typeof(Button).GetMethod("OnClick", Flags)!.Invoke(undo, [EventArgs.Empty]);
            Check(Current() == editedDesktop);
            Call("SelectGraphicsPreset", "medium");
            Check(undo.Enabled);
            ((HudDistanceSlider)form.Controls.Find("HudDistance", true).Single()).Value = 5m;
            Check(!undo.Enabled && Current().HudDistance == 5m);
            Call("SelectGraphicsPreset", "potato");
            var msaa = (ComboBox)form.Controls.Find("VrMSAA", true).Single();
            var presetPicker = (ComboBox)form.Controls.Find("GraphicsPreset", true).Single();
            msaa.SelectedItem = msaa.Items.Cast<ContentItem>().Single(c => c.Id == "4xmsaa");
            Check(((ContentItem)presetPicker.SelectedItem!).Id == "ultralow" && !undo.Enabled);
            msaa.SelectedItem = msaa.Items.Cast<ContentItem>().Single(c => c.Id == "off");
            Check(((ContentItem)presetPicker.SelectedItem!).Id == "potato");
            typeof(Button).GetMethod("OnClick", Flags)!.Invoke(form.Controls.Find("GraphicsDefaults", true).Single(), [EventArgs.Empty]);
            Check(!undo.Enabled && Current() == GraphicsOptions.Defaults);
            Call("SelectGraphicsPreset", "medium");
            advancedShadow.SelectedItem = advancedShadow.Items.Cast<ContentItem>().Single(c => c.Id == "ultra");
            Check(((ContentItem)((ComboBox)form.Controls.Find("GraphicsPreset", true).Single()).SelectedItem!).Id == "custom");
            var custom = Current();
            for (var i = 0; i < 3; ++i)
            {
                picker.Advanced = false;
                Check(tabs.TabCount == 1 && tabs.TabPages[0].Text == "Quick start" && Current() == custom);
                Check(form.Controls.Find("GraphicsPreset", true).Length == 1 && form.Controls.Find("Runtime", true).Length == 1);
                Check(form.Controls.Find("Shadows", true).Length == 0); // No duplicated advanced editor.
                picker.Advanced = true;
                Check(Current() == custom && Descendants(form).SequenceEqual(originalControls));
            }
            // Saving just the view must not silently save pending graphics/car edits.
            client.Save(choices); var settings = File.ReadAllBytes(Path.Combine(client.Artifacts, "launcher-settings.json"));
            typeof(MainForm).GetField("windowLoaded", Flags)!.SetValue(form, true);
            picker.Advanced = false;
            Check(client.LoadWindow().AdvancedView == false && File.ReadAllBytes(Path.Combine(client.Artifacts, "launcher-settings.json")).SequenceEqual(settings));
            foreach (var id in new[] { "Normal", "Practice", "Race" })
            {
                mode.SelectedItem = mode.Items.Cast<ContentItem>().Single(c => c.Id == id);
                Check(Current() == custom);
                Capture(tabs.TabPages[0], root, $"launcher-simple-{id.ToLowerInvariant()}-{(modern ? "modern" : "classic")}", [460, 820, 1320]);
            }
            picker.Advanced = true;
            Check(client.LoadWindow().AdvancedView == true && Current() == custom);
            var graphicsPage = tabs.TabPages.Cast<TabPage>().Single(p => p.Text == "Graphics");
            Capture(graphicsPage, root, $"launcher-expanded-graphics-{(modern ? "modern" : "classic")}", [460, 1320]);
            Check(!form.Visible);
        }
    }

    private static void Capture(TabPage page, string root, string name, int[] widths)
    {
        var content = page.Controls[0]; content.Font = page.FindForm()?.Font ?? page.Font; page.Controls.Remove(content);
        try
        {
            content.Visible = true; content.Dock = DockStyle.None;
            foreach (var width in widths)
            {
                content.Width = width; content.PerformLayout();
                content.CreateControl();
                foreach (var control in Descendants(content).Where(c => c.Visible)) control.CreateControl();
                foreach (var row in Descendants(content).OfType<SettingRow>().Where(r => r.Visible))
                {
                    Check(row.Controls.Cast<Control>().All(c => row.ClientRectangle.Contains(c.Bounds)));
                    Check(row.Controls.Cast<Control>().All(c => c.Visible));
                }
                using var bitmap = new Bitmap(content.Width, content.Height);
                content.DrawToBitmap(bitmap, content.ClientRectangle);
                bitmap.Save(Path.Combine(root, "build", $"{name}-{width}.png"));
                if (width == 820 && name == "launcher-simple-practice-modern")
                    File.WriteAllText(Path.Combine(root, "build", "launcher-simple-layout.json"), JsonSerializer.Serialize(
                        Descendants(content).Where(c => c is SettingRow or ComboBox or Label).Select(c => new
                        { c.Name, c.Text, c.Visible, bounds = c.Bounds.ToString(), font = c.Font.Size }), new JsonSerializerOptions { WriteIndented = true }));
            }
        }
        finally { page.Controls.Add(content); }
    }

    private static IEnumerable<Control> Descendants(Control parent)
    {
        foreach (Control child in parent.Controls)
        {
            yield return child;
            foreach (var nested in Descendants(child)) yield return nested;
        }
    }
}
