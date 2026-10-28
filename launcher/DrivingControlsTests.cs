using System.Globalization;
using System.Text.Json;

namespace GRID2VR;

internal static class DrivingControlsTests
{
    internal static void Run(string fixture)
    {
        DrivingCaptureTests.Run();
        static void Check(bool condition) { if (!condition) throw new Exception("Driving controls regression failed."); }
        static void Reject(Action action) { try { action(); } catch (IOException) { return; } throw new Exception("Invalid driving controls were accepted."); }
        var root = Path.Combine(fixture, "driving-controls");
        var initial = DrivingControls.Load(root); Check(!initial.Enabled && initial.Bindings.Count == 0 && initial.ListenAllDevices && initial.Transmission == "game" && initial.SteeringLinearity == 0 && initial.ClutchShiftThreshold == 50);
        var client = new SessionClient(root);
        Check(client.ExportDrivingControls() is null);
        var preset = new DrivingControls { Enabled = true, Bindings = DrivingControls.XboxPreset() };
        Check(preset.Bindings.All(b => b.DeadZone == 0 && b.Saturation == 1));
        preset.Save(root); var roundtrip = DrivingControls.Load(root);
        Check(roundtrip.Enabled && roundtrip.Bindings.SequenceEqual(preset.Bindings));
        var export = client.ExportDrivingControls()!;
        Check(Path.GetDirectoryName(export) == client.Artifacts);
        Check(System.Xml.Linq.XNode.DeepEquals(System.Xml.Linq.XDocument.Load(export), preset.ToXml()));
        var exportedText = File.ReadAllText(export);
        new DrivingControls { Enabled = false, Bindings = preset.Bindings }.Save(root);
        Check(client.ExportDrivingControls() is null && File.ReadAllText(export) == exportedText);
        new DrivingControls { Enabled = true }.Save(root);
        Check(client.ExportDrivingControls() is null);
        preset.Save(root);
        Check(roundtrip.Bindings.Single(b => b.Action == "Change View").Input == "xinput_button_right_shoulder");
        Check(roundtrip.Bindings.Single(b => b.Action == "Handbrake").Input == "xinput_button_b");
        var originalText = File.ReadAllText(DrivingControls.SettingsPath(root));
        foreach (var mode in new[] { "Normal", "Practice", "Race" })
            foreach (var vr in new[] { false, true })
            {
                var command = client.PrepareLaunchCommand(new Choices { LaunchMode = mode, SkipIntro = true }, vr);
                var index = command.ArgumentList.IndexOf("-DrivingControls");
                Check(index >= 0 && command.ArgumentList[index + 1] == export);
                Check(command.ArgumentList.Contains("-SkipIntro"));
                Check(File.ReadAllText(DrivingControls.SettingsPath(root)) == originalText);
                Check(File.ReadAllText(export) == exportedText);
            }
        new DrivingControls { Enabled = false, Bindings = preset.Bindings }.Save(root);
        Check(!client.PrepareLaunchCommand(new Choices(), false).ArgumentList.Contains("-DrivingControls"));
        Check(File.ReadAllText(export) == exportedText);
        preset.Save(root);
        foreach (var broken in new[]
        {
            preset.Bindings[0] with { Action = "Hand Brake" },
            preset.Bindings[0] with { DeviceType = "keyboard" },
            preset.Bindings[0] with { Input = "xinput_invented" },
            preset.Bindings[0] with { DeviceId = "xinput:4" },
            preset.Bindings[0] with { DeadZone = .8m, Saturation = .8m },
            preset.Bindings[0] with { DeadZone = .1000000000000000000000000001m, Saturation = .1000000000000000000000000002m },
            preset.Bindings[0] with { Calibration = "biDirectionalLower" },
            preset.Bindings[0] with { Device = "bad\nname" },
            preset.Bindings[0] with { DeviceType = "../../keyboard" }
        }) Reject(() => new DrivingControls { Enabled = true, Bindings = [broken] }.Save(root));
        Check(File.ReadAllText(DrivingControls.SettingsPath(root)) == originalText);
        Reject(() => new DrivingControls { Bindings = [preset.Bindings[0], preset.Bindings[0]] }.Validate());
        var sameSteering = new DrivingControls { Enabled = true, Bindings = [preset.Bindings[0], preset.Bindings[0] with { Action = "Steer Right" }] };
        Check(sameSteering.Problems().Count == 1); Reject(() => sameSteering.Save(root));
        var badTrigger = new DrivingControls { Enabled = true, Bindings = [preset.Bindings.Single(b => b.Action == "Brake") with { Calibration = "uniDirNeg" }] };
        Reject(() => badTrigger.Save(root));
        var keyboard = new DrivingBinding { Action = "Accelerate", Device = "Keyboard", DeviceType = "keyboard", DeviceId = "keyboard", Input = DrivingControls.KeyInput(Keys.W)! };
        Check(keyboard.Input == "vk_code_0x57" && DrivingControls.KeyInput(Keys.LWin) is null);
        roundtrip.Assign(keyboard); roundtrip.Validate(); Check(roundtrip.Bindings.Count == preset.Bindings.Count + 1);
        roundtrip.Assign(keyboard with { Input = DrivingControls.KeyInput(Keys.Up)! }); Check(roundtrip.Bindings.Count == preset.Bindings.Count + 1);
        var culture = CultureInfo.CurrentCulture;
        try
        {
            CultureInfo.CurrentCulture = new("de-DE");
            roundtrip.Assign(roundtrip.Bindings[0] with { DeadZone = .123m, Saturation = .876m });
            var xml = roundtrip.ToXml();
            Check(xml.Root!.Name == "DrivingControls" && xml.Descendants("ActionMap").Count() == 2);
            Check(xml.Descendants("Axis").Any(a => (string?)a.Attribute("deadzone") == "0.123" && (string?)a.Attribute("saturation") == "0.876"));
            Check(!xml.ToString().Contains("biDirectional", StringComparison.Ordinal));
        }
        finally { CultureInfo.CurrentCulture = culture; }
        var xbox = new DrivingInput.Device(0, "xinput:0", "Xbox controller 1", "xinput_pad");
        var wheel = new DrivingInput.Device(1, "{01234567-0000-0000-0000-504944564944}", "Test wheel", "{C29B046D-0000-0000-0000-504944564944}");
        // Same action is independent on keyboard, Xbox and each physical wheel.
        // Rebinding one device or applying the Xbox preset must retain the rest.
        var wheelThrottle = new DrivingBinding { Action = "Accelerate", Device = wheel.Name, DeviceId = wheel.Id, DeviceType = wheel.Product,
            Input = "di_y_axis", Calibration = "uniDirNeg", DeadZone = .03m };
        var secondWheelThrottle = wheelThrottle with { DeviceId = "{ABCDEFAB-0000-0000-0000-504944564944}", Device = "Second wheel" };
        var shifter = wheelThrottle with { DeviceId = "{ABCDEFAB-1111-0000-0000-504944564944}", Device = "Separate shifter", Action = "Gear 1", Input = "di_button_0", Calibration = "uniDirPos" };
        var multi = new DrivingControls { Enabled = true, ListenAllDevices = false, Transmission = "hpattern_clutch", SteeringLinearity = -10, ClutchShiftThreshold = 65, Bindings = [keyboard, wheelThrottle, secondWheelThrottle, shifter] };
        multi.ApplyXboxPreset(); multi.Validate();
        Check(multi.Bindings.Contains(keyboard) && multi.Bindings.Contains(wheelThrottle) && multi.Bindings.Contains(secondWheelThrottle) && multi.Bindings.Contains(shifter));
        Check(multi.Bindings.Count(b => b.Action == "Accelerate") == 4);
        multi.Assign(secondWheelThrottle with { Input = "di_z_axis", DeviceId = secondWheelThrottle.DeviceId.ToLowerInvariant() });
        Check(multi.Bindings.Count(b => b.Action == "Accelerate") == 4 && multi.Bindings.Contains(wheelThrottle));
        multi.ApplyXboxPreset(); Check(multi.Bindings.Count == DrivingControls.XboxPreset().Count + 4);
        var maps = multi.ToXml().Descendants("ActionMap").ToArray();
        Check((string?)multi.ToXml().Root!.Attribute("version") == "4" && (string?)multi.ToXml().Root!.Attribute("transmission") == "hpattern_clutch" &&
            (string?)multi.ToXml().Root!.Attribute("steering_linearity") == "-10" && (string?)multi.ToXml().Root!.Attribute("clutch_shift_threshold") == "65" && maps.Length == 5);
        Check(maps.Count(m => (string?)m.Attribute("device_type") == wheel.Product) == 3);
        Check(maps.All(m => !string.IsNullOrEmpty((string?)m.Attribute("device_id"))));
        Reject(() => new DrivingControls { Bindings = [secondWheelThrottle, secondWheelThrottle with { DeviceId = secondWheelThrottle.DeviceId.ToLowerInvariant() }] }.Validate());
        Reject(() => new DrivingControls { Bindings = [wheelThrottle, shifter with { DeviceId = wheelThrottle.DeviceId, DeviceType = "{11111111-0000-0000-0000-504944564944}" }] }.Validate());
        var independentSteering = new DrivingControls { Bindings = [wheelThrottle with { Action = "Steer Left", Input = "di_x_axis", Calibration = "biDirLower" },
            secondWheelThrottle with { Action = "Steer Right", Input = "di_x_axis", Calibration = "biDirLower" }] };
        Check(independentSteering.Problems().Count == 0);
        independentSteering.Assign(independentSteering.Bindings[0] with { Action = "Steer Right" });
        Check(independentSteering.Problems().Count == 1);
        var limit = new DrivingControls { Bindings = Enumerable.Range(1, DrivingControls.MaxDevices).Select(i => wheelThrottle with { DeviceId = $"{{{i:X8}-0000-0000-0000-504944564944}}" }).ToList() };
        limit.Validate(); limit.Assign(keyboard); Reject(limit.Validate);
        Check(DrivingControls.WizardActions.Distinct().Count() == DrivingControls.WizardActions.Length);
        Check(Enumerable.Range(1, 7).Select(i => "Gear " + i).Append("Gear Reverse").All(DrivingControls.WizardActions.Contains));
        // Profiles are portable controls files. Loading changes only the returned
        // editing model; bad saves must leave the previous profile intact.
        var profile = Path.Combine(DrivingControls.ProfilesPath(root), "Rig A.json"); multi.SaveFile(profile);
        var viewRoot = Path.Combine(root, "multi-view"); multi.Save(viewRoot);
        using (var form = new DrivingControlsForm(viewRoot))
        {
            var transmission = (ComboBox)form.Controls.Find("DrivingTransmission", true).Single();
            Check(transmission.SelectedIndex == Array.IndexOf(DrivingControls.Transmissions, "hpattern_clutch") && transmission.Items.Count == 5);
            var linearity = (TrackBar)form.Controls.Find("SteeringLinearitySlider", true).Single();
            Check(linearity.Value == -10 && linearity.Minimum == -10 && linearity.Maximum == 10);
            var threshold = (TrackBar)form.Controls.Find("ClutchShiftThresholdSlider", true).Single();
            var thresholdValue = (Label)form.Controls.Find("ClutchShiftThresholdValue", true).Single();
            Check(threshold.Value == 65 && threshold.Minimum == 10 && threshold.Maximum == 100 && threshold.Enabled && thresholdValue.Text == "65%");
            foreach (var mode in DrivingControls.Transmissions)
            {
                transmission.SelectedIndex = Array.IndexOf(DrivingControls.Transmissions, mode);
                Check(threshold.Enabled == (mode == "hpattern_clutch") && threshold.Value == 65);
            }
            threshold.Value = 50; Check(thresholdValue.Text == "50%");
            const System.Reflection.BindingFlags privateInstance = System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
            typeof(DrivingControlsForm).GetMethod("ReadOptions", privateInstance)!.Invoke(form, null);
            var editing = (DrivingControls)typeof(DrivingControlsForm).GetField("settings", privateInstance)!.GetValue(form)!;
            Check(editing.ClutchShiftThreshold == 50 && editing.Transmission == "hpattern_clutch" && editing.Bindings.SequenceEqual(multi.Bindings));
            var list = form.Controls.OfType<TableLayoutPanel>().Single().Controls.OfType<ListView>().Single();
            var throttleRows = list.Items.Cast<ListViewItem>().Where(r => r.Text == "Accelerate").ToArray();
            Check(!form.Visible && throttleRows.Length == 4 && throttleRows.All(r => r.Tag is DrivingBinding));
            Check(throttleRows.Select(r => ((DrivingBinding)r.Tag!).DeviceId).Distinct(StringComparer.OrdinalIgnoreCase).Count() == 4);
            var layout = form.Controls[0]; form.Controls.Remove(layout);
            try
            {
                layout.Visible = true; layout.Dock = DockStyle.None; layout.Size = new(760, 580); layout.PerformLayout();
                var footer = layout.Controls.Find("DrivingFooter", true).Single();
                Check(footer.Right <= layout.Width && footer.Bottom <= layout.Height);
                Check(linearity.Bottom <= linearity.Parent!.ClientSize.Height && linearity.Right <= linearity.Parent.ClientSize.Width);
                Check(threshold.Bottom <= threshold.Parent!.ClientSize.Height && threshold.Right <= threshold.Parent.ClientSize.Width);
            }
            finally { form.Controls.Add(layout); }
        }
        DrivingControlsLayoutTests.Run(viewRoot);
        using (var wizard = new DrivingBindingForm(null, false)) Check(!wizard.Visible && wizard.ListenAllDevices);
        using (var singleDevice = new DrivingBindingForm("Gear 1", false, listenAllDevices: false)) Check(!singleDevice.ListenAllDevices);
        var precise = wheelThrottle with { DeadZone = .12345m, Saturation = .98765m };
        using (var calibration = new DrivingCalibrationForm(precise))
        {
            Check(!calibration.Visible && calibration.Binding == precise); // Opening must not round an existing profile.
            var deadNumber = (NumericUpDown)calibration.Controls.Find("DeadzonePercent", true).Single();
            var deadSlider = (TrackBar)calibration.Controls.Find("DeadzoneSlider", true).Single();
            var saturationSlider = (TrackBar)calibration.Controls.Find("SaturationSlider", true).Single();
            deadNumber.Value = 12.25m; Check(deadSlider.Value == 12 && calibration.Binding.DeadZone == .1225m);
            deadSlider.Value = 17; saturationSlider.Value = 80;
            Check(deadNumber.Value == 17 && calibration.Binding.DeadZone == .17m && calibration.Binding.Saturation == .8m);
            deadSlider.Value = 0; saturationSlider.Value = 100;
            Check(calibration.Binding.DeadZone == 0 && calibration.Binding.Saturation == 1 && calibration.Binding.Calibration == precise.Calibration);
            var layout = calibration.Controls[0]; calibration.Controls.Remove(layout);
            try
            {
                layout.Visible = true; layout.Dock = DockStyle.None; layout.Size = calibration.ClientSize; layout.PerformLayout();
                foreach (var slider in new[] { deadSlider, saturationSlider })
                    Check(slider.Bottom <= slider.Parent!.ClientSize.Height && slider.Parent.Controls.OfType<NumericUpDown>().Single().Bottom <= slider.Parent.ClientSize.Height);
                var buttons = layout.Controls.OfType<FlowLayoutPanel>().Single();
                Check(buttons.Bottom <= layout.Height && buttons.Controls.Cast<Control>().All(b => b.Bottom <= buttons.ClientSize.Height));
            }
            finally { calibration.Controls.Add(layout); }
        }
        var savedProfile = File.ReadAllBytes(profile); var loadedProfile = DrivingControls.LoadFile(profile);
        Check(loadedProfile.Version == 2 && loadedProfile.Enabled && !loadedProfile.ListenAllDevices && loadedProfile.Transmission == multi.Transmission &&
            loadedProfile.SteeringLinearity == multi.SteeringLinearity && loadedProfile.ClutchShiftThreshold == multi.ClutchShiftThreshold && loadedProfile.Bindings.SequenceEqual(multi.Bindings));
        Check(File.ReadAllText(DrivingControls.SettingsPath(root)) == originalText);
        loadedProfile.Bindings.Clear(); Check(multi.Bindings.Count > 0 && File.ReadAllBytes(profile).SequenceEqual(savedProfile));
        Reject(() => new DrivingControls { Enabled = true, Bindings = [wheelThrottle with { Saturation = 0 }] }.SaveFile(profile));
        Check(File.ReadAllBytes(profile).SequenceEqual(savedProfile) && Directory.GetFiles(Path.GetDirectoryName(profile)!, "*.tmp").Length == 0);
        var legacy = Path.Combine(Path.GetDirectoryName(profile)!, "legacy.json");
        var transmissionProfile = Path.Combine(Path.GetDirectoryName(profile)!, "transmission.json");
        foreach (var transmission in DrivingControls.Transmissions)
        {
            var selected = new DrivingControls { Transmission = transmission, Bindings = [precise] };
            selected.SaveFile(transmissionProfile);
            var loaded = DrivingControls.LoadFile(transmissionProfile);
            Check(loaded.Transmission == transmission && !loaded.Enabled && loaded.Bindings.Single() == precise &&
                (string?)loaded.ToXml().Root!.Attribute("transmission") == transmission);
        }
        foreach (var linearity in Enumerable.Range(-10, 21))
        {
            var selected = new DrivingControls { SteeringLinearity = linearity, Bindings = [precise] };
            selected.SaveFile(transmissionProfile); var loaded = DrivingControls.LoadFile(transmissionProfile);
            Check(loaded.SteeringLinearity == linearity && loaded.Bindings.Single() == precise &&
                (string?)loaded.ToXml().Root!.Attribute("steering_linearity") == linearity.ToString(CultureInfo.InvariantCulture));
        }
        foreach (var linearity in new[] { int.MinValue, -11, 11, int.MaxValue })
        {
            Reject(() => new DrivingControls { SteeringLinearity = linearity }.SaveFile(profile));
            Check(File.ReadAllBytes(profile).SequenceEqual(savedProfile));
        }
        var preciseClutch = precise with { Action = "Clutch", Input = "di_slider_0" };
        foreach (var threshold in Enumerable.Range(10, 91))
        {
            var selected = new DrivingControls { Transmission = "hpattern_clutch", ClutchShiftThreshold = threshold, Bindings = [preciseClutch] };
            selected.SaveFile(transmissionProfile); var loaded = DrivingControls.LoadFile(transmissionProfile);
            Check(loaded.ClutchShiftThreshold == threshold && loaded.Bindings.Single() == preciseClutch &&
                (string?)loaded.ToXml().Root!.Attribute("clutch_shift_threshold") == threshold.ToString(CultureInfo.InvariantCulture));
            var clutchAxis = loaded.ToXml().Descendants("Axis").Single();
            Check((string?)clutchAxis.Attribute("deadzone") == "0.12345" && (string?)clutchAxis.Attribute("saturation") == "0.98765");
        }
        foreach (var threshold in new[] { int.MinValue, 0, 9, 101, int.MaxValue })
        {
            Reject(() => new DrivingControls { ClutchShiftThreshold = threshold }.SaveFile(profile));
            Check(File.ReadAllBytes(profile).SequenceEqual(savedProfile));
        }
        var transmissionRoot = Path.Combine(root, "transmission-only"); var transmissionClient = new SessionClient(transmissionRoot);
        new DrivingControls { Enabled = true, Transmission = "sequential" }.Save(transmissionRoot);
        var transmissionExport = transmissionClient.ExportDrivingControls();
        Check(transmissionExport is not null &&
            (string?)System.Xml.Linq.XDocument.Load(transmissionExport).Root!.Attribute("transmission") == "sequential");
        Check(transmissionClient.PrepareLaunchCommand(new Choices(), false).ArgumentList.Contains("-DrivingControls"));
        new DrivingControls { Enabled = false, Transmission = "sequential" }.Save(transmissionRoot);
        Check(transmissionClient.ExportDrivingControls() is null);
        foreach (var transmission in new[] { "", "manual", "Sequential", "hpattern\n", null })
        {
            Reject(() => new DrivingControls { Transmission = transmission! }.SaveFile(profile));
            Check(File.ReadAllBytes(profile).SequenceEqual(savedProfile));
        }
        File.WriteAllText(legacy, JsonSerializer.Serialize(new { Version = 1, Enabled = true, Bindings = new[] { keyboard, wheelThrottle } }));
        var legacyBytes = File.ReadAllBytes(legacy); var migrated = DrivingControls.LoadFile(legacy);
        Check(migrated.Version == 2 && migrated.ListenAllDevices && migrated.Transmission == "game" && migrated.SteeringLinearity == 0 && migrated.ClutchShiftThreshold == 50 && migrated.Bindings.SequenceEqual(new[] { keyboard, wheelThrottle }));
        Check(File.ReadAllBytes(legacy).SequenceEqual(legacyBytes));
        File.WriteAllBytes(legacy, new byte[] { 0xef, 0xbb, 0xbf }.Concat(legacyBytes).ToArray());
        Check(DrivingControls.LoadFile(legacy).Bindings.SequenceEqual(migrated.Bindings));
        // Guid.TryParseExact itself trims whitespace, whereas the native
        // protocol requires the canonical 38-character braced representation.
        foreach (var padded in new[] { wheelThrottle with { DeviceId = " " + wheelThrottle.DeviceId }, wheelThrottle with { DeviceType = wheelThrottle.DeviceType + " " } })
        {
            var invalid = new DrivingControls { Bindings = [padded] };
            Reject(() => invalid.SaveFile(profile));
            Check(File.ReadAllBytes(profile).SequenceEqual(savedProfile));
            File.WriteAllText(legacy, JsonSerializer.Serialize(invalid)); Reject(() => DrivingControls.LoadFile(legacy));
        }
        File.WriteAllText(legacy, "{\"Version\":2,\"Bindings\":[]}"); Check(DrivingControls.LoadFile(legacy) is { Transmission: "game", SteeringLinearity: 0, ClutchShiftThreshold: 50 });
        foreach (var malformed in new[] { "{", "null", "{\"Version\":3}", "{\"Bindings\":null}", "{\"Transmission\":null}", "{\"Transmission\":\"manual\"}",
            "{\"SteeringLinearity\":null}", "{\"SteeringLinearity\":0.5}", "{\"SteeringLinearity\":-11}", "{\"SteeringLinearity\":\"0\"}",
            "{\"ClutchShiftThreshold\":null}", "{\"ClutchShiftThreshold\":50.5}", "{\"ClutchShiftThreshold\":9}", "{\"ClutchShiftThreshold\":101}", "{\"ClutchShiftThreshold\":\"50\"}" })
        { File.WriteAllText(legacy, malformed); Reject(() => DrivingControls.LoadFile(legacy)); }
        File.WriteAllBytes(legacy, new byte[DrivingControls.MaxFileBytes + 1]); Reject(() => DrivingControls.LoadFile(legacy));
        var rest = DrivingInput.Sample.Empty(); rest.Connected = 1; rest.Axes = 0x3f;
        var pressed = rest.Copy(); pressed.Values[5] = .8f;
        Check(DrivingInput.Detect("Accelerate", xbox, rest, pressed)?.Input == "xinput_right_trigger");
        Check(DrivingInput.Detect("Accelerate", xbox, pressed, rest) is null); // Do not bind trigger release.
        pressed = rest.Copy(); pressed.Values[1] = .9f;
        Check(DrivingInput.Detect("Menu Up", xbox, rest, pressed)?.Calibration == "biDirUpper"); // GRID differs from DiRT.
        pressed = rest.Copy(); pressed.Values[0] = -.8f;
        var steering = DrivingInput.Detect("Steer Left", xbox, rest, pressed);
        Check(steering?.Calibration == "biDirLower" && steering.DeadZone == 0 && steering.Saturation == 1);
        Check(DrivingInput.Detect("Steer Left", xbox, rest, pressed, buttonsOnly: true) is null);
        pressed = rest.Copy(); pressed.Buttons[13] = 1;
        Check(DrivingInput.Detect("Handbrake", xbox, rest, pressed)?.Input == "xinput_button_b");
        Check(DrivingInput.Detect("Handbrake", xbox, rest, pressed, axesOnly: true) is null);
        var pedalRest = rest.Copy(); pedalRest.Values[1] = 1;
        pressed = pedalRest.Copy(); pressed.Values[1] = -.8f;
        var pedal = DrivingInput.Detect("Brake", wheel, pedalRest, pressed);
        Check(pedal?.Input == "di_y_axis" && pedal.Calibration == "uniDirNeg" && pedal.DeadZone == 0 && pedal.Saturation == 1);
        new DrivingControls { Bindings = [pedal!] }.Validate();
        var hat = rest.Copy(); hat.Pov[0] = 9000;
        Check(DrivingInput.Detect("Menu Right", wheel, rest, hat)?.Input == "di_dpad_0_right");
        hat.Pov[0] = 4500; Check(DrivingInput.Detect("Menu Right", wheel, rest, hat) is null);
        var capture = new DrivingCapture(xbox, "Accelerate");
        capture.Update(rest, 0); capture.Update(rest, 600); Check(capture.Ready);
        pressed = rest.Copy(); pressed.Values[5] = .9f;
        capture.Update(pressed, 700); Check(!capture.WaitingForRelease);
        capture.Update(pressed, 800); Check(capture.WaitingForRelease && capture.Completed is null);
        capture.Update(rest, 900); capture.Update(rest, 1060); Check(capture.Completed?.Input == "xinput_right_trigger");
        var disconnected = rest.Copy(); disconnected.Connected = 0;
        capture.Update(disconnected, 1100); Check(!capture.Ready && capture.Completed is null);
        capture.Update(pressed, 1200); capture.Update(pressed, 1800); Check(!capture.WaitingForRelease); // Held after reconnect is not a press.
        var held = rest.Copy(); held.Buttons[12] = 1;
        var buttonCapture = new DrivingCapture(xbox, "Menu Select") { ButtonsOnly = true };
        buttonCapture.Update(held, 0); buttonCapture.Update(held, 600); Check(!buttonCapture.WaitingForRelease);
        buttonCapture.Update(rest, 650); buttonCapture.Update(held, 700); Check(buttonCapture.WaitingForRelease);
        buttonCapture.Update(rest, 750); buttonCapture.Update(rest, 910); Check(buttonCapture.Completed?.Input == "xinput_button_a");
        var hatCapture = new DrivingCapture(wheel, "Menu Right") { ButtonsOnly = true };
        hatCapture.Update(rest, 0); hatCapture.Update(rest, 600); hat.Pov[0] = 9000;
        hatCapture.Update(hat, 700); Check(hatCapture.WaitingForRelease);
        hatCapture.Update(rest, 800); hatCapture.Update(rest, 960); Check(hatCapture.Completed?.Input == "di_dpad_0_right");
        File.WriteAllText(DrivingControls.SettingsPath(root), JsonSerializer.Serialize(new { Version = 3, Enabled = true, Bindings = Array.Empty<object>() }));
        Reject(() => DrivingControls.Load(root));
    }
}
