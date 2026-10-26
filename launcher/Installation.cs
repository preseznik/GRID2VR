using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace GRID2VR;

internal static class Installation
{
    internal const string MutexName = @"Global\GRID2VR.Session";
    internal static bool HasPending(string root) => new[]{"session-pending.json","diagnostic-pending.json","practice-flow-pending.json","steamvr-theater-pending.json"}.Any(name=>File.Exists(Path.Combine(AppPaths.Data(root),name)));
    internal static void ValidateGame(string game)
    {
        Updates.PlainPath(game);
        foreach (var name in new[] { "grid2.exe", "grid2_avx.exe" })
        {
            var path=Path.Combine(game,name); Updates.PlainPath(path);
            GameExecutable.Validate(path, name == "grid2_avx.exe");
        }
    }
    internal static void RequireIdle(string root, bool allowPending = false)
    {
        using var mutex=new Mutex(false,MutexName); bool acquired=false;
        try {
            try { acquired=mutex.WaitOne(0); } catch(AbandonedMutexException) { acquired=true; }
            if(!acquired) throw new IOException("Finish the GRID2VR session before installing or removing files.");
            foreach(var name in new[]{"grid2","grid2_avx"})
                foreach(var process in Process.GetProcessesByName(name)) using(process) throw new IOException("Close GRID 2 first.");
            foreach(var name in new[]{"session-pending.json","diagnostic-pending.json","practice-flow-pending.json","steamvr-theater-pending.json"})
                if(!allowPending && File.Exists(Path.Combine(AppPaths.Data(root),name))) throw new IOException("Use Restore original files in GRID2VR before upgrading or uninstalling.");
        } finally { if(acquired)mutex.ReleaseMutex(); }
    }
    internal static void CheckInstall(string game)
    {
        game=Path.GetFullPath(game).TrimEnd(Path.DirectorySeparatorChar); ValidateGame(game);
        var root=Path.Combine(game,"GRID2VR"); RequireIdle(root, allowPending:true);
        foreach(var process in Process.GetProcessesByName("GRID2VR"))
            using(process) if(process.Id!=Environment.ProcessId && string.Equals(process.MainModule?.FileName,Path.Combine(game,"GRID2VR.exe"),StringComparison.OrdinalIgnoreCase)) throw new IOException("Close the installed GRID2VR launcher before upgrading or uninstalling.");
        foreach(var name in new[]{"d3d11.dll","dxgi.dll","GRID2VR.diagnostic-session","GRID2VR.session.xml"})
            if(File.Exists(Path.Combine(game,name))) throw new IOException("Existing "+name+" was preserved. Restore the active session or remove the other mod first.");
        if(Directory.Exists(root) && Directory.EnumerateFileSystemEntries(root).Any() && !AppPaths.Packaged(root)) throw new IOException("The existing GRID2VR folder has no ownership manifest; it was preserved.");
        if(File.Exists(Path.Combine(game,"GRID2VR.exe")) && !AppPaths.Packaged(root)) throw new IOException("Existing GRID2VR.exe has no package ownership manifest.");
        if(AppPaths.Packaged(root)) AppPaths.VerifyPackage(root);
        SessionRecovery.TryClearUnstarted(root);
        RequireIdle(root);
    }
    internal static string? Discover()
    {
        var steam=ProfileService.SteamRoot(); var libraries=new HashSet<string>(StringComparer.OrdinalIgnoreCase){steam};
        var vdf=Path.Combine(steam,"steamapps","libraryfolders.vdf");
        if(File.Exists(vdf) && new FileInfo(vdf).Length<1024*1024)
            foreach(Match m in Regex.Matches(File.ReadAllText(vdf),"\"path\"\\s*\"([^\"]+)\"")) libraries.Add(m.Groups[1].Value.Replace("\\\\","\\"));
        foreach(var library in libraries)
        {
            var manifest=Path.Combine(library,"steamapps","appmanifest_44350.acf");
            if(!File.Exists(manifest) || new FileInfo(manifest).Length>65536)continue;
            var match=Regex.Match(File.ReadAllText(manifest),"\"installdir\"\\s*\"([^\"]+)\"");
            if(!match.Success || Path.GetFileName(match.Groups[1].Value)!=match.Groups[1].Value)continue;
            var game=Path.Combine(library,"steamapps","common",match.Groups[1].Value);
            if(File.Exists(Path.Combine(game,"grid2_avx.exe")))return game;
        }
        return null;
    }
    internal static async Task Setup(string root, IProgress<PreparationProgress>? progress = null)
    {
        await Task.Run(() =>
        {
            AppPaths.VerifyPackage(root, (done, total) => progress?.Report(new(15 * done / total, $"Checking installed files ({done:N0} of {total:N0})…")));
            progress?.Report(new(15, "Checking GRID 2 installation…"));
            ValidateGame(AppPaths.Game(root)); RequireIdle(root);
        });
        var command=new ProcessStartInfo(AppPaths.Shell(root)){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,WorkingDirectory=root};
        foreach(var arg in new[]{"-NoProfile","-NonInteractive","-ExecutionPolicy","Bypass","-File",Path.Combine(root,"tools","Setup-Package.ps1")})command.ArgumentList.Add(arg);
        using var process=Process.Start(command)??throw new IOException("Setup could not start.");
        var data=AppPaths.Data(root); Directory.CreateDirectory(data);
        using var log = new StreamWriter(Path.Combine(data, "setup.log"), false) { AutoFlush = true };
        var errors = process.StandardError.ReadToEndAsync();
        while (await process.StandardOutput.ReadLineAsync() is { } line)
        {
            await log.WriteLineAsync(line);
            if (PreparationProgress.Parse(line) is { } update) progress?.Report(update);
        }
        await process.WaitForExitAsync(); await log.WriteAsync(await errors);
        if(process.ExitCode!=0)throw new IOException("Local asset preparation failed. See "+Path.Combine(data,"setup.log"));
        progress?.Report(new(100, "Preparation complete."));
    }
    internal static void Migrate(string root,string source)
    {
        RequireIdle(root); var data=AppPaths.Data(root); Updates.PlainPath(source);Updates.PlainPath(data);
        if(new[]{"session-pending.json","diagnostic-pending.json","practice-flow-pending.json","steamvr-theater-pending.json"}.Any(name=>File.Exists(Path.Combine(source,name))))throw new IOException("Recover the source session before importing its data.");
        var copies=new List<(string source,string destination)>();long total=0;
        // Explicit import only. Never bake this developer's preferences or careers into a release.
        foreach(var name in new[]{"launcher-settings.json","launcher-window.json","driving-controls.json","seat-positions.json","update-preferences.json","profile-selection.json","profiles"})
        {
            var from=Path.Combine(source,name);var to=Path.Combine(data,name);
            if(!File.Exists(from)&&!Directory.Exists(from))continue;
            if(File.Exists(to)||Directory.Exists(to))throw new IOException("Import would replace existing "+name+"; it was preserved.");
            Updates.PlainPath(from);
            var files=File.Exists(from)?new[]{from}:UnlinkedFiles(from).ToArray();
            foreach(var file in files) { Updates.PlainPath(file);total+=new FileInfo(file).Length;if(total>512L*1024*1024)throw new IOException("Profile import exceeds 512 MiB.");copies.Add((file,Path.Combine(data,Path.GetRelativePath(source,file)))); }
        }
        Directory.CreateDirectory(data);
        foreach(var item in copies)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(item.destination)!);File.Copy(item.source,item.destination,false);
        }
    }
    private static IEnumerable<string> UnlinkedFiles(string directory)
    {
        foreach(var entry in Directory.EnumerateFileSystemEntries(directory))
        {
            // Reject junctions before traversal; a skipped subtree could silently omit a career.
            Updates.PlainPath(entry);
            if(Directory.Exists(entry)) foreach(var file in UnlinkedFiles(entry)) yield return file;
            else yield return entry;
        }
    }
    internal static int Command(string[] args)
    {
        string? Value(string key) { var i=Array.IndexOf(args,key);return i>=0&&i+1<args.Length?args[i+1]:null; }
        try {
            if(args[0]=="--discover") { File.WriteAllText(args[1],Discover()??"");return 0; }
            var game=Value("--game")??AppPaths.Game(SessionClient.FindRoot(AppContext.BaseDirectory));
            var root=Path.Combine(Path.GetFullPath(game),"GRID2VR");
            switch(args[0]) {
                case "--check-install": CheckInstall(game);break;
                case "--check-uninstall": CheckInstall(game);break;
                case "--setup": Setup(root).GetAwaiter().GetResult();break;
                case "--verify-package": AppPaths.VerifyPackage(root);break;
                case "--migrate-from": Migrate(root,args[1]);break;
                default:return 1;
            }
            return 0;
        } catch(Exception error) { Console.Error.WriteLine(error.Message);var log=Value("--error-log");if(log is not null)File.WriteAllText(log,error.ToString());return 1; }
    }
}
