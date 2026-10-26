using System.Diagnostics;
using System.Text.Json;

namespace GRID2VR;

internal sealed record LaunchProgress(string Message, bool Finished = false, bool Failed = false)
{
    // Status may still describe the preceding run while the new worker starts.
    internal static LaunchProgress Read(JsonElement data, int workerId, long workerTicks, bool vr, bool exited)
    {
        var last = Child(data, "last");
        var ownsLast = Owned(last, workerId, workerTicks);
        if (ownsLast && Text(last, "state") == "failed")
            return new(Text(last, "message", "Launch failed. Open Logs for details."), Failed: true);
        if (exited || (ownsLast && Text(last, "state") == "complete"))
            return new("The session ended before loading completed. Check the launcher status or open Logs for details.", Failed: true);
        if (Owned(Child(data, "session"), workerId, workerTicks))
        {
            var status = Child(data, "status");
            var reason = Text(Child(data, "readiness"), "reason");
            if (Flag(status, "failed") || reason is "failed" or "engine_install_refused")
                return new("VR could not start. Check the launcher status or open Logs for details.", Failed: true);
            if (Flag(data, "stopped") || reason == "stopped")
                return new("VR was stopped. GRID 2 may still be running.", Failed: true);
            if (vr && Child(status, "state") is { ValueKind: JsonValueKind.Number } phaseValue && phaseValue.TryGetInt32(out var phase) && phase == 0 &&
                Child(status, "acceptedPairs") is { ValueKind: JsonValueKind.Number } pairValue && pairValue.TryGetInt64(out var pairs) && pairs > 0)
                return new("VR is running.", Finished: true);
            if (vr && reason.Length > 0)
                return new(reason switch
                {
                    "foreground" => "Select the GRID 2 window to continue starting VR…",
                    "xr_views" => "Waiting for the headset and tracking. Put on your headset…",
                    "engine_object" => "Waiting for the game engine…",
                    "ready" or "rendering" => "Starting VR rendering…",
                    _ => "Waiting for GRID 2 to finish loading the scene…"
                });
        }
        if (ownsLast && !vr && Flag(last, "gameWindowReady")) return new("GRID 2 is open.", Finished: true);
        return new(ownsLast ? Text(last, "message", "Preparing session…") : "Starting the launch worker…");
    }

    private static bool Owned(JsonElement value, int id, long ticks) =>
        Child(value, "workerId") is { ValueKind: JsonValueKind.Number } worker && worker.TryGetInt32(out var actual) && actual == id &&
        DateTimeOffset.TryParse(Text(value, "workerStartedUtc"), out var started) && started.UtcTicks == ticks;
    private static JsonElement Child(JsonElement value, string key) =>
        value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var found) ? found : default;
    private static string Text(JsonElement value, string key, string fallback = "") =>
        Child(value, key) is { ValueKind: JsonValueKind.String } found ? found.GetString()! : fallback;
    private static bool Flag(JsonElement value, string key) => Child(value, key).ValueKind == JsonValueKind.True;
}

internal sealed class LaunchProgressForm : LauncherForm
{
    private readonly Label step = new() { Dock = DockStyle.Top, Height = 62, Text = "Saving launch settings…", AutoEllipsis = true };
    private readonly ProgressBar progress = new() { Dock = DockStyle.Top, Height = 16, Style = ProgressBarStyle.Marquee, MarqueeAnimationSpeed = 30 };
    private readonly Label elapsed = new() { Dock = DockStyle.Top, Height = 32, TextAlign = ContentAlignment.MiddleLeft };
    private readonly Label hint = new() { Dock = DockStyle.Top, Height = 48 };
    private readonly Button dismiss = new() { Text = "Hide", Dock = DockStyle.Right, Width = 85, DialogResult = DialogResult.Cancel };
    private readonly Stopwatch watch = Stopwatch.StartNew();
    private readonly System.Windows.Forms.Timer timer = new() { Interval = 1000 };
    internal Panel Content { get; } = new() { Dock = DockStyle.Fill, Padding = new Padding(20) };
    protected override bool ShowWithoutActivation => true;

    internal LaunchProgressForm(bool vr)
    {
        Text = vr ? "Launching GRID 2 in VR" : "Launching GRID 2";
        Font = new Font("Segoe UI", 10);
        ClientSize = new Size(460, 230); AutoScaleMode = AutoScaleMode.Dpi;
        StartPosition = FormStartPosition.CenterParent; FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = ShowInTaskbar = false;
        hint.Text = vr ? "This closes when VR starts. You can hide it and let loading continue."
            : "This closes when the game window opens. You can hide it and let loading continue.";
        var buttons = new Panel { Dock = DockStyle.Bottom, Height = 32 };
        buttons.Controls.Add(dismiss);
        Content.Controls.Add(hint); Content.Controls.Add(elapsed); Content.Controls.Add(progress); Content.Controls.Add(step); Content.Controls.Add(buttons);
        Controls.Add(Content); CancelButton = dismiss;
        dismiss.Click += (_, _) => Close();
        timer.Tick += (_, _) => UpdateElapsed();
        Shown += (_, _) => timer.Start();
        UpdateElapsed();
    }

    internal void DisplayProgress(LaunchProgress update)
    {
        if (IsDisposed) return;
        if (update.Finished) { Close(); return; }
        step.Text = update.Message;
        if (!update.Failed) return;
        timer.Stop(); watch.Stop(); UpdateElapsed();
        Text = "GRID 2 launch stopped";
        progress.Style = ProgressBarStyle.Continuous; progress.Value = 0;
        hint.Text = "Close this message to return to the launcher."; dismiss.Text = "Close";
    }

    private void UpdateElapsed() => elapsed.Text = $"Elapsed: {(int)watch.Elapsed.TotalMinutes}:{watch.Elapsed.Seconds:00}";
    protected override void Dispose(bool disposing)
    {
        if (disposing) timer.Dispose();
        base.Dispose(disposing);
    }
}
