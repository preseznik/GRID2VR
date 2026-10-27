using System.Diagnostics;
using Microsoft.Win32;

namespace GRID2VR;

internal static class ProfileService
{
    internal static ProfileStore Open(string workspace)
    {
        // Cache the UI capability for this launcher instance. The session worker
        // independently revalidates the executable pair before managed save access.
        var managed = GameExecutable.ValidateInstallation(AppPaths.Game(workspace)) == "Steam";
        return new(workspace, () => RequireClosed(workspace), CurrentSaveRoot, AccountId, managed);
    }

    internal static string? AccountId()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam\ActiveProcess");
        var value = key?.GetValue("ActiveUser");
        if (value is int account && account != 0)
            return unchecked((uint)account).ToString(System.Globalization.CultureInfo.InvariantCulture);
        // Some Steam sessions leave ActiveProcess at zero even while games are
        // authenticated. Use its last-selected account only while Steam runs;
        // the game-side provider still verifies the actual account on launch.
        var processes = Process.GetProcessesByName("steam");
        try
        {
            if (processes.Length == 0) return null;
            var path = Path.Combine(SteamRoot(), "config", "loginusers.vdf");
            ProfileStore.NoLinks(path);
            if (!File.Exists(path) || new FileInfo(path).Length > 65536) return null;
            return SteamAccountFile.ReadRecent(File.ReadAllText(path));
        }
        finally { foreach (var process in processes) process.Dispose(); }
    }

    internal static string SteamRoot()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
        return Path.GetFullPath(key?.GetValue("SteamPath") as string ?? @"C:\Program Files (x86)\Steam");
    }

    internal static string? CurrentSaveRoot()
    {
        var account = AccountId();
        if (account == null) throw new IOException("Sign into Steam before accessing career profiles.");
        var path = Path.Combine(SteamRoot(), "userdata", account, "44350", "remote");
        ProfileStore.NoLinks(path);
        return Directory.Exists(path) ? path : null;
    }

    static void RequireClosed(string workspace)
    {
        foreach (var name in new[] { "grid2", "grid2_avx" })
            foreach (var process in Process.GetProcessesByName(name))
                using (process) throw new IOException("Close GRID 2 before changing career profiles.");
        foreach (var name in new[] { "session-pending.json", "diagnostic-pending.json", "practice-flow-pending.json", "steamvr-theater-pending.json" })
            if (File.Exists(Path.Combine(AppPaths.Data(workspace), name)))
                throw new IOException("Restore the pending session before changing career profiles.");
    }
}
