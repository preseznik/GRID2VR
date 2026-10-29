namespace GRID2VR;

internal static class SectionCollapseTests
{
    internal static void Run(SessionClient fixture)
    {
        static void Check(bool ok, string message) { if (!ok) throw new Exception("Section collapse: " + message); }
        // A heading hides only the rows up to the next heading, keeps Simple-view
        // exclusions on expand, and restores the exact original layout.
        using (var stack = new VerticalStack { Width = 400, Visible = true })
        {
            var first = new SectionHeader("First"); var second = new SectionHeader("Second");
            var a = new Label { Text = "a", AutoSize = true }; var b = new CheckBox { Text = "b" }; var c = new Label { Text = "c", AutoSize = true };
            stack.Controls.AddRange([first, a, b, second, c]); stack.PerformLayout();
            var height = stack.Height; var secondTop = second.Top; var bBounds = b.Bounds;
            Check(!first.Collapsed && first.Text == "▾ First" && first.AccessibleName == "First" && a.Visible && b.Visible && first.TabStop, "sections start open and focusable");
            first.PerformClick();
            Check(first.Collapsed && first.Text == "▸ First", "a click collapses the heading");
            // Auto-sized labels keep their own size when parked; hiding is what matters.
            Check(!a.Visible && !b.Visible && a.Location == Point.Empty && b.Bounds.IsEmpty, "collapsing hides only its own rows");
            Check(second.Visible && c.Visible && second.Top < secondTop && stack.Height < height, "the next section moves up");
            VerticalStack.SetShown(b, false); first.Collapsed = false;
            Check(a.Visible && !b.Visible && b.Bounds.IsEmpty, "expanding keeps rows that Simple view hides");
            VerticalStack.SetShown(b, true);
            Check(b.Visible && b.Bounds == bBounds && second.Top == secondTop && stack.Height == height, "the original layout returns");
            first.Collapsed = true; VerticalStack.SetShown(a, true);
            Check(!a.Visible && a.Location == Point.Empty, "showing a row cannot reveal a collapsed section");
            stack.Controls.Remove(a);
            Check(a.Visible, "a row moved out of a collapsed section becomes visible again");
            first.Collapsed = false; second.Collapsed = true;
            Check(b.Visible && !c.Visible, "sections collapse independently");
        }
        // The launcher's headings, shared titles and remembered state.
        using var form = new MainForm(fixture);
        var headers = form.SectionHeaders;
        var titles = headers.Select(h => h.Title).ToHashSet();
        foreach (var expected in new[] { "Graphics preset", "VR rendering", "VR HUD", "Event selection", "Race options", "Driving controls", "Updates" })
            Check(titles.Contains(expected), "missing heading " + expected);
        Check(headers.All(h => !h.Collapsed && h.Tag is "section") && form.CollapsedSections is null, "all sections start open");
        var hud = headers.First(h => h.Title == "VR HUD"); var preset = headers.First(h => h.Title == "Graphics preset");
        hud.Collapsed = true; Check(form.CollapsedSections == "VR HUD", "collapse is remembered");
        preset.Collapsed = true; Check(form.CollapsedSections == "Graphics preset|VR HUD", "state lists every collapsed title");
        hud.Collapsed = false; Check(form.CollapsedSections == "Graphics preset", "expanding forgets the title");
        foreach (var group in headers.GroupBy(h => h.Title).Where(g => g.Count() > 1))
        {
            group.First().Collapsed = true;
            Check(group.All(h => h.Collapsed), "same-title headings move together: " + group.Key);
            group.First().Collapsed = false;
        }
        form.ApplyCollapsedSections("VR HUD|No such section");
        Check(hud.Collapsed && !preset.Collapsed && form.CollapsedSections == "VR HUD", "saved state is restored and unknown titles are ignored");
    }
}
