using System.Diagnostics;
using System.Net.Http;

namespace GRID2VR;

internal sealed record TexturePackProgress(string Message, int Percent = 0, bool CanCancel = true);

internal sealed class CockpitTexturePack(SessionClient client, HttpClient http,
    Func<ProcessStartInfo, Task>? worker = null, ReleaseInstaller? download = null)
{
    internal const string Version = "0.2.0-preview.1";
    // Immutable artwork release, independent of the launcher version. The worker
    // additionally verifies the pinned catalog and every extracted file.
    internal static readonly ReleaseInstaller Asset = new(
        "GRID2VR-CockpitTextures-" + Version + ".zip",
        new(Updates.Repository + "/releases/download/v0.1.6/GRID2VR-CockpitTextures-" + Version + ".zip"),
        122784997, "642938c9fb69f89e343181522aed927505be0222e374e46e8c5990dfd88417cd");

    internal async Task Ensure(IProgress<TexturePackProgress> progress, CancellationToken cancellation)
    {
        var target = Path.Combine(AppPaths.Game(client.Root), "GRID2VR", "addons", "cockpit-textures", Version);
        Updates.PlainPath(target);
        cancellation.ThrowIfCancellationRequested();
        if (Path.Exists(target))
        {
            progress.Report(new("Checking installed cockpit textures…", CanCancel: false));
            await Run("Verify");
            return;
        }
        Installation.RequireIdle(client.Root);
        progress.Report(new("Downloading cockpit textures (123 MB)…"));
        var zip = await new Updates(http).Download(download ?? Asset, client.Root,
            new DownloadProgress(progress), cancellation);
        cancellation.ThrowIfCancellationRequested();
        // Let the short, guarded transaction finish; never kill it midway through
        // extraction or its atomic move. Cancellation is available while downloading.
        progress.Report(new("Installing and verifying cockpit textures…", 100, false));
        await Run("Install", zip);
        try { Updates.PlainPath(zip); File.Delete(zip); }
        catch (IOException) { /* A retained download must not undo a successful installation. */ }
        catch (UnauthorizedAccessException) { }
    }

    private Task Run(string action, string? zip = null)
    {
        var command = client.Command("Cockpit-TexturePack.ps1", "-Action", action, "-GameRoot", AppPaths.Game(client.Root));
        if (zip is not null) { command.ArgumentList.Add("-ZipPath"); command.ArgumentList.Add(zip); }
        return (worker ?? RunWorker)(command);
    }

    private static async Task RunWorker(ProcessStartInfo command)
    {
        command.RedirectStandardOutput = command.RedirectStandardError = true;
        command.Environment["NO_COLOR"] = "1";
        using var process = Process.Start(command) ?? throw new IOException("Could not start cockpit texture installation.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        var message = await error; var result = await output;
        if (process.ExitCode != 0) throw new IOException("Cockpit textures could not be prepared. " + (string.IsNullOrWhiteSpace(message) ? result : message).Trim());
    }

    private sealed class DownloadProgress(IProgress<TexturePackProgress> progress) : IProgress<int>
    {
        public void Report(int value) => progress.Report(new($"Downloading cockpit textures… {value}%", value));
    }
}
