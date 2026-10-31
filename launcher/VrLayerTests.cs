using System.Reflection;
using System.Text.Json;

namespace GRID2VR;

internal static class VrLayerTests
{
    private static void Check(bool value, [System.Runtime.CompilerServices.CallerArgumentExpression(nameof(value))] string? expression = null)
    { if (!value) throw new InvalidOperationException("OpenXR layer recovery regression: " + expression); }
    private static bool Rejects(Action action) { try { action(); return false; } catch (IOException) { return true; } }
    private static void Click(Control control) => typeof(Control).GetMethod("OnClick", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(control, [EventArgs.Empty]);

    internal static void Run(string root, SessionClient client)
    {
        var reshade = new VrLayerBypass("XR_APILAYER_reshade", @"C:\ReShade\reshade.json", new string('A', 64), @"C:\ReShade\ReShade64.dll", "DISABLE_XR_APILAYER_reshade");
        var obs = new VrLayerBypass("XR_APILAYER_NOVENDOR_OBSMirror", @"C:\OBS\obs-mirror.json", new string('B', 64), @"C:\OBS\obs-mirror.dll", "DISABLE_XR_APILAYER_NOVENDOR_OBSMirror");
        Check(reshade.Valid && obs.Valid && reshade.DisplayName == "ReShade" && obs.DisplayName == "OpenXR OBS Mirror" && obs.ToString() == "OpenXR OBS Mirror");
        Check((reshade with { LayerName = "XR_APILAYER_other" }).Consequence.StartsWith("Features provided by this OpenXR layer"));
        foreach (var invalid in new[] { reshade with { DisableVariable = "PATH" }, reshade with { DisableVariable = "XR_RUNTIME_JSON" }, reshade with { DisableVariable = "DISABLE_XR_A;B" },
            reshade with { LayerName = "reshade" }, reshade with { ManifestHash = "abc" }, reshade with { LibraryPath = "ReShade64.dll" }, reshade with { ManifestPath = "" } })
            Check(!invalid.Valid && Rejects(() => VrApiLayers.Encode([invalid])));
        Check(reshade.SameIdentity(reshade with { ManifestPath = reshade.ManifestPath.ToUpperInvariant() }) && !reshade.SameIdentity(reshade with { ManifestHash = new string('C', 64) }) &&
            !reshade.SameIdentity(reshade with { DisableVariable = "DISABLE_XR_APILAYER_RESHADE" }) && !reshade.SameIdentity(null));

        // The transport is what tools/OpenXR-Layers.ps1 reads: camelCase identity, no display text.
        using (var transport = JsonDocument.Parse(Convert.FromBase64String(VrApiLayers.Encode([reshade, obs]))))
        {
            var first = transport.RootElement[0];
            Check(transport.RootElement.GetArrayLength() == 2 && first.GetProperty("layerName").GetString() == reshade.LayerName &&
                first.GetProperty("manifestHash").GetString() == reshade.ManifestHash && first.GetProperty("disableVariable").GetString() == reshade.DisableVariable &&
                !first.TryGetProperty("displayName", out _) && !first.TryGetProperty("consequence", out _));
        }
        Check(Rejects(() => VrApiLayers.Encode(Enumerable.Repeat(reshade, VrApiLayers.MaxLayers + 1).ToArray())));

        // Approved layers reach the worker only for VR, and the headset check for any query.
        var choices = client.Load();
        var vrArgs = client.LaunchCommand(choices, true, [reshade]).ArgumentList;
        Check(vrArgs[vrArgs.IndexOf("-XrLayerBypassBase64") + 1] == VrApiLayers.Encode([reshade]));
        Check(!client.LaunchCommand(choices, true).ArgumentList.Contains("-XrLayerBypassBase64") && !client.LaunchCommand(choices, true, []).ArgumentList.Contains("-XrLayerBypassBase64"));
        Check(!client.LaunchCommand(choices, false, [reshade]).ArgumentList.Contains("-XrLayerBypassBase64"));
        var query = client.HeadsetQueryCommand("SteamVR", [obs]).ArgumentList;
        Check(query[query.IndexOf("-Runtime") + 1] == "SteamVR" && query[query.IndexOf("-LayerBypassBase64") + 1] == VrApiLayers.Encode([obs]));
        Check(!client.HeadsetQueryCommand("VDXR", []).ArgumentList.Contains("-LayerBypassBase64"));

        // Get-VrResolution.ps1 results.
        string Layer(VrLayerBypass layer) => JsonSerializer.Serialize(new { layerName = layer.LayerName, manifestPath = layer.ManifestPath, manifestHash = layer.ManifestHash, libraryPath = layer.LibraryPath, disableVariable = layer.DisableVariable });
        var headset = "{\"runtime\":\"VDXR\",\"headset\":\"Pico 4 Ultra\",\"width\":2160,\"height\":2160,\"layerBypasses\":[" + Layer(reshade) + "]}";
        var passed = VrHeadsetPreflight.Parse(0, headset, "", "VDXR", [reshade, obs]);
        Check(passed.Recovery is null && passed.Failure is null && passed.Applied.Count == 1 && passed.Applied[0].SameIdentity(reshade));
        Check(VrHeadsetPreflight.Parse(0, headset.Replace("[" + Layer(reshade) + "]", "[]"), "", "VDXR", [reshade]).Applied.Count == 0);
        var recovery = "{\"message\":\"An OpenXR layer could not load: XR_APILAYER_NOVENDOR_OBSMirror (Windows error 193).\",\"layers\":[" + Layer(obs) + "],\"layerBypasses\":[" + Layer(reshade) + "],\"report\":\"exit_code=3\\ncreate_instance=-32\"}";
        var blocked = VrHeadsetPreflight.Parse(3, recovery, "An OpenXR layer could not load. Launch VR to review it.", "VDXR", [reshade]);
        Check(blocked.Recovery is { Layers.Count: 1 } found && found.Layers[0].SameIdentity(obs) && found.Report == "exit_code=3\r\ncreate_instance=-32" &&
            found.Message.Contains("Windows error 193") && blocked.Applied.Single().SameIdentity(reshade));
        var failed = VrHeadsetPreflight.Parse(1, "", "Could not read the headset resolution (query exit 4, get_headset_system=-34).\n", "VDXR", [obs]);
        Check(failed.Failure == "Could not read the headset resolution (query exit 4, get_headset_system=-34)." && failed.Recovery is null && failed.Applied.Single() == obs);
        foreach (var invalid in new[] { (0, "{"), (0, headset.Replace("VDXR", "SteamVR")), (0, headset.Replace(reshade.DisableVariable, "PATH")), (3, recovery.Replace("[" + Layer(obs) + "]", "[]")),
            (3, recovery.Replace("\"report\"", "\"other\"")), (3, "[]") })
            Check(Rejects(() => VrHeadsetPreflight.Parse(invalid.Item1, invalid.Item2, "", "VDXR", [])));

        // The launch flow: cancel, retry, remember by choice, and stale or still-failing layers.
        VrLayerRecovery? shown = null;
        var rememberedLayers = new List<VrLayerBypass>();
        var progress = new List<string>();
        (IReadOnlyList<VrLayerBypass> Layers, string Warning)? Flow(Queue<VrHeadsetPreflight> results, VrLayerRecoveryChoice? choice, List<IReadOnlyList<VrLayerBypass>> requests, Exception? rememberFailure = null)
        {
            rememberedLayers.Clear(); progress.Clear(); shown = null;
            return VrLayerRecoveryFlow.Check([reshade], layers => { requests.Add(layers); return Task.FromResult(results.Dequeue()); },
                request => { shown = request; return choice; },
                layers => { if (rememberFailure is not null) throw rememberFailure; rememberedLayers.AddRange(layers); }, progress.Add).GetAwaiter().GetResult();
        }
        var calls = new List<IReadOnlyList<VrLayerBypass>>();
        Check(Flow(new([passed]), null, calls) is { Warning: "" } clean && clean.Layers.Single().SameIdentity(reshade) && calls.Single().Single() == reshade && shown is null);
        calls.Clear();
        Check(Flow(new([failed]), null, calls) is { } other && other.Layers.Single() == obs && shown is null);
        calls.Clear();
        Check(Flow(new([blocked]), new(false, true), calls) is null && shown == blocked.Recovery && calls.Count == 1 && rememberedLayers.Count == 0);
        Check(Flow(new([blocked]), null, calls) is null);
        var both = VrHeadsetPreflight.Parse(0, headset.Replace("[" + Layer(reshade) + "]", "[" + Layer(reshade) + "," + Layer(obs) + "]"), "", "VDXR", []);
        calls.Clear();
        Check(Flow(new([blocked, both]), new(true, false), calls) is { Warning: "" } retried && retried.Layers.Count == 2 && rememberedLayers.Count == 0 &&
            calls[1].Count == 2 && calls[1][1] == obs && progress.SequenceEqual(["Review the VR compatibility prompt to continue…", "Checking the VR headset…"]));
        Check(Flow(new([blocked, both]), new(true, true), []) is { Warning: "" } && rememberedLayers.Single() == obs);
        Check(Flow(new([blocked, both]), new(true, true), [], new IOException("disk full")) is { } unsaved && unsaved.Warning.EndsWith("could not be remembered: disk full") && unsaved.Layers.Count == 2);
        foreach (var retry in new[] { blocked, failed, passed, both with { Applied = [reshade, obs with { ManifestHash = new string('D', 64) }] } })
        {
            var error = "";
            try { Flow(new([blocked, retry]), new(true, true), []); } catch (IOException failure) { error = failure.Message; }
            Check(error.Length > 0 && rememberedLayers.Count == 0);
            Check(retry == blocked || retry == failed ? error.StartsWith("VR still could not start after disabling OpenXR OBS Mirror.") && error.Contains("Original failure: An OpenXR layer")
                : error == "The OpenXR layer registration changed. Launch VR again to review it.");
        }

        // Remembered choices: per installation, replaced by layer name, and never trusted when invalid.
        var data = client.Artifacts;
        var path = VrApiLayers.PreferencesPath(data);
        if (File.Exists(path)) File.Delete(path);
        Check(VrApiLayers.Remembered(data).Count == 0);
        VrApiLayers.Remember(data, [reshade]); VrApiLayers.Remember(data, [obs, reshade with { ManifestHash = new string('D', 64) }]);
        var remembered = VrApiLayers.Remembered(data);
        Check(remembered.Count == 2 && remembered.Any(obs.SameIdentity) && remembered.Single(layer => layer.LayerName == reshade.LayerName).ManifestHash == new string('D', 64));
        VrApiLayers.Reenable(data, obs.LayerName);
        Check(VrApiLayers.Remembered(data).Single().LayerName == reshade.LayerName);
        foreach (var text in new[] { "{", "[]", "{\"version\":2,\"bypasses\":[]}", "{\"version\":1,\"bypasses\":[{\"layerName\":\"XR_APILAYER_x\",\"disableVariable\":\"PATH\"}]}",
            "{\"version\":1,\"bypasses\":[]}" + new string(' ', 70000) })
        { File.WriteAllText(path, text); Check(VrApiLayers.Remembered(data).Count == 0); }

        // Dialogs: Cancel is the default, nothing is remembered unless chosen.
        using (var dialog = new VrLayerRecoveryForm(new("message", [reshade, obs], "report")))
        {
            var consequences = dialog.Controls.Find("LayerConsequences", true).Single().Text;
            Check(consequences.Contains("ReShade: ReShade OpenXR effects will be unavailable.") && consequences.Contains("OpenXR OBS Mirror: OBS Mirror's OpenXR capture will be unavailable."));
            Check(dialog.AcceptButton == dialog.CancelButton && ((Button)dialog.CancelButton!).Name == "CancelLayerRecovery" && !dialog.RememberChoice);
            Check(((Button)dialog.Controls.Find("RetryWithoutLayers", true).Single()).DialogResult == DialogResult.OK);
            Check(dialog.Controls.Find("LayerFailureDetails", true).Single().Text == "message\r\n\r\nreport");
            var show = dialog.Controls.Find("ShowLayerDetails", true).Single(); Click(show);
            Check(show.Text == "Hide details");
            Click(show);
            // Offscreen review image, rendered unattached like SaveLauncherPreviews; never shown.
            LauncherAppearance.Apply(dialog);
            var content = dialog.Content; dialog.Controls.Remove(content);
            try
            {
                content.Font = dialog.Font; content.Visible = true; content.Size = content.GetPreferredSize(Size.Empty); content.PerformLayout();
                Check(!dialog.Visible && content.Width > 300 && content.Height > 150);
                using var preview = new Bitmap(content.Width, content.Height);
                content.DrawToBitmap(preview, content.ClientRectangle);
                preview.Save(Path.Combine(root, "build", "launcher-vr-layer-recovery.png"));
            }
            finally { dialog.Controls.Add(content); }
        }
        File.Delete(path);
        VrApiLayers.Remember(data, [obs]);
        using (var manage = new VrCompatibilityForm(data))
        {
            var list = (ListBox)manage.Controls.Find("RememberedLayerBypasses", true).Single();
            Check(list.Items.Count == 1 && list.Items[0] is VrLayerBypass item && item.SameIdentity(obs) && list.Items[0].ToString() == "OpenXR OBS Mirror");
            Click(manage.Controls.Find("ReenableLayer", true).Single());
            Check(list.Items.Count == 0 && VrApiLayers.Remembered(data).Count == 0 && manage.Controls.Find("LayerBypassSummary", true).Single().Text.StartsWith("No OpenXR layers"));
        }
        VrApiLayers.Remember(data, [obs]);
        using (var form = new MainForm(client))
        {
            Check(form.Controls.Find("VrCompatibilitySummary", true).Single().Text == "1 remembered OpenXR layer bypass." && form.Controls.Find("ManageVrCompatibility", true).Length == 1);
        }
        File.Delete(path);
    }
}
