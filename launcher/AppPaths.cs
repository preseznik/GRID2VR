using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Reflection;

namespace GRID2VR;

internal static class BuildInfo
{
    static string Metadata(string key, string fallback) => typeof(BuildInfo).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>().FirstOrDefault(x => x.Key == key)?.Value ?? fallback;
    public static string Channel => Metadata("ReleaseChannel", "Development");
    public static string Built => Metadata("BuildUtc", "Development build");
    public static string Revision => Metadata("BuildRevision", "local");
}

internal static class AppPaths
{
    public static bool Packaged(string root) => File.Exists(Path.Combine(root, "package.json"));
    public static string Game(string root) => Packaged(root) ? Path.GetDirectoryName(Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar))! : @"E:\SteamLibrary\steamapps\common\grid 2";
    public static string Data(string root)
    {
        if (!Packaged(root)) return Path.Combine(root, "artifacts");
        var id = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Game(root).ToUpperInvariant())))[..24];
        var basePath = Environment.GetEnvironmentVariable("GRID2VR_USER_BASE") ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "GRID2VR");
        if (!Path.IsPathFullyQualified(basePath)) throw new IOException("User-data location must be absolute.");
        return Path.Combine(basePath, id);
    }
    public static string Python(string root)
    {
        if (Packaged(root)) return Path.Combine(root, "payload", "python", "python.exe");
        var cache = File.ReadAllText(Path.Combine(root, "build", "win32", "CMakeCache.txt"));
        var match = System.Text.RegularExpressions.Regex.Match(cache, @"(?m)^_?Python3_EXECUTABLE:[^=]*=(.+)$");
        if (!match.Success) throw new IOException("Build configuration does not identify Python.");
        return match.Groups[1].Value.Trim();
    }
    public static string Shell(string root) => Packaged(root) ? Path.Combine(root, "payload", "powershell", "pwsh.exe") : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "PowerShell", "7", "pwsh.exe");
    public static void VerifyPackage(string root, Action<int, int>? progress = null)
    {
        if (!Packaged(root)) return;
        var path = Path.Combine(root, "package.json"); Updates.PlainPath(path);
        if (new FileInfo(path).Length > 2 * 1024 * 1024) throw new IOException("Package manifest exceeds its limit.");
        using var document = JsonDocument.Parse(File.ReadAllText(path));
        if (document.RootElement.GetProperty("schema").GetInt32() != 1) throw new IOException("Unsupported package manifest.");
        var game = Game(root);
        var files = document.RootElement.GetProperty("files").EnumerateObject().ToArray();
        var done = 0;
        foreach (var entry in files)
        {
            var file = Path.GetFullPath(Path.Combine(game, entry.Name));
            if (!file.StartsWith(game + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new IOException("Package path escapes the installation.");
            Updates.PlainPath(file);
            using var stream = File.OpenRead(file);
            if (!Convert.ToHexStringLower(SHA256.HashData(stream)).Equals(entry.Value.GetString(), StringComparison.Ordinal)) throw new IOException("Package file is missing or changed: " + entry.Name);
            ++done;
            if (done % 25 == 0 || done == files.Length) progress?.Invoke(done, files.Length);
        }
    }
}
