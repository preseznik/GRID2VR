using System.Security.Cryptography;
using System.Text.Json;

namespace GRID2VR;

internal static class PreparationCacheTests
{
    internal static void Run(string fixture)
    {
        static void Check(bool value) { if (!value) throw new Exception("Preparation cache check failed."); }
        var game = Path.Combine(fixture, "preparation game");
        var root = Path.Combine(game, "GRID2VR");
        Directory.CreateDirectory(root);
        var previous = Environment.GetEnvironmentVariable("GRID2VR_USER_BASE");
        Environment.SetEnvironmentVariable("GRID2VR_USER_BASE", Path.Combine(fixture, "preparation data"));
        try
        {
            string Write(string path, string value)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, value); return path;
            }
            var payload = Write(Path.Combine(root, "payload.dat"), "payload");
            var catalog = Write(Path.Combine(root, "tools", "data", "cockpit-assets.json"), "{\"cars\":[{\"car\":\"z06\"}]}");
            var manifest = Path.Combine(root, "package.json");
            var hashes = new Dictionary<string, string>();
            foreach (var path in new[] { payload, catalog })
                hashes[Path.GetRelativePath(game, path)] = Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)));
            Write(manifest, JsonSerializer.Serialize(new { schema = 1, files = hashes }));
            foreach (var name in new[] { "grid2.exe", "grid2_avx.exe", "system/flow.bin", "system/states.bin", "cutscene/intro_buildup.xml", "cutscene/startsequence.xml", "cars/models/z06/cameras.xml" })
                Write(Path.Combine(game, name), "original");
            var data = AppPaths.Data(root);
            foreach (var bank in PreparationCache.Banks) Write(Path.Combine(data, bank, "asset.bin"), "prepared");
            var receipt = Path.Combine(data, "preparation-cache.json");
            void Remember() { AppPaths.VerifyPackage(root); PreparationCache.SaveVerified(root); Check(PreparationCache.IsCurrent(root)); }
            void ChangedFile(string path)
            {
                var original = File.ReadAllBytes(path);
                File.AppendAllText(path, "changed"); Check(!PreparationCache.IsCurrent(root));
                File.WriteAllBytes(path, original); Remember();
            }
            Check(!PreparationCache.IsCurrent(root)); Remember();
            var runtime = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
            try
            {
                Environment.SetEnvironmentVariable("XR_RUNTIME_JSON", "different-headset-runtime.json");
                Check(PreparationCache.IsCurrent(root));
            }
            finally { Environment.SetEnvironmentVariable("XR_RUNTIME_JSON", runtime); }
            // Only successful setup can write a reusable receipt. A quick lookup
            // does not create one or bless changed package contents.
            ChangedFile(payload);
            ChangedFile(Path.Combine(game, "grid2_avx.exe"));
            ChangedFile(Path.Combine(game, "cars", "models", "z06", "cameras.xml"));
            ChangedFile(Path.Combine(data, PreparationCache.Banks[0], "asset.bin"));
            var bankFile = Path.Combine(data, PreparationCache.Banks[0], "asset.bin");
            File.Delete(bankFile); Check(!PreparationCache.IsCurrent(root));
            Directory.Delete(Path.GetDirectoryName(bankFile)!); Check(!PreparationCache.IsCurrent(root));
            Write(bankFile, "prepared"); Remember();
            var extra = Write(Path.Combine(data, PreparationCache.Banks[0], "new.bin"), "new");
            Check(!PreparationCache.IsCurrent(root)); File.Delete(extra); Remember();
            var seats = Write(Path.Combine(data, "seat-positions.json"), "{}");
            Check(!PreparationCache.IsCurrent(root)); File.Delete(seats); Remember();
            foreach (var name in new[] { "session-pending.json", "diagnostic-pending.json", "practice-flow-pending.json", "steamvr-theater-pending.json" })
            {
                var pending = Write(Path.Combine(data, name), "{}");
                Check(!PreparationCache.IsCurrent(root)); File.Delete(pending); Check(PreparationCache.IsCurrent(root));
            }
            var originalManifest = File.ReadAllText(manifest);
            Write(manifest, "{}"); Check(!PreparationCache.IsCurrent(root));
            Write(manifest, originalManifest); Remember();
            foreach (var invalid in new[] { "not json", "null", "{}", "{\"Schema\":999,\"Key\":\"old\"}", new string('x', 1025) })
            {
                Write(receipt, invalid); Check(!PreparationCache.IsCurrent(root)); Remember();
            }
            // The explicit full verification command still hashes contents even
            // when a same-size edit preserves metadata. This cache is freshness.
            var modifiedAt = File.GetLastWriteTimeUtc(payload);
            Write(payload, "damaged"); File.SetLastWriteTimeUtc(payload, modifiedAt);
            var rejected = false;
            try { AppPaths.VerifyPackage(root); } catch (IOException) { rejected = true; }
            Check(rejected); Write(payload, "payload"); Remember();
        }
        finally { Environment.SetEnvironmentVariable("GRID2VR_USER_BASE", previous); }
    }
}
