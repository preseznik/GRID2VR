using System.Diagnostics;
using System.Reflection;
using System.Text.Json;

namespace GRID2VR;

internal static class UpdateNotificationTests
{
    internal static void Run(string root, SessionClient fixture)
    {
        var path = Path.Combine(fixture.Root, "update-notification");
        var client = new SessionClient(path);
        var offer = new AppRelease("v9.0.0", ReleaseVersion.Parse("9.0.0")!, new Uri(Updates.Repository + "/releases/tag/v9.0.0"),
            new("GRID2VR-9.0.0-Setup.exe", new Uri(Updates.Repository + "/releases/download/v9.0.0/GRID2VR-9.0.0-Setup.exe"), 3, new string('a', 64)));
        var evidence = new List<object>();
        foreach (var modern in new[] { false, true })
        foreach (var scale in new[] { 1f, 1.5f, 2f })
        {
            new UpdatePreferences().Save(path);
            var calls = new List<bool>();
            AppRelease? next = offer;
            using var form = new MainForm(client, new ProfileStore(path, () => { }, () => null),
                (preview, _) => { calls.Add(preview); return Task.FromResult<AppRelease?>(next); });
            LauncherAppearance.Modern = modern; LauncherAppearance.Apply(form);
            var panel = (UpdatesPanel)form.Controls.Find("Updates", true).Single();
            var notice = (Button)form.Controls.Find("UpdateAvailable", true).Single();
            var help = (Button)form.Controls.Find("HelpAbout", true).Single();
            var header = (TableLayoutPanel)help.Parent!;
            var parent = header.Parent!;
            parent.Controls.Remove(header);
            try
            {
                header.Dock = DockStyle.None; header.Visible = true; header.Font = form.Font;
                Check(!notice.Visible && calls.Count == 0);
                Complete(panel.Start());
                Check(calls.SequenceEqual(new[] { false }) && notice.Visible && notice.AccessibleDescription!.Contains(offer.Tag));
                header.PerformLayout();
                var view = header.Controls.OfType<LauncherViewPicker>().Single();
                Check(header.GetPositionFromControl(view).Column + 1 == header.GetPositionFromControl(help).Column);
                header.Scale(new SizeF(scale, scale));
                foreach (var width in new[] { 520, 860 })
                {
                    header.Width = (int)(width * scale); header.PerformLayout();
                    header.Height = header.GetPreferredSize(new Size(header.Width, 0)).Height; header.PerformLayout();
                    var controls = header.Controls.Cast<Control>().ToArray();
                    Check(header.GetPositionFromControl(notice).Row == 1 || header.GetPositionFromControl(notice).Column + 1 == header.GetPositionFromControl(view).Column);
                    foreach (var control in controls) Check(header.ClientRectangle.Contains(control.Bounds));
                    for (var a = 0; a < controls.Length; ++a)
                    for (var b = a + 1; b < controls.Length; ++b) Check(!controls[a].Bounds.IntersectsWith(controls[b].Bounds));
                    using var bitmap = new Bitmap(header.Width, header.Height);
                    header.DrawToBitmap(bitmap, header.ClientRectangle);
                    bitmap.Save(Path.Combine(root, "build", $"launcher-update-header-{width}-{scale.ToString(System.Globalization.CultureInfo.InvariantCulture)}-{(modern ? "modern" : "classic")}.png"));
                    evidence.Add(new { modern, scale, width, noticeVisible = notice.Visible, boundsValid = true });
                }
                next = null; Complete(panel.Start(forceCheck: true));
                Check(!notice.Visible && panel.AvailableUpdate is null);
                var toggle = Descendants(panel).OfType<CheckBox>().Single(c => c.Text == "Include experimental releases");
                next = offer; toggle.Checked = true;
                Check(calls.Last() && notice.Visible && UpdatePreferences.Load(path).Experimental);
                next = null; toggle.Checked = false;
                Check(!calls.Last() && !notice.Visible && !UpdatePreferences.Load(path).Experimental);
                Check(!form.Visible);
            }
            finally { parent.Controls.Add(header); }
        }

        new UpdatePreferences(false, true).Save(path);
        var aboutCalls = 0;
        using (var panel = new UpdatesPanel(path, releaseCheck: (_, _) => { ++aboutCalls; return Task.FromResult<AppRelease?>(offer); }))
        {
            Complete(panel.Start()); Check(aboutCalls == 0);
        }
        using (var about = new AboutForm(path, 121, 0, sessionIdle: false, knownUpdate: offer,
            releaseCheck: (preview, _) => { Check(preview); ++aboutCalls; return Task.FromResult<AppRelease?>(offer); }))
        {
            Check(about.AvailableUpdate == offer && aboutCalls == 0);
            AppRelease? reported = null; about.UpdateAvailable += result => reported = result;
            typeof(Form).GetMethod("OnShown", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(about, [EventArgs.Empty]);
            Check(aboutCalls == 1 && reported == offer && !about.Visible);
            Check(!Descendants(about).OfType<Button>().Single(b => b.Text == "Download and install…").Enabled);
        }
        new UpdatePreferences().Save(path);
        using (var panel = new UpdatesPanel(path, releaseCheck: (_, _) => Task.FromException<AppRelease?>(new IOException("Offline fixture"))))
        {
            Complete(panel.Start());
            Check(panel.AvailableUpdate is null && panel.Controls.Find("UpdateStatus", true).Single().Text.Contains("Offline fixture"));
        }
        var pending = new TaskCompletionSource<AppRelease?>();
        using (var panel = new UpdatesPanel(path, releaseCheck: (_, _) => pending.Task))
        {
            var reported = false; panel.UpdateAvailable += _ => reported = true;
            var check = panel.Start();
            new UpdatePreferences(true, true).Save(path);
            pending.SetResult(offer); Complete(check);
            Check(!reported && panel.AvailableUpdate is null);
        }
        var cancellation = CancellationToken.None;
        var disposed = new UpdatesPanel(path, releaseCheck: (_, token) =>
        {
            cancellation = token;
            return Task.Delay(Timeout.Infinite, token).ContinueWith<AppRelease?>(_ => { token.ThrowIfCancellationRequested(); return offer; }, TaskScheduler.Default);
        });
        var closingCheck = disposed.Start(); disposed.Dispose(); Complete(closingCheck);
        Check(cancellation.IsCancellationRequested);
        File.WriteAllText(Path.Combine(root, "build/launcher-update-notification.json"), JsonSerializer.Serialize(new
        {
            startupCheck = true, disabledStartupRespected = true, aboutChecksAutomatically = true, channelChangesCheck = true,
            noUpdateClearsNotice = true, staleChannelIgnored = true, offlineHandled = true, closingCancelsCheck = true,
            visibleWindows = 0, gameLaunched = false, layouts = evidence
        }, new JsonSerializerOptions { WriteIndented = true }));
    }
    private static IEnumerable<Control> Descendants(Control parent)
    {
        foreach (Control child in parent.Controls)
        {
            yield return child;
            foreach (var descendant in Descendants(child)) yield return descendant;
        }
    }
    private static void Complete(Task task)
    {
        var elapsed = Stopwatch.StartNew();
        while (!task.IsCompleted && elapsed.Elapsed < TimeSpan.FromSeconds(5)) { Application.DoEvents(); Thread.Sleep(1); }
        Check(task.IsCompleted); task.GetAwaiter().GetResult();
    }
    private static void Check(bool value, [System.Runtime.CompilerServices.CallerArgumentExpression(nameof(value))] string? expression = null)
    { if (!value) throw new InvalidOperationException("Update notification regression: " + expression); }
}
