namespace GRID2VR;

internal static class NewFeaturesTests
{
    internal static void Run(string root, SessionClient fixture)
    {
        static void Check(bool ok, string message) { if (!ok) throw new Exception("New features: " + message); }
        var next = NewFeatures.NextVersion;
        Check(NewFeatures.Shown("0.2.1", false, next), "a development build before the release shows the marks");
        Check(NewFeatures.Shown(next, true, next) && NewFeatures.Shown(next + "+abc", true, next), "the release's Experimental build shows them");
        Check(!NewFeatures.Shown(next, false, next), "a Stable release never shows them");
        Check(!NewFeatures.Shown("0.2.17", true, "0.2.16") && !NewFeatures.Shown("0.3.0", false, "0.2.16"), "the next version drops them");
        Check(!NewFeatures.Shown("garbage", true, next) && !NewFeatures.Shown(null, true, next), "an unreadable version shows nothing");
        Check(NewFeatures.NamesFor(next, false).Count == 0 && NewFeatures.NamesFor(next, true).Count == NewFeatures.Listed.Count, "a Stable build marks nothing; the Experimental build marks every listed feature");
        // Release upkeep: after each release, move NextVersion on and drop the released entries.
        Check(NewFeatures.Listed.Values.All(v => v == next), "only the next release's features are listed");
        Check(Version.TryParse(Updates.CurrentVersion, out var build) && build <= Version.Parse(next),
            $"this build ({Updates.CurrentVersion}) is past {next}; update NewFeatures after a release");

        // Headings, settings rows and other controls each draw their mark in the accent colour.
        using (var stack = new VerticalStack { Width = 640, Visible = true })
        {
            var header = new SectionHeader("VR compatibility"); var plain = new SectionHeader("Updates");
            var option = new CheckBox { Name = "NativeLeftEye", Text = "On", AutoSize = true };
            var row = new SettingRow("Faster stereo (experimental)", option);
            var button = new Button { Name = "DrivingControls", Text = "Driving bindings…", AutoSize = true };
            stack.Controls.AddRange([header, row, button, plain]); stack.PerformLayout();
            var headerWidth = header.Width;
            Check(NewFeatures.Apply(stack, next, false).Count == 0 && !header.IsNew && !row.IsNew && !NewFeatures.IsMarked(button), "a Stable release leaves every control unmarked");
            Check(NewFeatures.Apply(stack, next, true).Count == 3 && header.IsNew && row.IsNew && NewFeatures.IsMarked(button) && !plain.IsNew, "its Experimental build marks the listed controls");
            stack.PerformLayout();
            var markedWidth = header.Width; stack.PerformLayout();
            Check(header.Width >= headerWidth + NewFeatures.Width(header.Font) && header.Width == markedWidth, "a marked heading leaves room for its mark, without growing on each layout");
            Check(stack.ClientSize.Width - button.Right >= NewFeatures.Width(button.Font), "a marked button keeps room to its right");
            var titleWidth = TextRenderer.MeasureText(header.Text, header.Font, Size.Empty, TextFormatFlags.SingleLine | TextFormatFlags.NoPrefix).Width;
            using (var marked = Snapshot(header)) using (var unmarked = Snapshot(plain))
                Check(AccentPixels(marked, new Rectangle(titleWidth, 0, NewFeatures.Width(header.Font) + 30, marked.Height)) > 10 && AccentPixels(unmarked, new Rectangle(Point.Empty, unmarked.Size)) == 0,
                    "the heading's mark is drawn right after its title, and only there");
            using (var caption = Snapshot(row.Controls.Find("NativeLeftEyeLabel", false).Single()))
                Check(AccentPixels(caption, new Rectangle(Point.Empty, caption.Size)) > 10, "the row label's mark is drawn in the accent colour");
            using (var beside = Snapshot(stack))
                Check(AccentPixels(beside, Rectangle.FromLTRB(button.Right, button.Top, beside.Width, button.Bottom)) > 10, "the button's mark is drawn beside it");
            var appearance = LauncherAppearance.Modern;
            foreach (var modern in new[] { false, true })
            {
                LauncherAppearance.Modern = modern; LauncherAppearance.Apply(stack);
                using var preview = Snapshot(stack); preview.Save(Path.Combine(root, "build", modern ? "new-marks-modern.png" : "new-marks-classic.png"));
            }
            LauncherAppearance.Modern = appearance;
            // Simple view moves the button into an action column; the mark follows it.
            using var column = new UniformButtonColumn("TestActions") { Width = 400 };
            column.Controls.Add(button); column.PerformLayout();
            using (var moved = Snapshot(column))
                Check(AccentPixels(moved, Rectangle.FromLTRB(button.Right, button.Top, moved.Width, button.Bottom)) > 10, "a moved control keeps its mark");
        }

        // The launcher lists real controls and marks exactly those in builds that show marks.
        using var form = new MainForm(fixture, new ProfileStore(fixture.Root, () => { }, () => null));
        var missing = NewFeatures.Listed.Keys.Where(name => form.Controls.Find(name, true).Length != 1).ToArray();
        Check(missing.Length == 0, "every listed feature exists once in the launcher (" + string.Join(", ", missing) + ")");
        var shown = NewFeatures.NamesFor(Updates.CurrentVersion, BuildInfo.Channel == "Experimental").Count > 0;
        Control Find(string name) => form.Controls.Find(name, true).Single();
        var compatibility = form.SectionHeaders.Single(h => h.Title == "VR compatibility");
        Check(compatibility.IsNew == shown && form.SectionHeaders.Where(h => h != compatibility).All(h => !h.IsNew), "only the VR compatibility heading is marked");
        Check(((SettingRow)Find("NativeLeftEye").Parent!).IsNew == shown && ((SettingRow)Find("LastVrPerformance").Parent!).IsNew == shown && !((SettingRow)Find("PerformanceDisplay").Parent!).IsNew,
            "new settings rows are marked; older rows aren't");
        Check(NewFeatures.IsMarked(Find("RecommendGraphics")) == shown && NewFeatures.IsMarked(Find("DrivingControls")) == shown && !NewFeatures.IsMarked(Find("PotatoPreset")),
            "new buttons are marked; older ones aren't");
    }

    private static Bitmap Snapshot(Control control)
    {
        var image = new Bitmap(Math.Max(1, control.Width), Math.Max(1, control.Height));
        control.DrawToBitmap(image, new Rectangle(Point.Empty, image.Size));
        return image;
    }

    private static int AccentPixels(Bitmap image, Rectangle area)
    {
        var accent = NewFeatures.Color; var count = 0;
        area.Intersect(new Rectangle(Point.Empty, image.Size));
        for (var x = area.Left; x < area.Right; ++x)
            for (var y = area.Top; y < area.Bottom; ++y)
            {
                var pixel = image.GetPixel(x, y);
                if (Math.Abs(pixel.R - accent.R) + Math.Abs(pixel.G - accent.G) + Math.Abs(pixel.B - accent.B) < 60) ++count;
            }
        return count;
    }
}
