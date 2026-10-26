namespace GRID2VR;

internal sealed class CockpitTextureForm : Form
{
    private readonly Label status = new() { Dock = DockStyle.Top, Height = 60, Text = "Preparing cockpit textures…" };
    private readonly ProgressBar progress = new() { Dock = DockStyle.Top, Height = 24 };
    private readonly Button cancel = new() { Text = "Cancel", Dock = DockStyle.Bottom, Height = 32 };
    private readonly CancellationTokenSource cancellation = new();
    private bool finished;
    internal Panel Content { get; } = new() { Dock = DockStyle.Fill, Padding = new Padding(20) };
    internal Exception? Failure { get; private set; }

    internal CockpitTextureForm(SessionClient client)
    {
        Text = "GRID2VR — Cockpit textures";
        ClientSize = new Size(520, 180);
        Font = new Font("Segoe UI", 10); AutoScaleMode = AutoScaleMode.Dpi;
        StartPosition = FormStartPosition.CenterParent; FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = false;
        Content.Controls.Add(progress); Content.Controls.Add(status); Content.Controls.Add(cancel); Controls.Add(Content);
        cancel.Click += (_, _) => CancelDownload();
        FormClosing += (_, e) => { if (!finished) { e.Cancel = true; if (cancel.Enabled) CancelDownload(); } };
        Shown += async (_, _) =>
        {
            using var http = Updates.CreateClient();
            try
            {
                await new CockpitTexturePack(client, http).Ensure(new Progress<TexturePackProgress>(DisplayProgress), cancellation.Token);
                DialogResult = DialogResult.OK;
            }
            catch (OperationCanceledException) { DialogResult = DialogResult.Cancel; }
            catch (Exception error) { Failure = error; DialogResult = DialogResult.Abort; }
            finally { finished = true; Close(); }
        };
    }

    internal void DisplayProgress(TexturePackProgress update)
    {
        if (finished || IsDisposed) return;
        status.Text = update.Message; progress.Value = update.Percent;
        cancel.Enabled = update.CanCancel && !cancellation.IsCancellationRequested;
        progress.Style = update.CanCancel ? ProgressBarStyle.Continuous : ProgressBarStyle.Marquee;
    }

    private void CancelDownload()
    {
        cancellation.Cancel(); cancel.Enabled = false; status.Text = "Cancelling download…";
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) cancellation.Dispose();
        base.Dispose(disposing);
    }
}
