using System.Text.Json;

namespace GRID2VR;

internal static class SelfTests
{
    // No window is shown and no game, headset, or worker is started. UI previews render offscreen only.
    public static int Run()
    {
        var root = SessionClient.FindRoot(AppContext.BaseDirectory);
        var fixture = Path.Combine(root, "build", "launcher-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(fixture);
        try
        {
            var client = new SessionClient(fixture);
            ProfileTests.Run(fixture);
            var defaults = new Choices();
            Check(!defaults.Graphics.VrMirrorBorderless);
            Check(SessionClient.CarLabel("z28") == "Chevrolet Camaro Z28");
            Check(SessionClient.CarLabel("unverified_car") == "unverified_car");
            Check(SessionClient.LocationLabel("circuits/brands_hatch") == "Brands Hatch");
            Check(SessionClient.TrackLabel("circuits/brands_hatch/route_2") == "Brands Hatch — Route 2");
            Check(client.Load() == defaults);
            Check(!defaults.SkipIntro && !defaults.DiagnosticLogging);
            Check(!defaults.PerformanceDisplay && defaults.PerformanceFollowView);
            Check(defaults.Graphics.HudDistance == 1m && !defaults.Graphics.HudFollowView && !defaults.Graphics.HudGauges);
            Check(defaults.Graphics.HudLapTime && defaults.Graphics.HudPosition && defaults.Graphics.HudMap && defaults.Graphics.HudProgress && defaults.Graphics.HiddenHudElements == 1);
            Check(defaults.Graphics.VrMsaa == "off");
            Check(defaults.RecenterKey == 121 && defaults.RecenterModifiers == 0 && RecenterShortcut.Label(121, 0) == "F10");
            Directory.CreateDirectory(client.Artifacts);
            File.WriteAllText(Path.Combine(client.Artifacts, "launcher-settings.json"), "{\"Car\":\"pag\",\"Track\":\"p2p/okutama/route_1\",\"Runtime\":\"VDXR\"}");
            Check(client.Load() == (defaults with { ModernInterface = false, Graphics = new() })); // Existing settings retain Classic and gain F10.
            Check(!client.Load().SkipIntro && !client.Load().DiagnosticLogging);
            Check(client.Load().OpponentCars == "same");
            Check(!client.Load().PerformanceDisplay && client.Load().Graphics.HiddenHudElements == 1 && client.Load().Graphics.HudDistance == 1m);
            File.WriteAllText(Path.Combine(client.Artifacts, "launcher-settings.json"), "{\"Car\":\"z28\",\"Track\":\"circuits/brands_hatch/route_2\",\"Runtime\":\"SteamVR\",\"RecenterKey\":82,\"RecenterModifiers\":5}");
            Check(client.Load() == new Choices("z28", "circuits/brands_hatch/route_2", "SteamVR", 82, 5, 1) { ModernInterface = false, Graphics = new() }); // Old choices gain one lap and retain Classic.
            Check(client.Load().Graphics == new GraphicsOptions());
            var graphics = new GraphicsOptions(150, false, "4xmsaa", "medium", "high", "low", "high", 175);
            var selection = new Choices("z28", "circuits/brands_hatch/route_2", "SteamVR", 82, 5, 5) { Graphics = graphics };
            Check(RecenterShortcut.Label(82, 5) == "Ctrl+Shift+R");
            client.Save(selection); Check(client.Load() == selection);
            var legacyGraphics = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(Path.Combine(client.Artifacts, "launcher-settings.json")))!.AsObject();
            legacyGraphics.Remove("ModernInterface"); legacyGraphics["Graphics"]!.AsObject().Remove("HeadsetScale"); legacyGraphics["Graphics"]!.AsObject().Remove("VrMsaa");
            File.WriteAllText(Path.Combine(client.Artifacts, "launcher-settings.json"), legacyGraphics.ToJsonString());
            Check(client.Load() == (selection with { ModernInterface = false, Graphics = graphics with { HeadsetScale = 100 } }));
            Check(client.Load().Graphics.VrMsaa == "off");
            client.Save(selection);
            foreach (var runtime in new[] { "SteamVR", "VDXR" })
                foreach (var mode in new[] { "Normal", "Practice", "Race" })
                    foreach (var borderless in new[] { false, true })
                    {
                        var mirrorOptions = selection with { Runtime = runtime, LaunchMode = mode, Graphics = graphics with { VrMirrorBorderless = borderless } };
                        client.Save(mirrorOptions); Check(client.Load() == mirrorOptions);
                        Check(client.LaunchCommand(mirrorOptions, true).ArgumentList.Contains("-VrMirrorBorderless") == borderless);
                        Check(!client.LaunchCommand(mirrorOptions, false).ArgumentList.Contains("-VrMirrorBorderless"));
                    }
            var legacyMirror = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(Path.Combine(client.Artifacts, "launcher-settings.json")))!.AsObject();
            legacyMirror["Graphics"]!.AsObject().Remove("VrMirrorBorderless");
            File.WriteAllText(Path.Combine(client.Artifacts, "launcher-settings.json"), legacyMirror.ToJsonString());
            Check(!client.Load().Graphics.VrMirrorBorderless);
            client.Save(selection);
            foreach (var value in new[] { defaults with { Car = "pag; Stop-Process steam" }, defaults with { Track = "../foreign" }, defaults with { Runtime = "Auto" },
                defaults with { RecenterKey = 27 }, defaults with { RecenterModifiers = 8 }, defaults with { RecenterKey = 115, RecenterModifiers = 2 },
                defaults with { RecenterKey = 9, RecenterModifiers = 2 }, defaults with { RecenterKey = 46, RecenterModifiers = 3 },
                defaults with { Laps = 0 }, defaults with { Laps = 21 },
                defaults with { Graphics = graphics with { RenderScale = 49 } }, defaults with { Graphics = graphics with { RenderScale = 301 } }, defaults with { Graphics = graphics with { HeadsetScale = 0 } },
                defaults with { Graphics = graphics with { HeadsetScale = 201 } }, defaults with { Graphics = graphics with { HeadsetScale = 99 } }, defaults with { Graphics = graphics with { DesktopMsaa = "16xmsaa" } },
                defaults with { Graphics = graphics with { VrMsaa = "16xmsaa" } }, defaults with { Graphics = graphics with { VrMsaa = "4x" } },
                defaults with { Graphics = graphics with { HudDistance = 0.5m } }, defaults with { Graphics = graphics with { HudDistance = 20.5m } }, defaults with { Graphics = graphics with { HudDistance = 1.25m } },
                defaults with { Graphics = graphics with { TreeDetail = "low" } }, defaults with { Graphics = graphics with { ObjectDetail = "off" } },
                defaults with { Graphics = graphics with { CrowdDetail = "off" } }, defaults with { Graphics = graphics with { TextureFiltering = "custom" } } })
            {
                var rejected = false;
                try { client.Save(value); } catch (ArgumentException) { rejected = true; }
                Check(rejected && client.Load() == selection);
            }
            foreach (var vr in new[] { false, true })
            {
                var command = client.LaunchCommand(selection, vr);
                Check(!command.UseShellExecute && command.CreateNoWindow && !command.RedirectStandardOutput && !command.RedirectStandardError);
                Check(command.ArgumentList.Contains(vr ? "VR" : "Desktop") && command.ArgumentList.Contains("SteamVR"));
                Check(command.ArgumentList.Contains(Path.Combine(fixture, "tools", "Start-UserSession.ps1")));
                Check(!command.ArgumentList.Contains("-Command"));
                Check(command.ArgumentList[command.ArgumentList.IndexOf("-Car") + 1] == selection.Car);
                Check(command.ArgumentList[command.ArgumentList.IndexOf("-Track") + 1] == selection.Track);
                Check(command.ArgumentList[command.ArgumentList.IndexOf("-RecenterKey") + 1] == "82");
                Check(command.ArgumentList[command.ArgumentList.IndexOf("-RecenterModifiers") + 1] == "5");
                Check(command.ArgumentList[command.ArgumentList.IndexOf("-Laps") + 1] == "5");
                foreach (var (flag, value) in new[] { ("-RenderScale", "150"), ("-DesktopVSync", "0"), ("-DesktopMsaa", "4xmsaa"), ("-VrMsaa", "off"),
                    ("-TreeDetail", "medium"), ("-ObjectDetail", "high"), ("-CrowdDetail", "low"), ("-TextureFiltering", "high") })
                    Check(command.ArgumentList[command.ArgumentList.IndexOf(flag) + 1] == value);
                Check(!command.ArgumentList.Contains("-SkipIntro") && !command.ArgumentList.Contains("-DiagnosticLogging"));
                Check(!command.ArgumentList.Contains("-PerformanceDisplay"));
                Check(command.ArgumentList.Contains("-HudDistance") == vr);
                if (vr)
                {
                    Check(command.ArgumentList[command.ArgumentList.IndexOf("-HudDistance") + 1] == "1");
                    Check(command.ArgumentList[command.ArgumentList.IndexOf("-HudFollowView") + 1] == "0");
                    Check(command.ArgumentList[command.ArgumentList.IndexOf("-HudHiddenElements") + 1] == "1");
                }
                foreach (var skip in new[] { false, true })
                    foreach (var logging in new[] { false, true })
                    {
                        var options = selection with { SkipIntro = skip, DiagnosticLogging = logging };
                        client.Save(options); Check(client.Load() == options);
                        var arguments = client.LaunchCommand(options, vr).ArgumentList;
                        Check(arguments.Contains("-SkipIntro") == skip);
                        Check(arguments.Contains("-DiagnosticLogging") == logging);
                        Check(arguments.Where(arg => arg is not ("-SkipIntro" or "-DiagnosticLogging")).SequenceEqual(command.ArgumentList));
                    }
            }
            client.Save(selection);
            // Normal must keep the original startup files even when an existing
            // preference requests skipping; preserve that choice for other modes.
            foreach (var mode in new[] { "Normal", "Practice", "Race" })
                foreach (var vr in new[] { false, true })
                    foreach (var skip in new[] { false, true })
                    {
                        var options = selection with { LaunchMode = mode, SkipIntro = skip };
                        client.Save(options);
                        var saved = File.ReadAllBytes(Path.Combine(client.Artifacts, "launcher-settings.json"));
                        Check(client.LaunchCommand(options, vr).ArgumentList.Contains("-SkipIntro") == skip);
                        Check(File.ReadAllBytes(Path.Combine(client.Artifacts, "launcher-settings.json")).SequenceEqual(saved));
                        Check(client.Load() == options);
                    }
            client.Save(selection);
            for (var mask = 0; mask < 32; ++mask)
            {
                var options = selection with { PerformanceDisplay = true, Graphics = graphics with { HudDistance = 7.5m, HudFollowView = true,
                    HudGauges = (mask & 1) == 0, HudLapTime = (mask & 2) == 0, HudPosition = (mask & 4) == 0, HudMap = (mask & 8) == 0, HudProgress = (mask & 16) == 0 } };
                client.Save(options); Check(client.Load() == options && options.Graphics.HiddenHudElements == mask);
                var oldCulture = System.Globalization.CultureInfo.CurrentCulture;
                try
                {
                    System.Globalization.CultureInfo.CurrentCulture = System.Globalization.CultureInfo.GetCultureInfo("sl-SI");
                    var args = client.LaunchCommand(options, true).ArgumentList;
                    Check(args[args.IndexOf("-HudDistance") + 1] == "7.5" && args[args.IndexOf("-HudFollowView") + 1] == "1");
                    Check(args[args.IndexOf("-HudHiddenElements") + 1] == mask.ToString(System.Globalization.CultureInfo.InvariantCulture));
                    Check(args.Contains("-PerformanceDisplay"));
                    Check(client.LaunchCommand(options, false).ArgumentList.SequenceEqual(client.LaunchCommand(selection, false).ArgumentList));
                }
                finally { System.Globalization.CultureInfo.CurrentCulture = oldCulture; }
            }
            foreach (var distance in Enumerable.Range(2, 39).Select(v => v / 2m)) (graphics with { HudDistance = distance }).Validate();
            using (var slider = new HudDistanceSlider())
            {
                Check(slider.Value == 1m); slider.Value = 20m; Check(slider.Value == 20m);
                slider.Value = 7.5m; Check(slider.Value == 7.5m);
            }
            foreach (var follow in new[] { false, true })
                foreach (var mode in new[] { "Normal", "Practice", "Race" })
                {
                    var options = selection with { PerformanceDisplay = true, PerformanceFollowView = follow, LaunchMode = mode };
                    client.Save(options); Check(client.Load() == options);
                    var args = client.LaunchCommand(options, true).ArgumentList;
                    Check(args[args.IndexOf("-PerformanceFollowView") + 1] == (follow ? "1" : "0"));
                    Check(!client.LaunchCommand(options, false).ArgumentList.Contains("-PerformanceFollowView"));
                    Check(!client.LaunchCommand(options with { PerformanceDisplay = false }, true).ArgumentList.Contains("-PerformanceFollowView"));
                }
            // A real pre-HUD settings shape gains HUD defaults without resetting graphics or selection.
            File.WriteAllText(Path.Combine(client.Artifacts, "launcher-settings.json"), "{\"Car\":\"b1m\",\"Track\":\"cities/miami/route_0\",\"Runtime\":\"VDXR\",\"Graphics\":{\"RenderScale\":300,\"HeadsetScale\":200,\"VrMsaa\":\"4xmsaa\",\"DesktopMsaa\":\"4xmsaa\"},\"ModernInterface\":false,\"PreviewVrFixes\":true,\"SkipIntro\":true,\"DiagnosticLogging\":true}");
            var upgraded = client.Load();
            Check(upgraded.Car == "b1m" && upgraded.Track == "cities/miami/route_0" && upgraded.PreviewVrFixes && upgraded.SkipIntro && upgraded.DiagnosticLogging);
            Check(upgraded.Graphics.RenderScale == 300 && upgraded.Graphics.HeadsetScale == 200 && upgraded.Graphics.VrMsaa == "4xmsaa" && upgraded.Graphics.DesktopMsaa == "4xmsaa");
            Check(upgraded.VrCockpit && upgraded.CockpitDriver == "HeadOnly"); // Existing preferences gain the requested VR default; graphics remain exact.
            Check(upgraded.Graphics.HudDistance == 1m && upgraded.Graphics.HiddenHudElements == 1 && !upgraded.Graphics.HudFollowView && !upgraded.PerformanceDisplay && upgraded.PerformanceFollowView);
            client.Save(selection);
            foreach (var msaa in new[] { "off", "2xmsaa", "4xmsaa", "8xmsaa" })
            {
                var options = selection with { Graphics = graphics with { VrMsaa = msaa } };
                client.Save(options); Check(client.Load() == options && client.Load().Graphics.DesktopMsaa == "4xmsaa");
                foreach (var vr in new[] { false, true })
                {
                    var arguments = client.LaunchCommand(options, vr).ArgumentList;
                    Check(arguments[arguments.IndexOf("-VrMsaa") + 1] == msaa);
                    Check(arguments[arguments.IndexOf("-DesktopMsaa") + 1] == "4xmsaa");
                }
            }
            client.Save(selection);
            var unchangedSettings = File.ReadAllBytes(Path.Combine(client.Artifacts, "launcher-settings.json"));
            Check(client.LoadWindow() == new LauncherWindow());
            var window = new LauncherWindow(1100, 810, true);
            client.SaveWindow(window); Check(client.LoadWindow() == window);
            Check(File.ReadAllBytes(Path.Combine(client.Artifacts, "launcher-settings.json")).SequenceEqual(unchangedSettings));
            Check(window.ClientSizeAtDpi(144, new Size(2000, 1500)) == new Size(1650, 1215));
            Check(window.ClientSizeAtDpi(144, new Size(1200, 850)) == new Size(1200, 850));
            Check(LauncherWindow.FromClientSize(new Size(1650, 1215), 144) == (window with { Maximized = false }));
            client.SaveWindow(new LauncherWindow(-1, 810)); Check(client.LoadWindow() == window);
            File.WriteAllText(Path.Combine(client.Artifacts, "launcher-window.json"), "{invalid"); Check(client.LoadWindow() == new LauncherWindow());
            File.WriteAllText(Path.Combine(client.Artifacts, "launcher-window.json"), "{\"Width\":99999,\"Height\":720}"); Check(client.LoadWindow() == new LauncherWindow());
            Check(client.LogsFolder() == client.Artifacts);
            var olderLogs = Path.Combine(client.Artifacts, "diagnostic-20260101-000000-001");
            var newerLogs = Path.Combine(client.Artifacts, "diagnostic-20260101-000000-002");
            Directory.CreateDirectory(olderLogs); Directory.CreateDirectory(newerLogs);
            Check(client.LogsFolder() == newerLogs);
            var lastPath = Path.Combine(client.Artifacts, "user-session-last.json");
            var pendingPath = Path.Combine(client.Artifacts, "session-pending.json");
            File.WriteAllText(lastPath, JsonSerializer.Serialize(new { run = olderLogs }));
            Check(client.LogsFolder() == olderLogs);
            File.WriteAllText(pendingPath, JsonSerializer.Serialize(new { run = newerLogs }));
            Check(client.LogsFolder() == newerLogs);
            File.WriteAllText(pendingPath, "{\"run\":null}"); Check(client.LogsFolder() == olderLogs);
            File.WriteAllText(pendingPath, JsonSerializer.Serialize(new { run = root })); Check(client.LogsFolder() == olderLogs);
            File.WriteAllText(lastPath, JsonSerializer.Serialize(new { run = client.Artifacts })); Check(client.LogsFolder() == newerLogs);
            File.Delete(pendingPath);
            var workerLog = Path.Combine(client.Artifacts, "user-session-" + Guid.NewGuid().ToString("N") + ".log");
            File.WriteAllText(workerLog, "Linked input retained: OneDrive Documents");
            File.WriteAllText(lastPath, JsonSerializer.Serialize(new { run = (string?)null, state = "failed", log = workerLog }));
            Check(client.LogsFolder() == client.Artifacts); // Current failure takes priority over old runs.
            foreach (var brokenReceipt in new[] { "{invalid", "[]", new string('x', 256 * 1024 + 1) })
            {
                File.WriteAllText(lastPath, brokenReceipt);
                Check(client.LogsFolder() == client.Artifacts);
            }
            File.WriteAllText(lastPath, JsonSerializer.Serialize(new { log = Path.Combine(root, Path.GetFileName(workerLog)) }));
            Check(client.LogsFolder() == newerLogs); // Never open an external log location.
            File.Delete(workerLog);
            File.Delete(pendingPath); File.Delete(lastPath);
            var groupedLogs = Path.Combine(client.Artifacts, "logs", "20261026-212549-730");
            var nextLogs = Path.Combine(client.Artifacts, "logs", "20261026-212549-731");
            Directory.CreateDirectory(groupedLogs); Directory.CreateDirectory(nextLogs);
            Check(client.LogsFolder() == nextLogs);
            File.WriteAllText(lastPath, JsonSerializer.Serialize(new { logDirectory = groupedLogs, run = (string?)null, state = "failed" }));
            Check(client.LogsFolder() == groupedLogs); // Includes preparation failures and the worker log.
            File.WriteAllText(pendingPath, JsonSerializer.Serialize(new { logDirectory = nextLogs, run = (string?)null }));
            Check(client.LogsFolder() == nextLogs);
            File.WriteAllText(pendingPath, JsonSerializer.Serialize(new { logDirectory = Path.Combine(root, "logs", "20261026-212549-731") }));
            Check(client.LogsFolder() == groupedLogs);
            File.WriteAllText(lastPath, JsonSerializer.Serialize(new { run = groupedLogs }));
            Check(client.LogsFolder() == groupedLogs);
            File.Delete(pendingPath); File.Delete(lastPath);
            Check(graphics.SceneLabel == "1920 × 1080" && new GraphicsOptions().SceneLabel == "1280 × 720");
            Check(new GraphicsOptions().HeadsetScale == 100 && new GraphicsOptions().HeadsetLabel == "1280 × 720 per eye");
            foreach (var scale in Enumerable.Range(1, 8).Select(x => x * 25))
                (graphics with { HeadsetScale = scale }).Validate();
            Check((graphics with { HeadsetScale = 200 }).HeadsetLabel == "2560 × 1440 per eye");
            Check(defaults.ModernInterface);
            Check(!defaults.PreviewVrFixes);
            var preview = new Choices("z28", "circuits/brands_hatch/route_2") { PreviewVrFixes = true, Graphics = new(HeadsetScale: 150) };
            client.Save(preview); Check(client.Load() == preview);
            var previewCommand = client.LaunchCommand(preview, true);
            var normalCommand = client.LaunchCommand(preview with { PreviewVrFixes = false }, true);
            Check(previewCommand.ArgumentList.Last() == "-PreviewVrFixes");
            Check(previewCommand.ArgumentList.SkipLast(1).SequenceEqual(normalCommand.ArgumentList));
            Check(!normalCommand.ArgumentList.Contains("-DiagnosticProxy") && !normalCommand.ArgumentList.Contains("-HeadsetVisibilityCandidate"));
            Check(client.LaunchCommand(selection with { PreviewVrFixes = true }, false).ArgumentList.SequenceEqual(client.LaunchCommand(selection, false).ArgumentList));
            var okutamaPreview = preview with { Track = "p2p/okutama/route_1", Laps = 5 };
            var okutamaCommand = client.LaunchCommand(okutamaPreview, true);
            Check(okutamaCommand.ArgumentList.Contains("-PreviewVrFixes"));
            Check(okutamaCommand.ArgumentList[okutamaCommand.ArgumentList.IndexOf("-Track") + 1] == okutamaPreview.Track);
            Check(okutamaCommand.ArgumentList[okutamaCommand.ArgumentList.IndexOf("-Laps") + 1] == "1" && okutamaPreview.Laps == 5);
            client.Save(okutamaPreview); Check(client.Load() == okutamaPreview); client.Save(preview);
            foreach (var choice in new[] { preview with { Car = "pag" }, preview with { Track = "p2p/okutama/route_2" }, preview with { Runtime = "SteamVR" },
                preview with { Laps = 2 }, preview with { Graphics = preview.Graphics with { HeadsetScale = 100 } },
                preview with { Graphics = preview.Graphics with { RenderScale = 150 } }, preview with { Graphics = preview.Graphics with { TreeDetail = "high" } },
                preview with { Graphics = preview.Graphics with { ObjectDetail = "high" } }, preview with { Graphics = preview.Graphics with { CrowdDetail = "high" } },
                preview with { Graphics = preview.Graphics with { TextureFiltering = "high" } },
                preview with { Car = "mp4", Track = "cities/chicago/route_0", Runtime = "SteamVR", Laps = 20,
                    Graphics = new(RenderScale: 75, HeadsetScale: 25, TreeDetail: "medium", ObjectDetail: "medium", CrowdDetail: "low", TextureFiltering: "off") } })
            {
                // Preview selects the fixed DLL; every requested setting is forwarded unchanged.
                var command = client.LaunchCommand(choice, true);
                Check(command.ArgumentList.Last() == "-PreviewVrFixes");
                Check(command.ArgumentList.SkipLast(1).SequenceEqual(client.LaunchCommand(choice with { PreviewVrFixes = false }, true).ArgumentList));
                client.Save(choice); Check(client.Load() == choice);
            }
            foreach (var render in Enumerable.Range(50, 251))
                foreach (var scale in new[] { 25, 50, 75, 100, 125, 150, 175, 200 })
                {
                    var command = client.LaunchCommand(preview with { Graphics = preview.Graphics with { RenderScale = render, HeadsetScale = scale } }, true);
                    Check(command.ArgumentList[command.ArgumentList.IndexOf("-RenderScale") + 1] == render.ToString());
                    Check(!command.ArgumentList.Contains("-HeadsetScale") && command.ArgumentList[command.ArgumentList.IndexOf("-VrRenderScale") + 1] == "100");
                }
            Check(new GraphicsOptions(RenderScale: 300).SceneLabel == "3840 × 2160");
            client.Save(selection with { Graphics = graphics with { RenderScale = 237 } });
            Check(client.Load().Graphics.RenderScale == 237);
            using (var slider = new RenderScaleSlider())
            {
                Check(slider.Value == 100);
                slider.Value = 300; Check(slider.Value == 300);
                slider.Value = 151; Check(slider.AccessibleDescription!.StartsWith("151%"));
                foreach (var modern in new[] { false, true })
                {
                    LauncherAppearance.Modern = modern; LauncherAppearance.Apply(slider);
                    Check(slider.Controls.Find("RenderScaleRecommendation", true).Length == 0);
                    Check(!slider.AccessibleDescription!.Contains("recommended"));
                    slider.Value = 150; slider.Size = new Size(420, 72);
                    using var previewImage = new Bitmap(slider.Width, slider.Height);
                    slider.DrawToBitmap(previewImage, slider.ClientRectangle);
                    previewImage.Save(Path.Combine(root, "build", modern ? "render-scale-modern.png" : "render-scale-classic.png"));
                }
            }
            Check(client.LaunchCommand(preview with { Graphics = preview.Graphics with { DesktopVSync = false, DesktopMsaa = "4xmsaa" } }, true).ArgumentList.Contains("-PreviewVrFixes"));
            client.Save(selection with { ModernInterface = false }); Check(!client.Load().ModernInterface && client.Load().Graphics == graphics);
            client.Save(selection); Check(client.Load().ModernInterface);
            Check(client.LaunchCommand(selection, true).ArgumentList.SequenceEqual(client.LaunchCommand(selection with { ModernInterface = false }, true).ArgumentList));
            foreach (var dark in new[] { false, true })
            {
                var accent = LauncherAppearance.AccentFor(dark);
                Check(accent.R > accent.G * 1.8 && accent.R > accent.B * 1.8);
            }
            using (var icon = Branding.LoadIcon()) Check(icon.Width > 0 && icon.Height > 0);
            using (var logo = Branding.LoadLogo()) Check(logo.Width == logo.Height && logo.GetPixel(0, 0).A <= 2 && logo.GetPixel(logo.Width / 2, logo.Height / 2).A > 200);
            Check((selection with { Graphics = new() }).Car == selection.Car); // Graphics-only reset keeps race choices.
            var pointToPoint = selection with { Track = "p2p/okutama/route_1" };
            var pointCommand = client.LaunchCommand(pointToPoint, true);
            Check(pointCommand.ArgumentList[pointCommand.ArgumentList.IndexOf("-Laps") + 1] == "1");
            Check(pointToPoint.Laps == 5 && SessionClient.EffectiveLaps("cities/chicago/route_0", 5) == 5);
            ContentItem[] cars = [new("z28", SessionClient.CarLabel("z28")), new("mp4", SessionClient.CarLabel("mp4"))];
            Check(LauncherPresentation.FilterCars(cars, "chevrolet").Single().Id == "z28");
            Check(LauncherPresentation.FilterCars(cars, "  MP4  ").Single().Id == "mp4");
            Check(LauncherPresentation.FilterCars(cars, "no such car").Length == 0);
            Check(LauncherPresentation.FilterCars(cars, "").Length == 2);
            using var status = JsonDocument.Parse("{\"session\":null,\"pending\":false,\"status\":{\"failed\":true}}");
            Check(!MainForm.Flag(status.RootElement, "pending"));
            Check(MainForm.Flag(MainForm.Child(status.RootElement, "status"), "failed"));
            Check(MainForm.TextValue(MainForm.Child(status.RootElement, "session"), "mode", "idle") == "idle");
            var requested = LauncherPresentation.RequestedSession(new ContentItem(selection.Car, "Camaro"), new ContentItem(selection.Track, "Brands Hatch"), selection.Runtime, selection.RecenterKey, selection.RecenterModifiers, selection.Laps);
            Check(requested.Contains("Camaro") && requested.Contains("Brands Hatch") && requested.Contains("SteamVR") && requested.Contains("Ctrl+Shift+R"));
            Check(requested.Contains("5 laps"));
            Check(LauncherPresentation.RequestedSession(cars[0], new(pointToPoint.Track, "Okutama"), "VDXR", 121, 0, 5).Contains("point-to-point"));
            Check(LauncherPresentation.Instructions(selection.RecenterKey, selection.RecenterModifiers).Single(t => t.Title == "Controls").Text.StartsWith("Recenter: Ctrl+Shift+R"));
            using var measured = JsonDocument.Parse("{\"requested\":{\"width\":3840,\"height\":2160},\"scene\":{\"width\":3840,\"height\":2160},\"sceneObserved\":true,\"sceneMatchesRequest\":true,\"applied\":true}");
            Check(LauncherPresentation.MeasuredScene(measured.RootElement, true) == "Current session: 3840 × 2160 · matches this session's request.");
            Check(LauncherPresentation.MeasuredScene(measured.RootElement, false).StartsWith("Last session: 3840 × 2160"));
            using var mismatch = JsonDocument.Parse("{\"requested\":{\"width\":3840,\"height\":2160},\"scene\":{\"width\":1280,\"height\":720},\"sceneObserved\":true,\"sceneMatchesRequest\":false,\"applied\":true}");
            Check(LauncherPresentation.MeasuredScene(mismatch.RootElement, true) == "Current session: 1280 × 720 · mismatch: requested 3840 × 2160.");
            using var waiting = JsonDocument.Parse("{\"requested\":{\"width\":3840,\"height\":2160},\"backbuffer\":{\"width\":3840,\"height\":2160},\"sceneObserved\":false,\"applied\":true}");
            Check(LauncherPresentation.MeasuredScene(waiting.RootElement, true) == "Current session: scene not yet observed (requested 3840 × 2160).");
            Check(LauncherPresentation.MeasuredScene(default, false) == "No measured scene from the last session.");
            using var invalidSize = JsonDocument.Parse("{\"scene\":{\"width\":\"3840\",\"height\":2160},\"sceneObserved\":true}");
            Check(LauncherPresentation.MeasuredScene(invalidSize.RootElement, true) == "Current session: scene not yet measured.");
            LaunchModeTests.Run(client);
            LaunchProgressTests.Run(root);
            DesktopWindowTests.Run(fixture);
            CockpitTests.Run(client);
            CockpitTexturePackTests.Run(root, fixture);
            LauncherFeaturesTests.Run(fixture);
            UpdateNotificationTests.Run(root, client);
            InstallationTests.Run(fixture);
            SessionRecoveryTests.Run(fixture);
            DrivingControlsTests.Run(fixture);
            ChaseCameraTests.Run(root, client);
            ResponsiveLayoutTests.Run(root, client);
            VrResolutionTests.Run(client);
            SaveLauncherPreviews(root, client);
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
        finally
        {
            var resolved = Path.GetFullPath(fixture);
            if (Path.GetDirectoryName(resolved) != Path.Combine(root, "build") || !Path.GetFileName(resolved).StartsWith("launcher-test-")) throw new IOException("Fixture escaped build.");
            if (Directory.EnumerateFileSystemEntries(resolved, "*", SearchOption.AllDirectories).Any(p => File.GetAttributes(p).HasFlag(FileAttributes.ReparsePoint))) throw new IOException("Linked fixture retained.");
            Directory.Delete(resolved, true);
        }
    }
    private static void SaveLauncherPreviews(string root, SessionClient fixture)
    {
        foreach (var modern in new[] { false, true })
        {
            using var form = new MainForm(fixture) { ClientSize = new Size(1000, 900) };
            LauncherAppearance.Modern = modern; LauncherAppearance.Apply(form);
            var tabs = (TabControl)form.Controls.Find("LauncherTabs", true).Single();
            foreach (var title in new[] { "Graphics", "Settings", "Advanced" })
            {
                var page = tabs.TabPages.Cast<TabPage>().Single(p => p.Text == title);
                tabs.SelectedTab = page;
                var content = page.Controls[0]; content.Font = form.Font; page.Controls.Remove(content);
                try
                {
                    // An invisible Form suppresses child painting. Render the panel
                    // unattached, like the existing slider fixture, without showing it.
                    content.Visible = true; content.Dock = DockStyle.None; content.Size = new Size(920, 700);
                    content.Height = content.GetPreferredSize(new Size(920, 0)).Height;
                    content.PerformLayout();
                    Check(!form.Visible && content.Width > 200 && content.Height > 100);
                    using var preview = new Bitmap(content.Width, content.Height);
                    content.DrawToBitmap(preview, content.ClientRectangle);
                    Check(!form.Visible);
                    preview.Save(Path.Combine(root, "build", $"launcher-hud-{title.ToLowerInvariant()}-{(modern ? "modern" : "classic")}.png"));
                }
                finally { page.Controls.Add(content); }
            }
        }
    }
    private static void Check(bool condition, [System.Runtime.CompilerServices.CallerArgumentExpression(nameof(condition))] string? expression = null)
    { if (!condition) throw new InvalidOperationException("Launcher check failed: " + expression); }
}
