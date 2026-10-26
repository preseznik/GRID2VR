using System.Text.Json;
using System.Text.Json.Nodes;

namespace GRID2VR;

internal static class LaunchProgressTests
{
    internal static void Run(string root)
    {
        static void Check(bool value) { if (!value) throw new InvalidOperationException("Launch progress regression."); }
        var started = DateTimeOffset.UtcNow;
        var data = new JsonObject
        {
            ["last"] = new JsonObject { ["workerId"] = 123, ["workerStartedUtc"] = started.ToString("o"), ["state"] = "starting", ["message"] = "Backing up original files…" },
            ["session"] = new JsonObject { ["workerId"] = 123, ["workerStartedUtc"] = started.ToString("o") }
        };
        LaunchProgress Read(bool vr = false, bool exited = false) => LaunchProgress.Read(JsonSerializer.SerializeToElement(data), 123, started.UtcTicks, vr, exited);
        Check(Read().Message == "Backing up original files…" && !Read().Finished);
        Check(!Read(true).Finished); // Pending journal alone is not a game window or VR rendering.
        data["last"]!["gameWindowReady"] = true;
        Check(Read().Finished && !Read(true).Finished);
        data["last"]!["workerId"] = 124;
        Check(!Read().Finished);
        data["last"]!["workerId"] = 123;
        data["last"]!["workerStartedUtc"] = started.AddSeconds(-1).ToString("o");
        Check(!Read().Finished); // PID reuse cannot complete a new launch.
        data["last"]!["workerStartedUtc"] = started.ToString("o");
        data["status"] = new JsonObject { ["state"] = 0, ["acceptedPairs"] = 0, ["failed"] = false };
        Check(!Read(true).Finished);
        data["status"]!["acceptedPairs"] = 1;
        Check(Read(true).Finished);
        data["session"]!["workerId"] = 124;
        Check(!Read(true).Finished);
        data["session"]!["workerId"] = 123;
        data["status"]!["failed"] = true;
        Check(Read(true).Failed && !Read(true).Finished);
        data["status"] = null;
        foreach (var reason in new[] { "foreground", "xr_views", "engine_object", "ready", "scene_unavailable" })
        {
            data["readiness"] = new JsonObject { ["reason"] = reason };
            var result = Read(true);
            Check(!result.Finished && !result.Failed && !result.Message.Contains(reason));
        }
        data["readiness"]!["reason"] = "engine_install_refused";
        Check(Read(true).Failed);
        data["readiness"] = null; data["stopped"] = true;
        Check(Read(true).Failed);
        data["stopped"] = false;
        data["last"]!["state"] = "failed"; data["last"]!["message"] = "Selected runtime is missing.";
        Check(Read(true, true).Message == "Selected runtime is missing." && Read(true, true).Failed);
        Check(LaunchProgress.Read(default, 123, started.UtcTicks, true, true).Failed);
        Check(!LaunchProgress.Read(default, 123, started.UtcTicks, false, false).Failed);

        foreach (var modern in new[] { false, true })
        foreach (var vr in new[] { false, true })
        foreach (var scale in new[] { 1f, 1.5f, 2f })
        {
            using var form = new LaunchProgressForm(vr);
            LauncherAppearance.Modern = modern; LauncherAppearance.Apply(form);
            form.DisplayProgress(new(vr ? "Select the GRID 2 window to continue starting VR…" : "Backing up original settings and protected files…"));
            var content = form.Content; form.Controls.Remove(content);
            try
            {
                content.Dock = DockStyle.None; content.Font = form.Font; content.Visible = true; content.Size = form.ClientSize;
                content.Scale(new SizeF(scale, scale)); content.PerformLayout();
                var labels = content.Controls.OfType<Label>().ToArray();
                foreach (var label in labels)
                {
                    Check(content.ClientRectangle.Contains(label.Bounds));
                    var measured = TextRenderer.MeasureText(label.Text, label.Font, new Size(label.Width, int.MaxValue), TextFormatFlags.WordBreak);
                    Check(measured.Height <= label.Height);
                }
                var bar = content.Controls.OfType<ProgressBar>().Single();
                Check(bar.Style == ProgressBarStyle.Marquee && !form.TopMost && !form.ShowInTaskbar && !form.Visible);
                if (scale == 1f)
                {
                    using var bitmap = new Bitmap(content.Width, content.Height); content.DrawToBitmap(bitmap, content.ClientRectangle);
                    bitmap.Save(Path.Combine(root, "build", $"launcher-launch-progress-{(vr ? "vr" : "desktop")}-{(modern ? "modern" : "classic")}.png"));
                }
                form.DisplayProgress(new("The selected OpenXR runtime is unavailable. Check your runtime installation and try again.", Failed: true));
                Check(bar.Style == ProgressBarStyle.Continuous);
                Check(content.Controls.OfType<Panel>().Single().Controls.OfType<Button>().Single().Text == "Close");
                Check(!form.Visible);
            }
            finally { form.Controls.Add(content); }
        }
    }
}
