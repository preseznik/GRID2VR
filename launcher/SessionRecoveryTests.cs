using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace GRID2VR;

internal static class SessionRecoveryTests
{
    internal static void Run(string fixture)
    {
        static void Check(bool value) { if (!value) throw new IOException("Unstarted session recovery regression."); }
        static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
        var previous = Environment.GetEnvironmentVariable("GRID2VR_USER_BASE");
        Environment.SetEnvironmentVariable("GRID2VR_USER_BASE", Path.Combine(fixture, "recovery-data"));
        try
        {
            foreach (var test in new[] { "unchanged", "grouped", "settings", "asset", "backup", "executable", "worker", "run", "deployed", "theater", "foreign", "malformed", "outside", "outside-backup" })
            {
                var game = Path.Combine(fixture, "recovery-" + test);
                var root = Path.Combine(game, "GRID2VR"); Directory.CreateDirectory(root);
                File.WriteAllText(Path.Combine(root, "package.json"), "{}");
                var data = AppPaths.Data(root);
                Directory.CreateDirectory(data);
                var backup = test == "grouped" ? Path.Combine(data, "logs", "20261026-214003-560", "backup") :
                    test == "outside-backup" ? Path.Combine(fixture, "logs", "20261026-214003-560", "backup") :
                    Path.Combine(data, "stereo-probe-20261026-214003-560");
                Directory.CreateDirectory(backup);
                var documents = Path.Combine(fixture, "recovery-documents-" + test); Directory.CreateDirectory(documents);
                var files = new JsonArray();
                var originals = new Dictionary<string, string>();
                foreach (var path in new[] { Path.Combine(documents, "hardware_settings_config.xml"), Path.Combine(documents, "hardware_settings_info.xml"), Path.Combine(game, "camera.bin") })
                {
                    File.WriteAllText(path, "original " + Path.GetFileName(path));
                    var saved = Path.Combine(backup, "protected-" + files.Count + ".bin"); File.Copy(path, saved);
                    files.Add(new JsonObject { ["path"] = path, ["backup"] = saved, ["before"] = Hash(path) });
                    originals[path] = Hash(path);
                }
                var executables = new JsonArray();
                foreach (var name in new[] { "grid2.exe", "grid2_avx.exe" })
                {
                    var path = Path.Combine(game, name); File.WriteAllText(path, "executable fixture");
                    executables.Add(new JsonObject { ["path"] = path, ["before"] = Hash(path) }); originals[path] = Hash(path);
                }
                var journal = Path.Combine(data, "session-pending.json");
                var state = new JsonObject { ["schema"] = 1, ["backup"] = backup, ["run"] = null, ["workerId"] = int.MaxValue,
                    ["workerStartedUtc"] = "2026-10-26T20:39:58Z", ["mode"] = "Headset", ["runtime"] = "SteamVR" };
                switch (test)
                {
                    case "settings": File.AppendAllText(files[0]!["path"]!.GetValue<string>(), "changed"); break;
                    case "asset": File.AppendAllText(files[2]!["path"]!.GetValue<string>(), "changed"); break;
                    case "backup": File.AppendAllText(files[0]!["backup"]!.GetValue<string>(), "changed"); break;
                    case "executable": File.AppendAllText(Path.Combine(game, "grid2.exe"), "changed"); break;
                    case "worker": using (var current = Process.GetCurrentProcess()) { state["workerId"] = current.Id; state["workerStartedUtc"] = current.StartTime.ToUniversalTime().ToString("o"); } break;
                    case "run": state["run"] = Path.Combine(data, "diagnostic-20261026-214003-560"); break;
                    case "deployed": files[0]!["probeBefore"] = files[0]!["before"]!.DeepClone(); break;
                    case "theater": File.WriteAllText(Path.Combine(data, "steamvr-theater-pending.json"), "{}"); break;
                    case "foreign": File.WriteAllText(Path.Combine(game, "dxgi.dll"), "foreign proxy"); break;
                    case "outside": var outside = Path.Combine(fixture, "unowned.txt"); File.Copy(files[2]!["path"]!.GetValue<string>(), outside); files[2]!["path"] = outside; break;
                }
                File.WriteAllText(Path.Combine(backup, "manifest.json"), new JsonObject { ["files"] = files, ["executables"] = executables }.ToJsonString());
                File.WriteAllText(journal, test == "malformed" ? "{}" : state.ToJsonString());
                var beforeJournal = File.ReadAllBytes(journal);
                Check(SessionRecovery.TryClearUnstarted(root, documents) == (test is "unchanged" or "grouped"));
                if (test is "unchanged" or "grouped")
                {
                    Check(!File.Exists(journal));
                    Check(File.ReadAllBytes(Path.Combine(backup, "user-session-recovered-unchanged.json")).SequenceEqual(beforeJournal));
                    Check(originals.All(pair => Hash(pair.Key) == pair.Value));
                    Check(!SessionRecovery.TryClearUnstarted(root, documents));
                }
                else Check(File.ReadAllBytes(journal).SequenceEqual(beforeJournal));
            }
        }
        finally { Environment.SetEnvironmentVariable("GRID2VR_USER_BASE", previous); }
    }
}
