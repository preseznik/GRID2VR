using System.Reflection;
using System.Text.Json;

namespace GRID2VR;

internal static class SettingsWheelGuardTests
{
    private const BindingFlags Flags = BindingFlags.Instance | BindingFlags.NonPublic;
    private static void Check(bool value, [System.Runtime.CompilerServices.CallerArgumentExpression(nameof(value))] string? text = null)
    { if (!value) throw new InvalidOperationException("Settings wheel protection regression: " + text); }

    // Message values are passed directly to managed filters. No SendInput,
    // SendMessage, PostMessage, focus changes, visible windows or real input.
    private static Message Wheel(Control target, int delta, bool horizontal = false) => Message.Create(target.Handle,
        horizontal ? SettingsWheelGuard.MouseHWheel : SettingsWheelGuard.MouseWheel,
        new IntPtr(unchecked((int)((ushort)(short)delta << 16))), IntPtr.Zero);

    private static bool Filter(SettingsWheelGuard guard, Control target, int delta, bool horizontal = false)
    { var message = Wheel(target, delta, horizontal); return guard.PreFilterMessage(ref message); }
    private static int Y(ScrollableControl page) => -page.AutoScrollPosition.Y;
    private static int X(ScrollableControl page) => -page.AutoScrollPosition.X;
    private static Panel Page(Size size, Size content) => new() { AutoScroll = true, Size = size, AutoScrollMinSize = content };

    private static void CreateOffscreenPage(ScrollableControl page)
    {
        // AutoScrollPosition setters require Control.Created, not just a HWND.
        // Create only this owned hidden fixture subtree without showing its form.
        typeof(Control).GetMethod("CreateControl", Flags, null, [typeof(bool)], null)!.Invoke(page, [true]);
        page.PerformLayout(); Check(page.Created);
    }

    internal static void Run(string root, SessionClient fixture)
    {
        TestScrolling(); TestNestedScrolling(); TestLifetime();
        var evidence = new List<object>();
        foreach (var modern in new[] { false, true })
        {
            using var form = new MainForm(fixture, new ProfileStore(fixture.Root, () => { }, () => null), approveAdvancedView: () => true);
            LauncherAppearance.Modern = modern; LauncherAppearance.Apply(form);
            LaunchModeTests.RunForm(form);
            var picker = (LauncherViewPicker)form.Controls.Find("LauncherView", true).Single();
            foreach (var advanced in new[] { true, false, true })
            {
                picker.Advanced = advanced;
                var snapshot = Current(form);
                var count = TestEditors(form);
                Check(Current(form) == snapshot && count > 8 && !form.Visible);
                evidence.Add(new { modern, advanced, protectedEditors = count, choicesUnchanged = true });
            }
            using var driving = new DrivingControlsForm(fixture.Root);
            Check(TestEditors(driving) >= 3 && !driving.Visible);
            using var calibration = new DrivingCalibrationForm(new DrivingBinding
            { Action = "Clutch", Device = "Fixture pad", DeviceType = "xinput_pad", DeviceId = "xinput:0", Input = "xinput_left_trigger" });
            Check(TestEditors(calibration) == 5 && !calibration.Visible);
            var number = (NumericUpDown)calibration.Controls.Find("DeadzonePercent", true).Single();
            var edit = number.Controls.OfType<TextBoxBase>().Single();
            var before = calibration.Binding;
            using var guard = new SettingsWheelGuard(calibration);
            Check(Filter(guard, edit, -120) && Filter(guard, edit, 120) && calibration.Binding == before);
        }
        File.WriteAllText(Path.Combine(root, "build", "launcher-wheel-guard.json"), JsonSerializer.Serialize(new
        {
            windowsShown = 0, gameLaunched = false, inputSynthesized = false,
            highResolutionDeltas = true, systemLinesAndPageMode = true, nestedPageBoundaries = true,
            numericChildHandles = true, openDropdownException = true, keyboardAndMouseButtonsUnfiltered = true,
            filterDisposal = true, surfaces = evidence
        }, new JsonSerializerOptions { WriteIndented = true }));
    }

    private static Choices Current(MainForm form) => (Choices)typeof(MainForm).GetMethod("Current", Flags)!.Invoke(form, null)!;

