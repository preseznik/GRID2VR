using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace GRID2VR;

internal static class SessionRecovery
{
    // Repair the old pre-launch lock only after proving there is nothing to restore.
    // Actual deployments and changed settings still go through explicit recovery.
    internal static bool TryClearUnstarted(string root, string? documents = null)
    {
        using var mutex = new Mutex(false, Installation.MutexName);
        var locked = false;
        try
        {
            try { locked = mutex.WaitOne(0); } catch (AbandonedMutexException) { locked = true; }
            if (!locked) return false;
            foreach (var name in new[] { "grid2", "grid2_avx" })
                foreach (var process in Process.GetProcessesByName(name)) using (process) return false;
            var data = AppPaths.Data(root);
            var journal = Path.Combine(data, "session-pending.json");
            if (!File.Exists(journal)) return false;
            foreach (var name in new[] { "diagnostic-pending.json", "practice-flow-pending.json", "steamvr-theater-pending.json" })
                if (File.Exists(Path.Combine(data, name))) return false;
            var game = AppPaths.Game(root);
            foreach (var name in new[] { "d3d11.dll", "dxgi.dll", "GRID2VR.diagnostic-session", "GRID2VR.session.xml" })
                if (File.Exists(Path.Combine(game, name))) return false;
            var bytes = Read(journal, 256 * 1024);
            using var session = JsonDocument.Parse(bytes);
            var state = session.RootElement;
            if (state.GetProperty("schema").GetInt32() != 1 || state.GetProperty("run").ValueKind != JsonValueKind.Null ||
                state.GetProperty("mode").GetString() is not ("Desktop" or "Headset") ||
                state.GetProperty("runtime").GetString() is not ("VDXR" or "SteamVR")) return false;
            var workerId = state.GetProperty("workerId").GetInt32();
            if (workerId <= 0) return false;
            var started = state.GetProperty("workerStartedUtc").GetDateTimeOffset();
            try
            {
                using var worker = Process.GetProcessById(workerId);
                if (worker.StartTime.ToUniversalTime().Ticks == started.UtcTicks) return false;
            }
            catch (ArgumentException) { } // The recorded worker has exited; PID reuse alone is not ownership.
            var backupPath = state.GetProperty("backup").GetString()!;
            var backup = Path.GetFileName(backupPath) == "backup"
                ? Child(Child(Path.Combine(data, "logs"), Path.GetDirectoryName(backupPath)!, @"\d{8}-\d{6}-\d{3}"), backupPath, "backup")
                : Child(data, backupPath, @"stereo-probe-\d{8}-\d{6}-\d{3}");
            using var manifest = JsonDocument.Parse(Read(Path.Combine(backup, "manifest.json"), 1024 * 1024));
            var files = manifest.RootElement.GetProperty("files");
            if (files.GetArrayLength() is < 2 or > 1000) return false;
            documents ??= Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "My Games", "GRID 2", "hardwaresettings");
            var settings = new HashSet<string>(new[] { Path.Combine(documents, "hardware_settings_config.xml"), Path.Combine(documents, "hardware_settings_info.xml") }, StringComparer.OrdinalIgnoreCase);
            var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var steamSaves = Path.Combine(ProfileService.SteamRoot(), "userdata") + Path.DirectorySeparatorChar;
            long total = 0;
            foreach (var file in files.EnumerateArray())
            {
                var path = file.GetProperty("path").GetString()!;
                if (!Path.IsPathFullyQualified(path) || !paths.Add(Path.GetFullPath(path)) ||
                    (file.TryGetProperty("probeBefore", out var deployed) && deployed.ValueKind != JsonValueKind.Null)) return false;
                if (settings.Contains(path)) HardwareSettingsPath.Validate(path);
                else
                {
                    var full = Path.GetFullPath(path);
                    var save = full.StartsWith(steamSaves, StringComparison.OrdinalIgnoreCase) &&
                        Regex.IsMatch(full[steamSaves.Length..], @"\A[0-9]+[\\/]44350[\\/]");
                    if (!full.StartsWith(game + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) && !save) return false;
                    Updates.PlainPath(path);
                }
                var saved = Child(backup, file.GetProperty("backup").GetString()!, @"protected-[0-9]+\.bin");
                total += new FileInfo(path).Length + new FileInfo(saved).Length;
                if (total > 200L * 1024 * 1024) return false;
                var hash = file.GetProperty("before").GetString()!;
                if (!Matches(path, hash) || !Matches(saved, hash)) return false;
            }
            if (!settings.IsSubsetOf(paths)) return false;
            var executables = manifest.RootElement.GetProperty("executables");
            if (executables.GetArrayLength() != 2) return false;
            var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var file in executables.EnumerateArray())
            {
                var path = Child(game, file.GetProperty("path").GetString()!, @"grid2(?:_avx)?\.exe");
                if (!names.Add(path) || !Matches(path, file.GetProperty("before").GetString()!)) return false;
            }
            // Keep the journal as evidence. A rename cannot leave a half-cleared journal.
            var archived = Path.Combine(backup, "user-session-recovered-unchanged.json");
            Updates.PlainPath(archived);
            if (!Read(journal, 256 * 1024).SequenceEqual(bytes)) return false;
            File.Move(journal, archived);
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or ArgumentException or
            InvalidOperationException or KeyNotFoundException or FormatException or System.ComponentModel.Win32Exception)
        {
            return false; // Preserve anything we cannot prove safe; the recovery UI remains available.
        }
        finally { if (locked) mutex.ReleaseMutex(); }
    }

    private static byte[] Read(string path, int bound)
    {
        Updates.PlainPath(path);
        if (new FileInfo(path).Length > bound) throw new IOException("Recovery metadata exceeds its limit.");
        return File.ReadAllBytes(path);
    }
    private static string Child(string parent, string path, string pattern)
    {
        if (!Path.IsPathFullyQualified(path)) throw new IOException("Recovery path must be absolute.");
        var full = Path.GetFullPath(path);
        if (!string.Equals(Path.GetDirectoryName(full), parent, StringComparison.OrdinalIgnoreCase) ||
            !Regex.IsMatch(Path.GetFileName(full), "\\A(?:" + pattern + ")\\z", RegexOptions.IgnoreCase)) throw new IOException("Recovery path is outside its owned folder.");
        Updates.PlainPath(full);
        return full;
    }
    private static bool Matches(string path, string hash)
    {
        if (!Regex.IsMatch(hash, @"\A[0-9a-fA-F]{64}\z")) return false;
        using var input = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(input)).Equals(hash, StringComparison.OrdinalIgnoreCase);
    }
}
