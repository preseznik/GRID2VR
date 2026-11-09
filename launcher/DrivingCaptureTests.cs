namespace GRID2VR;

internal static class DrivingCaptureTests
{
    internal static void Run()
    {
        static void Check(bool condition, string message) { if (!condition) throw new Exception("Driving capture: " + message); }
        var wheel = new DrivingInput.Device(0, "{01234567-0000-0000-0000-504944564944}", "Wheel", "{C262046D-0000-0000-0000-504944564944}");
        var shifter = new DrivingInput.Device(1, "{01234568-0000-0000-0000-504944564944}", "Separate shifter", "{00200EB7-0000-0000-0000-504944564944}");
        var xbox = new DrivingInput.Device(2, "xinput:0", "Xbox controller", "xinput_pad");
        var rest = DrivingInput.Sample.Empty(); rest.Connected = 1; rest.Axes = 3; rest.Values[1] = 1;

        // The reported first attempt: a press before axis settling must not
        // become the new held baseline and require Try again.
        var first = new DrivingCapture(wheel, "Gear Up"); first.Update(rest, 0);
        var pressed = rest.Copy(); pressed.Buttons[5] = 1;
        first.Update(pressed, 40);
        Check(first.WaitingForRelease && first.Detected?.Input == "di_button_5" && !first.AxesReady, "first automatic button press before axis settling");
        first.Update(rest, 80); first.Update(rest, 240);
        Check(first.Completed?.Input == "di_button_5", "first gesture completes on release");

        var held = new DrivingCapture(wheel, "Gear Up"); held.Update(pressed, 0); held.Update(pressed, 700);
        Check(!held.WaitingForRelease, "startup-held button is ignored");
        held.Update(rest, 740); held.Update(pressed, 780);
        Check(held.WaitingForRelease, "held control arms independently after release");

        var another = new DrivingCapture(wheel, "Gear Down"); another.Update(pressed, 0);
        var two = pressed.Copy(); two.Buttons[6] = 1; another.Update(two, 40);
        Check(another.Detected?.Input == "di_button_6", "one held button cannot block another");

        // Other axes can be continuously unsettled. Digital readiness is per
        // control, not a stable snapshot of the entire wheel.
        var noisy = new DrivingCapture(wheel, "Handbrake"); noisy.Update(rest, 0);
        var chatter = rest.Copy();
        for (var time = 40; time <= 560; time += 40) { chatter.Values[0] = time % 80 == 0 ? .8f : -.8f; chatter.Values[1] = -chatter.Values[0]; noisy.Update(chatter, time); }
        chatter.Buttons[127] = 1; noisy.Update(chatter, 580);
        Check(noisy.Detected?.Input == "di_button_127" && noisy.WaitingForRelease, "automatic detects highest button amid axis chatter");

        var axisAndButton = rest.Copy(); axisAndButton.Values[1] = -1; axisAndButton.Buttons[7] = 1;
        Check(DrivingInput.Detect("Handbrake", wheel, rest, axisAndButton)?.Input == "di_button_7", "button wins over incidental pedal travel");
        Check(DrivingInput.Detect("Handbrake", wheel, rest, axisAndButton, axesOnly: true)?.Input == "di_y_axis", "explicit axis filter remains effective");

        var hat = rest.Copy(); hat.Pov[0] = 9000;
        var hatCapture = new DrivingCapture(wheel, "Menu Right"); hatCapture.Update(rest, 0); hatCapture.Update(hat, 40);
        Check(hatCapture.WaitingForRelease && hatCapture.Detected?.Input == "di_dpad_0_right", "first automatic POV press");
        var diagonal = hat.Copy(); diagonal.Pov[0] = 4500; hatCapture.Update(diagonal, 80); hatCapture.Update(diagonal, 300);
        Check(hatCapture.Completed is null, "diagonal is not a release");
        hatCapture.Update(rest, 340); hatCapture.Update(rest, 500);
        Check(hatCapture.Completed?.Input == "di_dpad_0_right", "POV completes at actual neutral");
        var heldHat = new DrivingCapture(wheel, "Menu Up"); heldHat.Update(diagonal, 0); hat.Pov[0] = 0; heldHat.Update(hat, 100);
        Check(!heldHat.WaitingForRelease, "startup diagonal cannot arm a cardinal direction");
        heldHat.Update(rest, 140); heldHat.Update(hat, 180);
        Check(heldHat.Detected?.Input == "di_dpad_0_up", "POV arms after neutral");

        // A G920's H-shifter can report a gear button alongside the D-pad.
        // Menu binding must keep the deliberate hat gesture, not that button.
        foreach (var (direction, angle) in new[] { ("Up", 0u), ("Right", 9000u), ("Down", 18000u), ("Left", 27000u) })
        {
            var combined = rest.Copy(); combined.Pov[0] = angle; combined.Buttons[15] = 1; combined.Values[1] = -1;
            foreach (var buttonsOnly in new[] { false, true })
                Check(DrivingInput.Detect("Menu " + direction, wheel, rest, combined, buttonsOnly: buttonsOnly)?.Input == "di_dpad_0_" + direction.ToLowerInvariant(),
                    "POV wins over gear button and incidental axis: " + direction);
            Check(DrivingInput.Detect("Menu " + direction, wheel, rest, combined, axesOnly: true)?.Input == "di_y_axis", "axis-only filter ignores POV");
        }
        var gearHeld = rest.Copy(); gearHeld.Buttons[15] = 1;
        var buttonFirst = new DrivingCapture(wheel, "Menu Up"); buttonFirst.Update(rest, 0); buttonFirst.Update(gearHeld, 40);
        Check(buttonFirst.Detected?.Input == "di_button_15", "gear button initially waiting for release");
        var gearAndHat = gearHeld.Copy(); gearAndHat.Pov[0] = 4500; buttonFirst.Update(gearAndHat, 80);
        Check(buttonFirst.Detected?.Input == "di_button_15", "diagonal cannot replace pending button");
        gearAndHat.Pov[0] = 0; buttonFirst.Update(gearAndHat, 120);
        Check(buttonFirst.Detected?.Input == "di_dpad_0_up", "POV replaces an already pending gear button");
        buttonFirst.Update(gearHeld, 160); buttonFirst.Update(gearHeld, 320);
        Check(buttonFirst.Completed?.Input == "di_dpad_0_up", "POV release completes while gear button stays held");

        var shiftRest = rest.Copy(); shiftRest.Axes = 0;
        var shiftPressed = shiftRest.Copy(); shiftPressed.Buttons[15] = 1;
        var menuShifter = new DrivingCapture(shifter, "Menu Right"); menuShifter.Update(shiftRest, 0); menuShifter.Update(shiftPressed, 40);
        var menuWheel = new DrivingCapture(wheel, "Menu Right"); menuWheel.Update(rest, 0); hat.Pov[0] = 9000; menuWheel.Update(hat, 80);
        Check(ReferenceEquals(DrivingCapture.ChooseActive([menuShifter, menuWheel], menuShifter), menuWheel), "all-device POV supersedes pending separate shifter button");
        menuWheel.Update(rest, 120); menuWheel.Update(rest, 280);
        Check(menuWheel.Completed?.Input == "di_dpad_0_right" && menuWheel.Completed.DeviceId == wheel.Id, "all-device selection retains wheel POV identity");

        var pedal = new DrivingCapture(wheel, "Brake"); pedal.Update(rest, 0); pedal.Update(rest, 600);
        Check(pedal.AxesReady, "pedal rest settles");
        var down = rest.Copy(); down.Values[1] = -1; pedal.Update(down, 640);
        Check(!pedal.WaitingForRelease, "axis movement needs deliberate hold");
        pedal.Update(down, 740); Check(pedal.WaitingForRelease && pedal.Detected?.Calibration == "uniDirNeg", "pedal direction learned");
        pedal.Update(rest, 780); pedal.Update(rest, 940); Check(pedal.Completed?.Input == "di_y_axis", "pedal release completes");

        var analog = new DrivingCapture(wheel, "Handbrake"); analog.Update(rest, 0); analog.Update(rest, 600); analog.Update(down, 640); analog.Update(down, 740);
        Check(analog.WaitingForRelease && DrivingInput.IsAxis(analog.Detected!), "analog candidate present");
        var overrideButton = down.Copy(); overrideButton.Buttons[9] = 1; analog.Update(overrideButton, 780);
        Check(analog.Detected?.Input == "di_button_9", "automatic digital supersedes stuck analog on same device");

        var wheelCapture = new DrivingCapture(wheel, "Gear 1"); wheelCapture.Update(rest, 0); wheelCapture.Update(rest, 600); wheelCapture.Update(down, 640); wheelCapture.Update(down, 740);
        var gearCapture = new DrivingCapture(shifter, "Gear 1"); var gearRest = rest.Copy(); gearRest.Axes = 0; gearCapture.Update(gearRest, 0);
        var gear = gearRest.Copy(); gear.Buttons[2] = 1; gearCapture.Update(gear, 780);
        Check(ReferenceEquals(DrivingCapture.ChooseActive([wheelCapture, gearCapture], wheelCapture), gearCapture), "separate shifter wins over active wheel axis");
        Check(ReferenceEquals(DrivingCapture.ChooseActive([wheelCapture, gearCapture], gearCapture), gearCapture), "active digital stays selected until released");
        gearCapture.Update(gearRest, 820); gearCapture.Update(gearRest, 980); Check(gearCapture.Completed?.DeviceId == shifter.Id, "separate shifter identity retained");

        // Invalid samples and focus/disconnect reset cannot complete a gesture
        // from the previous capture period.
        var missing = rest.Copy(); missing.Connected = 0; analog.Update(missing, 800);
        Check(!analog.Ready && !analog.WaitingForRelease && analog.Completed is null, "disconnect resets all candidates");
        analog.Update(overrideButton, 840); analog.Update(overrideButton, 1500);
        Check(!analog.WaitingForRelease, "reconnect with held button is not a press");
        held.Reset(); held.Update(pressed, 1000); held.Update(pressed, 1700);
        Check(!held.WaitingForRelease, "focus reset cannot retain a prior press");
        var bad = rest.Copy(); bad.Values[0] = float.NaN;
        var finite = new DrivingCapture(wheel, "Steer Left") { AxesOnly = true }; finite.Update(bad, 0); finite.Update(rest, 40); finite.Update(rest, 640);
        var left = rest.Copy(); left.Values[0] = -.8f; finite.Update(left, 680); finite.Update(left, 780);
        Check(finite.Detected?.Input == "di_x_axis" && finite.WaitingForRelease, "axis recovers from nonfinite first sample");

        var saved = new DrivingBinding { Device = shifter.Name, DeviceId = shifter.Id, DeviceType = shifter.Product, Input = "di_button_2", Action = "Gear 1" };
        var keyboard = new DrivingBinding { Device = "Keyboard", DeviceId = "keyboard", DeviceType = "keyboard", Input = "vk_code_0x41", Action = "Gear 1" };
        Check(DrivingInputMonitor.MissingDevices([saved, saved with { Action = "Gear 2" }, keyboard], [wheel]).Count == 1, "monitor lists missing saved device once, excluding keyboard");
        Check(DrivingInputMonitor.MissingDevices([saved], [shifter with { Id = shifter.Id.ToLowerInvariant(), Product = shifter.Product.ToLowerInvariant() }]).Count == 0, "monitor identity match is case insensitive");
        Check(DrivingInputMonitor.MissingDevices([saved], [shifter with { Product = wheel.Product }]).Count == 1, "compatibility mode mismatch remains visible");
        Check(DrivingInputMonitor.ButtonsText(wheel, chatter) == "127", "monitor shows all 128 DirectInput buttons");
        var xboxSample = rest.Copy(); xboxSample.Buttons[12] = 1;
        Check(DrivingInputMonitor.ButtonsText(xbox, xboxSample) == "button a", "monitor labels Xbox buttons");
        Check(DrivingInputMonitor.HatsText(diagonal).Contains("0: 45°", StringComparison.Ordinal), "monitor retains exact POV diagonal angle");
        Check(DrivingInputMonitor.ButtonsText(wheel, missing) == "Disconnected", "monitor clears stale disconnected state");
        Check(DrivingInput.FeedbackMessage(xbox, 1).StartsWith("Rumbling", StringComparison.Ordinal) &&
              DrivingInput.FeedbackMessage(wheel, 1).Contains("force-feedback shake", StringComparison.Ordinal), "feedback test names rumble and wheel force feedback");
        Check(DrivingInput.FeedbackMessage(wheel, -1).Contains("no force feedback", StringComparison.Ordinal) &&
              DrivingInput.FeedbackMessage(wheel, -2).Contains("Close GRID 2", StringComparison.Ordinal) &&
              DrivingInput.FeedbackMessage(wheel, -3).Contains("could not start", StringComparison.Ordinal) &&
              DrivingInput.FeedbackMessage(wheel, 0).Contains("unavailable", StringComparison.Ordinal), "feedback test explains every refusal");
        Check(DrivingInput.FeedbackMessage(wheel, -3, (6u, unchecked((int)0x80070057))).EndsWith("(CreateEffect returned 0x80070057)", StringComparison.Ordinal) &&
              DrivingInput.FeedbackMessage(wheel, -3).EndsWith("on " + wheel.Name + ".", StringComparison.Ordinal) &&
              DrivingInput.FeedbackMessage(wheel, 1, (6u, 1)).EndsWith("for one second.", StringComparison.Ordinal), "feedback refusal names the Windows call and HRESULT");
        var refused = unchecked((int)0x80070057);
        Check(DrivingInput.FeedbackMessage(wheel, -3, (6u, refused), [refused, refused, refused])
                  .EndsWith("(CreateEffect: sine 0x80070057, sine on X 0x80070057, constant force 0x80070057)", StringComparison.Ordinal) &&
              DrivingInput.FeedbackMessage(wheel, 1, null, [refused, 0]).EndsWith("(using sine on X)", StringComparison.Ordinal) &&
              DrivingInput.FeedbackMessage(wheel, 1, null, [0]).EndsWith("for one second.", StringComparison.Ordinal), "feedback test reports every effect shape it tried");
        Check(DrivingInput.InputDescription(saved) == "button 2", "binding label separates input from device");
    }
}