    private static int TestEditors(LauncherForm form)
    {
        var guard = (SettingsWheelGuard)typeof(LauncherForm).GetField("wheelGuard", Flags)!.GetValue(form)!;
        var editors = Descendants(form).Where(c => c is TrackBar or ComboBox or NumericUpDown).ToArray();
        foreach (var editor in editors)
        {
            object? Value() => editor switch { TrackBar track => track.Value, ComboBox combo => combo.SelectedIndex, NumericUpDown number => number.Value, _ => null };
            var before = Value(); var changes = 0;
            EventHandler changed = (_, _) => ++changes;
            if (editor is TrackBar track) track.ValueChanged += changed;
            if (editor is ComboBox combo) combo.SelectedIndexChanged += changed;
            if (editor is NumericUpDown number) number.ValueChanged += changed;
            try
            {
                foreach (var delta in new[] { -120, 120, -30, -30, -30, -30, 240 })
                { Check(Filter(guard, editor, delta)); Check(Filter(guard, editor, delta, horizontal: true)); }
                Check(Equals(before, Value()) && changes == 0);
            }
            finally
            {
                if (editor is TrackBar t) t.ValueChanged -= changed;
                if (editor is ComboBox c) c.SelectedIndexChanged -= changed;
                if (editor is NumericUpDown n) n.ValueChanged -= changed;
            }
        }
        return editors.Length;
    }

    private static void TestScrolling()
    {
        using var form = new LauncherForm();
        using var page = Page(new(280, 160), new(1000, 1600)); form.Controls.Add(page);
        var track = new TrackBar { Minimum = 0, Maximum = 100, Value = 40, Size = new(180, 35) };
        var combo = new ComboBox { Top = 40, Width = 180, DropDownStyle = ComboBoxStyle.DropDownList };
        combo.Items.AddRange(["Low", "Medium", "High"]); combo.SelectedIndex = 1;
        var number = new NumericUpDown { Top = 75, Width = 100, Value = 40 };
        var text = new TextBox { Top = 110, Width = 100, Text = "Untouched" };
        page.Controls.AddRange([track, combo, number, text]);
        _ = form.Handle; CreateOffscreenPage(page);
        Check(page.VerticalScroll.Visible && page.HorizontalScroll.Visible);
        var lines = 3; var open = false;
        using var guard = new SettingsWheelGuard(form, () => lines, _ => open);
        var changes = 0; track.ValueChanged += (_, _) => ++changes; combo.SelectedIndexChanged += (_, _) => ++changes; number.ValueChanged += (_, _) => ++changes;
        page.AutoScrollPosition = new(40, 100);
        var y = Y(page); var x = X(page); var small = page.VerticalScroll.SmallChange;
        var handled = Filter(guard, track, -120);
        if (!handled || Y(page) != y + 3 * small || X(page) != x)
            throw new InvalidOperationException($"Wheel fixture: handled={handled}, y={y}->{Y(page)}, x={x}->{X(page)}, small={small}, scrollValue={page.VerticalScroll.Value}, max={page.VerticalScroll.Maximum}, page={page.VerticalScroll.LargeChange}, handle={page.IsHandleCreated}");
        Check(Filter(guard, combo, 120) && Y(page) == y && combo.SelectedIndex == 1);
        Check(Filter(guard, number, -120) && Y(page) == y + 3 * small && number.Value == 40);
        var child = number.Controls.OfType<TextBoxBase>().Single();
        y = Y(page); Check(Filter(guard, child, -120) && Y(page) == y + 3 * small);
        // Fractional input is accumulated per page, not lost when the pointer crosses editors.
        y = Y(page);
        Check(Filter(guard, track, -30) && Y(page) == y);
        Check(Filter(guard, combo, -30) && Y(page) == y);
        Check(Filter(guard, child, -30) && Y(page) == y);
        Check(Filter(guard, number, -30) && Y(page) == y + 3 * small);
        y = Y(page); Check(Filter(guard, track, -60) && Filter(guard, combo, 60) && Y(page) == y);
        Check(Filter(guard, track, -120) && Y(page) == y + 3 * small);
        x = X(page); y = Y(page);
        Check(Filter(guard, track, 120, horizontal: true) && X(page) == x + 3 * page.HorizontalScroll.SmallChange && Y(page) == y);
        lines = -1; page.AutoScrollPosition = new(40, 100); y = Y(page);
        Check(Filter(guard, track, -120) && Y(page) == y + page.VerticalScroll.LargeChange);
        lines = 0; y = Y(page); Check(Filter(guard, track, -240) && Y(page) == y);
        lines = 3; Check(Filter(guard, track, -120));
        Check(Filter(guard, track, short.MinValue) && Y(page) == page.VerticalScroll.Maximum - page.VerticalScroll.LargeChange + 1);
        Check(Filter(guard, track, 0) && track.Value == 40 && combo.SelectedIndex == 1 && number.Value == 40 && changes == 0);
        // The open-list branch is tested through a policy seam; opening a native
        // dropdown here would display a popup, which offscreen tests must not do.
        open = true; y = Y(page); Check(!Filter(guard, combo, -120) && Y(page) == y);
        open = false; Check(Filter(guard, combo, -120));
        Check(!Filter(guard, text, -120));
        foreach (var messageId in new[] { 0x0100, 0x0101, 0x0200, 0x0201, 0x0202 })
        { var message = Message.Create(track.Handle, messageId, IntPtr.Zero, IntPtr.Zero); Check(!guard.PreFilterMessage(ref message)); }
        track.Value = 41; combo.SelectedIndex = 2; number.Value = 41;
        Check(changes == 3); // Ordinary value/selection paths and their events still work.
        using var foreign = new Form(); var other = new TrackBar(); foreign.Controls.Add(other);
        Check(!Filter(guard, other, -120));
        var missing = Message.Create(IntPtr.Zero, SettingsWheelGuard.MouseWheel, IntPtr.Zero, IntPtr.Zero);
        Check(!guard.PreFilterMessage(ref missing) && !form.Visible && !foreign.Visible);
    }

