using System.Diagnostics;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;

namespace GRID2VR;

internal static class CockpitTexturePackTests
{
    internal static void Run(string root, string fixture)
    {
        using var catalog = JsonDocument.Parse(File.ReadAllText(Path.Combine(root, "tools", "data", "cockpit-texture-pack.json")));
        Check(catalog.RootElement.GetProperty("zipSha256").GetString() == CockpitTexturePack.Asset.Sha256);
        Check(catalog.RootElement.GetProperty("zipBytes").GetInt64() == CockpitTexturePack.Asset.Size);
        Check(catalog.RootElement.GetProperty("version").GetString() == CockpitTexturePack.Version);
        Check(CockpitTexturePack.Asset.Url.AbsoluteUri == Updates.Repository + "/releases/download/v" +
            Updates.CurrentVersion + "/" + CockpitTexturePack.Asset.Name);
        var previous = Environment.GetEnvironmentVariable("GRID2VR_USER_BASE");
        Environment.SetEnvironmentVariable("GRID2VR_USER_BASE", Path.Combine(fixture, "texture-user-data"));
        try { Task.Run(() => Exercise(fixture)).GetAwaiter().GetResult(); }
        finally { Environment.SetEnvironmentVariable("GRID2VR_USER_BASE", previous); }
        foreach (var scale in new[] { 1f, 1.5f, 2f })
        {
            using var dialog = new CockpitTextureForm(new SessionClient(fixture));
            var content = dialog.Content; dialog.Controls.Remove(content);
            try
            {
                content.Dock = DockStyle.None; content.Font = dialog.Font; content.Visible = true; content.Size = dialog.ClientSize;
                content.Scale(new SizeF(scale, scale)); content.PerformLayout();
                dialog.DisplayProgress(new("Downloading cockpit textures… 45%", 45));
                var cancel = content.Controls.OfType<Button>().Single(); var bar = content.Controls.OfType<ProgressBar>().Single();
                Check(cancel.Enabled && bar.Value == 45 && bar.Style == ProgressBarStyle.Continuous);
                foreach (Control control in content.Controls) Check(content.ClientRectangle.Contains(control.Bounds));
                var label = content.Controls.OfType<Label>().Single();
                Check(TextRenderer.MeasureText(label.Text, label.Font, new Size(label.Width, int.MaxValue), TextFormatFlags.WordBreak).Height <= label.Height);
                if (scale == 1f)
                {
                    using var image = new Bitmap(content.Width, content.Height); content.DrawToBitmap(image, content.ClientRectangle);
                    image.Save(Path.Combine(root, "build", "cockpit-texture-progress.png"));
                }
                dialog.DisplayProgress(new("Installing and verifying cockpit textures…", 100, false));
                Check(!cancel.Enabled && bar.Style == ProgressBarStyle.Marquee && !dialog.Visible);
            }
            finally { dialog.Controls.Add(content); }
        }
    }

