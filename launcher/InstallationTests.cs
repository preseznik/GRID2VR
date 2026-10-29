using System.Security.Cryptography;
using System.Text.Json;

namespace GRID2VR;
internal static class InstallationTests
{
    public static void Run(string fixture)
    {
        static void Check(bool ok) { if(!ok)throw new Exception("Installed package contract failed."); }
        static void Reject(Action action) { try{action();}catch(IOException){return;}throw new Exception("Invalid package accepted."); }
        var game=Path.Combine(fixture,"installed game");var root=Path.Combine(game,"GRID2VR");Directory.CreateDirectory(root);
        var before=Environment.GetEnvironmentVariable("GRID2VR_USER_BASE");Environment.SetEnvironmentVariable("GRID2VR_USER_BASE",Path.Combine(fixture,"user-data"));
        try {
            var file=Path.Combine(root,"test.dat");File.WriteAllText(file,"owned");
            var manifest=Path.Combine(root,"package.json");
            void Write(string key,string hash)=>File.WriteAllText(manifest,JsonSerializer.Serialize(new{schema=1,files=new Dictionary<string,string>{{key,hash}}}));
            var hash=Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(file)));Write("GRID2VR/test.dat",hash);
            Check(AppPaths.Game(root)==game);Check(SessionClient.FindRoot(game)==root);
            Check(!AppPaths.Data(root).StartsWith(game,StringComparison.OrdinalIgnoreCase));
            Check(AppPaths.Python(root)==Path.Combine(root,"payload","python","python.exe"));
            var verified = 0;
            AppPaths.VerifyPackage(root, (done, total) => { Check(done == total); verified = done; });
            Check(verified == 1);
            Check(PreparationProgress.Parse("GRID2VR_PROGRESS|28|Preparing cockpit cameras…") is { Percent: 28 });
            Check(PreparationProgress.Parse("GRID2VR_PROGRESS|28|Preparing cockpit camera 1 of 51: s20") is { Message: "Preparing cockpit camera 1 of 51: Honda S2000" });
            foreach (var invalid in new[] { "log output", "GRID2VR_PROGRESS|100|not complete yet", "GRID2VR_PROGRESS|-1|bad", "GRID2VR_PROGRESS|20|" })
                Check(PreparationProgress.Parse(invalid) is null);
            // Setup failures name the refusal and its remedy in the dialog players screenshot.
            const string steamGame = @"E:\SteamLibrary\steamapps\common\GRID 2", setupLog = @"C:\Users\Player\AppData\Local\GRID2VR\ID\setup.log";
            var camera = Installation.SetupFailure("Traceback (most recent call last):\r\n  File \"cockpit_assets.py\", line 60, in inputs\r\nValueError: Installed camera changed: s20\r\nGRID2VR_ERROR|Could not prepare cockpit-cameras; originals are unchanged.\r\n", setupLog, steamGame, true);
            Check(camera.Contains("Reason: Installed camera changed: Honda S2000 (s20)") && camera.Contains("Verify integrity of game files") && camera.EndsWith(setupLog));
            var missing = Installation.SetupFailure("\u001b[31;1mFileNotFoundError: [Errno 2] No such file or directory: 'E:\\\\SteamLibrary\\\\steamapps\\\\common\\\\GRID 2\\\\system\\\\flow.bin'\u001b[0m\r\nGRID2VR_ERROR|Could not prepare practice-flow-20261014; originals are unchanged.\r\n", setupLog, steamGame, false);
            Check(missing.Contains(@"Reason: [Errno 2] No such file or directory: 'E:\SteamLibrary\steamapps\common\GRID 2\system\flow.bin'") &&!missing.Contains("\u001b") && missing.Contains("Restore the original GRID 2 files") && !missing.Contains("In Steam"));
            var blocked = Installation.SetupFailure("GRID2VR_ERROR|The term 'E:\\SteamLibrary\\steamapps\\common\\GRID 2\\GRID2VR\\payload\\python\\python.exe' is not recognized as a name of a cmdlet, function, script file, or executable program.\r\n", setupLog, steamGame, true);
            Check(blocked.Contains("antivirus") && !blocked.Contains("Verify integrity"));
            var other = Installation.SetupFailure("GRID2VR_ERROR|Setup requires an installed package.\r\n", setupLog, steamGame, true);
            Check(other.Contains("Reason: Setup requires an installed package.") && !other.Contains("antivirus") && !other.Contains("Verify integrity"));
            Check(Installation.SetupFailure("", setupLog, steamGame, true).Contains("Reason: not reported."));
            File.WriteAllText(file,"changed");Reject(()=>AppPaths.VerifyPackage(root));File.WriteAllText(file,"owned");
            Write("../escaped",hash);Reject(()=>AppPaths.VerifyPackage(root));Write("GRID2VR/test.dat",hash);
            // First install imports preferences/profiles without embedding them in a release.
            var source=Path.Combine(fixture,"import");Directory.CreateDirectory(Path.Combine(source,"profiles","career"));
            File.WriteAllText(Path.Combine(source,"launcher-settings.json"),"{\"Car\":\"s20\"}");File.WriteAllText(Path.Combine(source,"profiles","career","save.dat"),"career bytes");
            Installation.MigrateData(root,source,managedProfilesSupported:true);
            Check(File.ReadAllText(Path.Combine(AppPaths.Data(root),"profiles","career","save.dat"))=="career bytes");
            Reject(()=>Installation.MigrateData(root,source,managedProfilesSupported:true));
            var standaloneRoot=Path.Combine(fixture,"standalone-import");
            Reject(()=>Installation.MigrateData(standaloneRoot,source,managedProfilesSupported:false));
            Check(!Directory.Exists(AppPaths.Data(standaloneRoot)));
            var preferences=Path.Combine(fixture,"preferences-only");Directory.CreateDirectory(preferences);
            File.WriteAllText(Path.Combine(preferences,"launcher-settings.json"),"{\"Car\":\"s20\"}");
            File.WriteAllText(Path.Combine(preferences,"profile-selection.json"),"{\"schema\":1,\"id\":\"current\"}");
            Reject(()=>Installation.MigrateData(standaloneRoot,preferences,managedProfilesSupported:false));
            Check(!Directory.Exists(AppPaths.Data(standaloneRoot)));
            File.Delete(Path.Combine(preferences,"profile-selection.json"));
            Installation.MigrateData(standaloneRoot,preferences,managedProfilesSupported:false);
            Check(File.ReadAllText(Path.Combine(AppPaths.Data(standaloneRoot),"launcher-settings.json"))=="{\"Car\":\"s20\"}");
            var pending=Path.Combine(AppPaths.Data(root),"session-pending.json");File.WriteAllText(pending,"{}");
            Check(Installation.HasPending(root));Reject(()=>Installation.RequireIdle(root));File.Delete(pending);
            var client=new SessionClient(root);
            // Full fixes are the installed default, independent of the developer preview checkbox.
            Directory.CreateDirectory(Path.GetDirectoryName(AppPaths.Shell(root))!);File.WriteAllText(AppPaths.Shell(root),"fixture");
            var command=client.LaunchCommand(new Choices{PreviewVrFixes=false},true);
            Check(command.ArgumentList.Contains("-PreviewVrFixes")&&command.FileName==AppPaths.Shell(root));
        } finally{Environment.SetEnvironmentVariable("GRID2VR_USER_BASE",before);}
    }
}