    private static void TestNestedScrolling()
    {
        using var form = new LauncherForm();
        using var outer = Page(new(300, 180), new(0, 2200));
        using var inner = Page(new(220, 100), new(0, 800));
        form.Controls.Add(outer); outer.Controls.Add(inner);
        var track = new TrackBar { Maximum = 100, Value = 50 }; inner.Controls.Add(track);
        _ = form.Handle; CreateOffscreenPage(outer); CreateOffscreenPage(inner);
        using var guard = new SettingsWheelGuard(form, () => 3);
        Check(Filter(guard, track, -120) && Y(inner) > 0 && Y(outer) == 0);
        inner.AutoScrollPosition = new(0, 10000);
        var atEnd = Y(inner); Check(Filter(guard, track, -120) && Y(inner) == atEnd && Y(outer) > 0);
        inner.AutoScrollPosition = Point.Empty; var outerY = Y(outer);
        Check(Filter(guard, track, 120) && Y(inner) == 0 && Y(outer) < outerY && track.Value == 50);
        var standalone = new NumericUpDown { Value = 40 }; form.Controls.Add(standalone);
        Check(Filter(guard, standalone, -120) && standalone.Value == 40 && !form.Visible);
    }

    private static void TestLifetime()
    {
        var form = new LauncherForm(); var track = new TrackBar { Maximum = 100, Value = 50 }; form.Controls.Add(track);
        var installed = (SettingsWheelGuard)typeof(LauncherForm).GetField("wheelGuard", Flags)!.GetValue(form)!;
        try
        {
            var message = Wheel(track, -120);
            Check(Application.FilterMessage(ref message) && track.Value == 50);
            installed.Dispose();
            message = Wheel(track, -120); Check(!Application.FilterMessage(ref message) && installed.IsDisposed);
        }
        finally { form.Dispose(); }
        using var another = new LauncherForm();
        var automatic = (SettingsWheelGuard)typeof(LauncherForm).GetField("wheelGuard", Flags)!.GetValue(another)!;
        another.Dispose(); Check(automatic.IsDisposed);
    }

    private static IEnumerable<Control> Descendants(Control parent)
    {
        foreach (Control child in parent.Controls)
        { yield return child; foreach (var nested in Descendants(child)) yield return nested; }
    }
}