    private static async Task Exercise(string fixture)
    {
        var root = Path.Combine(fixture, "texture game", "GRID2VR");
        Directory.CreateDirectory(Path.Combine(root, "payload", "powershell"));
        File.WriteAllText(Path.Combine(root, "package.json"), "{}");
        File.WriteAllText(AppPaths.Shell(root), "test worker placeholder");
        var client = new SessionClient(root);
        var target = Path.Combine(root, "addons", "cockpit-textures", CockpitTexturePack.Version);
        byte[] bytes = [1, 2, 3, 4];
        var asset = CockpitTexturePack.Asset with { Size = bytes.Length, Sha256 = Convert.ToHexStringLower(SHA256.HashData(bytes)) };
        var handler = new DownloadHandler(bytes);
        using var http = new HttpClient(handler);
        var calls = new List<string>(); var updates = new ProgressLog();
        var failWorker = false;
        Task Worker(ProcessStartInfo info)
        {
            Check(info.CreateNoWindow && !info.UseShellExecute && !info.ArgumentList.Contains("-Command"));
            Check(info.ArgumentList.Contains(Path.Combine(root, "tools", "Cockpit-TexturePack.ps1")));
            var args = info.ArgumentList;
            Check(args[args.IndexOf("-GameRoot") + 1] == AppPaths.Game(root));
            var action = args[args.IndexOf("-Action") + 1]; calls.Add(action);
            if (failWorker) throw new IOException("Fixture installation failure");
            if (action == "Install")
            {
                Check(File.ReadAllBytes(args[args.IndexOf("-ZipPath") + 1]).SequenceEqual(bytes));
                Directory.CreateDirectory(target);
            }
            return Task.CompletedTask;
        }
        var service = new CockpitTexturePack(client, http, Worker, asset);
        await service.Ensure(updates, CancellationToken.None);
        Check(calls.SequenceEqual(new[] { "Install" }) && handler.Calls == 1);
        Check(updates.Items.Any(p => p.CanCancel && p.Percent == 100) && !updates.Items.Last().CanCancel);
        Check(!Directory.EnumerateFiles(Path.Combine(AppPaths.Data(root), "updates")).Any());
        await service.Ensure(updates, CancellationToken.None);
        Check(calls.SequenceEqual(new[] { "Install", "Verify" }) && handler.Calls == 1);
        failWorker = true;
        await Refuses(() => service.Ensure(updates, CancellationToken.None));
        Check(Directory.Exists(target) && handler.Calls == 1); // Changed installed content is retained, never silently accepted.
        Directory.Delete(target); failWorker = false; calls.Clear();
        handler.Bytes = [4, 3, 2, 1];
        await Refuses(() => service.Ensure(updates, CancellationToken.None));
        Check(calls.Count == 0 && !Directory.Exists(target));
        Check(!Directory.EnumerateFiles(Path.Combine(AppPaths.Data(root), "updates")).Any());
        handler.Bytes = bytes; handler.Status = HttpStatusCode.ServiceUnavailable;
        await Refuses(() => service.Ensure(updates, CancellationToken.None));
        Check(calls.Count == 0 && !Directory.Exists(target));
        handler.Status = HttpStatusCode.OK;
        using var cancellation = new CancellationTokenSource();
        var cancellingProgress = new ProgressLog { OnReport = p => { if (p.Percent > 0) cancellation.Cancel(); } };
        try { await service.Ensure(cancellingProgress, cancellation.Token); throw new Exception("Cancellation ignored"); }
        catch (OperationCanceledException) { }
        Check(calls.Count == 0 && !Directory.Exists(target));
        Check(!Directory.EnumerateFiles(Path.Combine(AppPaths.Data(root), "updates")).Any());
        var pending = Path.Combine(AppPaths.Data(root), "session-pending.json");
        File.WriteAllText(pending, "{}"); var requests = handler.Calls;
        await Refuses(() => service.Ensure(updates, CancellationToken.None));
        Check(handler.Calls == requests && calls.Count == 0); File.Delete(pending);
        failWorker = true;
        await Refuses(() => service.Ensure(updates, CancellationToken.None));
        Check(!Directory.Exists(target));
        failWorker = false;
        await service.Ensure(updates, CancellationToken.None); // Retry after a failed transaction.
        Check(Directory.Exists(target));
    }

    private static async Task Refuses(Func<Task> action)
    {
        try { await action(); }
        catch (Exception error) when (error is IOException or HttpRequestException) { return; }
        throw new InvalidOperationException("Cockpit texture failure was accepted.");
    }
    private static void Check(bool ok, [System.Runtime.CompilerServices.CallerLineNumber] int line = 0)
    { if (!ok) throw new InvalidOperationException($"Cockpit texture download regression at line {line}."); }
    private sealed class ProgressLog : IProgress<TexturePackProgress>
    {
        internal List<TexturePackProgress> Items { get; } = [];
        internal Action<TexturePackProgress>? OnReport { get; init; }
        public void Report(TexturePackProgress value) { Items.Add(value); OnReport?.Invoke(value); }
    }
    private sealed class DownloadHandler(byte[] bytes) : HttpMessageHandler
    {
        internal byte[] Bytes { get; set; } = bytes;
        internal int Calls { get; private set; }
        internal HttpStatusCode Status { get; set; } = HttpStatusCode.OK;
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellation)
        {
            ++Calls; Check(request.RequestUri == CockpitTexturePack.Asset.Url);
            return Task.FromResult(new HttpResponseMessage(Status) { Content = new ByteArrayContent(Bytes) });
        }
    }
}
