using System.Text.Json;

namespace GRID2VR;

internal static class SelectableStatusTests
{
    private static void Check(bool value, [System.Runtime.CompilerServices.CallerArgumentExpression(nameof(value))] string? text = null)
    { if (!value) throw new InvalidOperationException("Selectable status regression: " + text); }

    internal static void Run(string root, SessionClient fixture)
    {
        const string message = @"Game exception recorded; inspect engine-faults.txt and engine-fault-context.txt in: C:\Users\Driver & tester\AppData\Local\GRID2VR\11A495FA65E8C6A33CD17E7E\logs\20261029-115519-843";
        var evidence = new List<object>();
        foreach (var modern in new[] { false, true })
        {
            using var form = new MainForm(fixture, new ProfileStore(fixture.Root, () => { }, () => null));
            LauncherAppearance.Modern = modern; LauncherAppearance.Apply(form);
            foreach (var name in new[] { "SessionStatus", "SessionNotice" })
            {
                var status = (SelectableStatusText)form.Controls.Find(name, true).Single();
                Check(status.ReadOnly && status.Multiline && status.WordWrap && status.ShortcutsEnabled && status.TabStop);
                Check(status.BorderStyle == BorderStyle.None && !status.HideSelection);
                status.Text = message;
                _ = status.Handle; // Owned hidden native edit control; never a live launcher/game window.
                foreach (var width in new[] { 460, 820, 1320, 460 })
                {
                    status.FitWidth(width);
                    status.SelectAll(); Check(status.SelectedText == message);
                    var path = message.IndexOf(@"C:\", StringComparison.Ordinal);
                    status.Select(path, message.Length - path);
                    var selected = status.SelectedText;
                    status.Text = message; status.FitWidth(width);
                    Check(status.SelectedText == selected && status.SelectionStart == path);
                    Check(status.Width <= width && status.Height > 0 && status.Height <= status.Font.Height * 6 + 4);
                    Check(status.BackColor == status.Parent!.BackColor);
                    evidence.Add(new { modern, name, width, height = status.Height, fullTextSelectable = true, selectionPreserved = true });
                }
                var longText = string.Join(Environment.NewLine, Enumerable.Repeat(message, 30));
                status.Text = longText; status.FitWidth(460); status.SelectAll(); Check(status.SelectedText == longText);
                Check(status.ScrollBars == ScrollBars.Vertical && status.Height <= status.Font.Height * 6 + 4);
                status.Text = "Ready."; status.FitWidth(1320); status.SelectAll();
                Check(status.SelectedText == "Ready." && status.ScrollBars == ScrollBars.None);
                Check(status.Height <= status.Font.Height + 4);
                status.Text = ""; Check(status.TextLength == 0 && status.Height == 0);
            }
            using (var preview = new SelectableStatusText { Font = form.Font, Text = message })
            {
                LauncherAppearance.Apply(preview); preview.FitWidth(900); _ = preview.Handle;
                using var image = new Bitmap(preview.Width, preview.Height);
                preview.DrawToBitmap(image, preview.ClientRectangle);
                image.Save(Path.Combine(root, "build", $"launcher-selectable-status-{(modern ? "modern" : "classic")}.png"));
            }
            Check(!form.Visible);
        }
        foreach (var dpi in new[] { 96, 144, 192 })
        {
            using var font = new Font("Segoe UI", 10f * dpi / 96f);
            using var status = new SelectableStatusText { Font = font, Text = message };
            _ = status.Handle;
            status.FitWidth(460 * dpi / 96); status.SelectAll();
            Check(status.SelectedText == message && status.Height > 0 && status.Height <= font.Height * 6 + 4);
            evidence.Add(new { dpi, width = status.Width, height = status.Height, fullTextSelectable = true });
        }
        File.WriteAllText(Path.Combine(root, "build", "launcher-selectable-status.json"), JsonSerializer.Serialize(new
        { gameLaunched = false, windowsShown = 0, clipboardTouched = false, checks = evidence }, new JsonSerializerOptions { WriteIndented = true }));
    }
}
