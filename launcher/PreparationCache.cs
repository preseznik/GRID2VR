using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace GRID2VR;

// A freshness check for an already successful setup, not an integrity verdict.
// Session deployment still checks exact executable, asset and payload hashes.
internal static class PreparationCache
{
    internal static readonly string[] Banks = ["practice-flow-20261014", "practice-menus-20261106", "startup-movies", "cockpit-cameras", "cockpit-start"];
    private const int Schema = 1;
    private sealed record Receipt(int Schema, string Key);
    private static string PathFor(string root) => Path.Combine(AppPaths.Data(root), "preparation-cache.json");

    internal static bool IsCurrent(string root)
    {
        try
        {
            if (!AppPaths.Packaged(root) || Installation.HasPending(root)) return false;
            var path = PathFor(root); Updates.PlainPath(path);
            if (!File.Exists(path) || new FileInfo(path).Length > 1024) return false;
            var bytes = File.ReadAllBytes(path);
            if (bytes.Length > 1024) return false;
            var receipt = JsonSerializer.Deserialize<Receipt>(bytes);
            return receipt is { Schema: Schema } && receipt.Key == Key(root);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or ArgumentException or InvalidOperationException or KeyNotFoundException or FormatException)
        {
            return false; // The existing full preparation path reports the actual refusal.
        }
    }

    // Called only after Installation.Setup has completed its full validation.
    internal static void SaveVerified(string root)
    {
        var path = PathFor(root); Updates.PlainPath(path);
        var value = JsonSerializer.Serialize(new Receipt(Schema, Key(root)));
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, value);
            File.Move(temporary, path, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    private static string Key(string root)
    {
        var game = AppPaths.Game(root); var data = AppPaths.Data(root);
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var checkedPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var files = 0;
        void Add(string value) => digest.AppendData(Encoding.UTF8.GetBytes(value + '\0'));
        void Plain(string path)
        {
            for (var current = Path.GetFullPath(path); !string.IsNullOrEmpty(current); current = Path.GetDirectoryName(current))
            {
                if (!checkedPaths.Add(current)) break;
                if (Path.Exists(current) && File.GetAttributes(current).HasFlag(FileAttributes.ReparsePoint))
                    throw new IOException("Linked preparation input retained.");
            }
        }
        void Stamp(string path, bool required = true)
        {
            if (++files > 20000) throw new IOException("Too many preparation inputs.");
            Plain(path); var info = new FileInfo(path); info.Refresh();
            Add(Path.GetFullPath(path).ToUpperInvariant());
            if (!info.Exists)
            {
                if (required) throw new IOException("Missing preparation input: " + path);
                Add("missing"); return;
            }
            Add(info.Length.ToString(CultureInfo.InvariantCulture));
            Add(info.LastWriteTimeUtc.Ticks.ToString(CultureInfo.InvariantCulture));
            Add(((int)info.Attributes).ToString(CultureInfo.InvariantCulture));
        }
        byte[] Read(string path, long limit)
        {
            Stamp(path);
            if (new FileInfo(path).Length > limit) throw new IOException("Preparation input exceeds its limit.");
            var bytes = File.ReadAllBytes(path);
            if (bytes.LongLength > limit) throw new IOException("Preparation input exceeds its limit.");
            return bytes;
        }
        void Bank(string folder, int depth = 0)
        {
            Plain(folder);
            if (depth > 8 || !Directory.Exists(folder)) throw new IOException("Missing or invalid prepared bank.");
            foreach (var path in Directory.EnumerateFileSystemEntries(folder).Order(StringComparer.Ordinal))
            {
                Plain(path);
                if (Directory.Exists(path)) Bank(path, depth + 1); else Stamp(path);
            }
        }
        Add(Schema.ToString(CultureInfo.InvariantCulture));
        Add(Path.GetFullPath(root).ToUpperInvariant()); Add(Path.GetFullPath(data).ToUpperInvariant());
        var manifest = Read(Path.Combine(root, "package.json"), 2 * 1024 * 1024);
        digest.AppendData(manifest);
        using (var package = JsonDocument.Parse(manifest))
        {
            if (package.RootElement.GetProperty("schema").GetInt32() != 1) throw new IOException("Unsupported package manifest.");
            foreach (var entry in package.RootElement.GetProperty("files").EnumerateObject().OrderBy(entry => entry.Name, StringComparer.Ordinal))
            {
                var path = Path.GetFullPath(Path.Combine(game, entry.Name));
                if (!path.StartsWith(game + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new IOException("Package path escapes the installation.");
                Stamp(path);
            }
        }
        foreach (var name in new[] { "grid2.exe", "grid2_avx.exe", "system/flow.bin", "system/states.bin", "cutscene/intro_buildup.xml", "cutscene/startsequence.xml" })
            Stamp(Path.Combine(game, name));
        using (var catalog = JsonDocument.Parse(Read(Path.Combine(root, "tools", "data", "cockpit-assets.json"), 256 * 1024)))
        {
            foreach (var car in catalog.RootElement.GetProperty("cars").EnumerateArray())
            {
                var id = car.GetProperty("car").GetString();
                if (id is null || id.Length != 3 || id.Any(value => !char.IsAsciiLetterLower(value) && !char.IsAsciiDigit(value))) throw new IOException("Invalid cockpit car.");
                Stamp(Path.Combine(game, "cars", "models", id, "cameras.xml"));
            }
        }
        Stamp(Path.Combine(data, "seat-positions.json"), required: false);
        foreach (var bank in Banks) Bank(Path.Combine(data, bank));
        return Convert.ToHexStringLower(digest.GetHashAndReset());
    }
}
