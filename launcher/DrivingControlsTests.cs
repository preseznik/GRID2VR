using System.Globalization;
using System.Text.Json;

namespace GRID2VR;

internal static class DrivingControlsTests
{
    internal static void Run(string fixture)
    {
        static void Check(bool condition) { if (!condition) throw new Exception("Driving controls regression failed."); }
        static void Reject(Action action) { try { action(); } catch (IOException) { return; } throw new Exception("Invalid driving controls were accepted."); }
        var root = Path.Combine(fixture, "driving-controls");
        var initial = DrivingControls.Load(root); Check(!initial.Enabled && initial.Bindings.Count == 0);
        var client = new SessionClient(root);
        Check(client.ExportDrivingControls() is null);
        var preset = new DrivingControls { Enabled = true, Bindings = DrivingControls.XboxPreset() };
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
            var xml = roundtrip.ToXml();
            Check(xml.Root!.Name == "DrivingControls" && xml.Descendants("ActionMap").Count() == 2);
            Check(xml.Descendants("Axis").Any(a => (string?)a.Attribute("deadzone") == "0.2"));
            Check(!xml.ToString().Contains("biDirectional", StringComparison.Ordinal));
        }
        finally { CultureInfo.CurrentCulture = culture; }
        var xbox = new DrivingInput.Device(0, "xinput:0", "Xbox controller 1", "xinput_pad");
        var wheel = new DrivingInput.Device(1, "{01234567-0000-0000-0000-504944564944}", "Test wheel", "{C29B046D-0000-0000-0000-504944564944}");
        var rest = DrivingInput.Sample.Empty(); rest.Connected = 1; rest.Axes = 0x3f;
        var pressed = rest.Copy(); pressed.Values[5] = .8f;
        Check(DrivingInput.Detect("Accelerate", xbox, rest, pressed)?.Input == "xinput_right_trigger");
        Check(DrivingInput.Detect("Accelerate", xbox, pressed, rest) is null); // Do not bind trigger release.
        pressed = rest.Copy(); pressed.Values[1] = .9f;
        Check(DrivingInput.Detect("Menu Up", xbox, rest, pressed)?.Calibration == "biDirUpper"); // GRID differs from DiRT.
        pressed = rest.Copy(); pressed.Values[0] = -.8f;
        Check(DrivingInput.Detect("Steer Left", xbox, rest, pressed)?.Calibration == "biDirLower");
        Check(DrivingInput.Detect("Steer Left", xbox, rest, pressed, buttonsOnly: true) is null);
        pressed = rest.Copy(); pressed.Buttons[13] = 1;
        Check(DrivingInput.Detect("Handbrake", xbox, rest, pressed)?.Input == "xinput_button_b");
        Check(DrivingInput.Detect("Handbrake", xbox, rest, pressed, axesOnly: true) is null);
        var pedalRest = rest.Copy(); pedalRest.Values[1] = 1;
        pressed = pedalRest.Copy(); pressed.Values[1] = -.8f;
        var pedal = DrivingInput.Detect("Brake", wheel, pedalRest, pressed);
        Check(pedal?.Input == "di_y_axis" && pedal.Calibration == "uniDirNeg");
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
        File.WriteAllText(DrivingControls.SettingsPath(root), JsonSerializer.Serialize(new { Version = 2, Enabled = true, Bindings = Array.Empty<object>() }));
        Reject(() => DrivingControls.Load(root));
    }
}
