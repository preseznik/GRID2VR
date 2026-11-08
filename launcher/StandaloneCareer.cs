using System.Globalization;

namespace GRID2VR;

internal static class StandaloneCareer
{
    internal static string? SaveRoot(string game)
    {
        var config = Path.Combine(game, "SteamConfig.ini");
        ProfileStore.NoLinks(config);
        if (!File.Exists(config) || new FileInfo(config).Length > 65536) return null;
        string? player = null, saveType = null;
        var settings = false;
        foreach (var raw in File.ReadLines(config))
        {
            var line = raw.Trim();
            if (line.StartsWith('[')) { settings = line.Equals("[Settings]", StringComparison.OrdinalIgnoreCase); continue; }
            if (!settings || line.StartsWith(';') || line.StartsWith('#')) continue;
            var pair = line.Split('=', 2, StringSplitOptions.TrimEntries);
            if (pair.Length != 2) continue;
            if (pair[0].Equals("PlayerName", StringComparison.OrdinalIgnoreCase))
            {
                if (player != null) return null;
                player = pair[1];
            }
            if (pair[0].Equals("SaveType", StringComparison.OrdinalIgnoreCase))
            {
                if (saveType != null) return null;
                saveType = pair[1];
            }
        }
        if (saveType != "0" || player == null || !ProfileStore.ValidSaveName(player)) return null;
        var path = Path.Combine(game, "Profile", player, "Saves");
        ProfileStore.NoLinks(path);
        return Directory.Exists(path) ? path : null;
    }

    internal static void Publish(string staged, string saves)
    {
        // Slot zero belongs to the player's original career. A partial slot is occupied too.
        ProfileStore.NoLinks(saves);
        var slot = 1;
        while (Directory.EnumerateFileSystemEntries(saves, $"savegame@profile{slot}#*").Any())
        {
            slot = checked(slot + 1);
        }
        var prefix = "savegame@profile" + slot.ToString(CultureInfo.InvariantCulture) + "#";
        var members = ProfileStore.NormalSaveMembers;
        var published = new List<string>();
        // Prepare all three files on the destination volume. Rename the directory
        // entry last, so the game never lists a partially written career.
        var temp = Path.Combine(saves, ".grid2vr-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        try
        {
            ProfileStore.NoLinks(temp);
            foreach (var member in members)
            {
                var source = Path.Combine(staged, "savegame@profile0#" + member);
                ProfileStore.NoLinks(source);
                using var input = File.OpenRead(source);
                using var output = new FileStream(Path.Combine(temp, member), FileMode.CreateNew, FileAccess.Write, FileShare.None);
                input.CopyTo(output);
                output.Flush(true);
            }
            foreach (var member in members)
            {
                var target = Path.Combine(saves, prefix + member);
                File.Move(Path.Combine(temp, member), target, overwrite: false);
                published.Add(target);
            }
        }
        catch
        {
            foreach (var path in published) { ProfileStore.NoLinks(path); File.Delete(path); }
            throw;
        }
        finally
        {
            foreach (var member in members)
            {
                var path = Path.Combine(temp, member);
                ProfileStore.NoLinks(path);
                if (File.Exists(path)) File.Delete(path);
            }
            Directory.Delete(temp);
        }
    }
}
