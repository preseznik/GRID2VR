namespace GRID2VR;

internal sealed record PreparationProgress(int Percent, string Message)
{
    internal static PreparationProgress? Parse(string line)
    {
        var parts = line.Split('|', 3);
        if (parts.Length != 3 || parts[0] != "GRID2VR_PROGRESS" ||
            !int.TryParse(parts[1], out var percent) || percent is < 15 or > 99 ||
            string.IsNullOrWhiteSpace(parts[2])) return null;
        var car = System.Text.RegularExpressions.Regex.Match(parts[2], @"^(Preparing cockpit camera \d+ of \d+: )([a-z0-9]{3})$");
        return new(percent, car.Success ? car.Groups[1].Value + CarNames.Label(car.Groups[2].Value) : parts[2]);
    }
}

internal sealed class PreparationForm : Form
{
    private readonly Label step = new() { AutoSize = false, Dock = DockStyle.Top, Height = 55, Text = "Checking installation…" };
    private readonly ProgressBar progress = new() { Dock = DockStyle.Top, Height = 22, Style = ProgressBarStyle.Continuous };
    private readonly Label count = new() { Dock = DockStyle.Top, Height = 28, TextAlign = ContentAlignment.MiddleRight, Text = "0%" };
    internal Panel Content { get; } = new() { Dock = DockStyle.Fill, Padding = new Padding(20) };
    internal Exception? Failure { get; private set; }

    internal void DisplayProgress(PreparationProgress update)
    {
        if (IsDisposed) return;
        progress.Value = Math.Max(progress.Value, Math.Clamp(update.Percent, 0, 100));
        count.Text = $"{progress.Value}%"; step.Text = update.Message;
    }

    internal PreparationForm(string root)
    {
        Text = "GRID2VR — Preparing local game assets";
        ClientSize = new Size(540, 172);
        StartPosition = FormStartPosition.CenterScreen; FormBorderStyle = FormBorderStyle.FixedDialog;
        ControlBox = false; AutoScaleMode = AutoScaleMode.Dpi;
        Content.Controls.Add(new Label { Dock = DockStyle.Bottom, Height = 30, Text = "First-time preparation can take a few minutes." });
        Content.Controls.Add(count); Content.Controls.Add(progress); Content.Controls.Add(step); Controls.Add(Content);
        Shown += async (_, _) =>
        {
            try
            {
                IProgress<PreparationProgress> updates = new Progress<PreparationProgress>(DisplayProgress);
                if (Installation.HasPending(root))
                {
                    await Task.Run(() => AppPaths.VerifyPackage(root, (done, total) =>
                        updates.Report(new(100 * done / total, $"Checking installed files ({done:N0} of {total:N0})…"))));
                    await Task.Run(() => SessionRecovery.TryClearUnstarted(root));
                }
                if (!Installation.HasPending(root)) await Installation.Setup(root, updates);
            }
            catch (Exception ex) { Failure = ex; }
            finally { Close(); }
        };
    }
}
