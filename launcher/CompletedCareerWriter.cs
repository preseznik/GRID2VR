using System.ComponentModel;
using System.Diagnostics;

namespace GRID2VR;

internal static class CompletedCareerWriter
{
    internal static Action<string, string>? ForInstallation(string root)
    {
        var scripts = Path.Combine(root, "tools", "career_profile");
        var runtime = Path.Combine(root, AppPaths.Packaged(root) ? "payload" : "build", "career-runtime");
        if (!File.Exists(Path.Combine(scripts, "create_completed.py")) ||
            !File.Exists(Path.Combine(runtime, "unicorn", "lib", "unicorn.dll")))
            return null;
        return (remote, name) => Run(AppPaths.Python(root), scripts, runtime, AppPaths.Game(root), remote, name);
    }

    static void Run(string python, string scripts, string runtime, string game, string remote, string name)
    {
        var executable = Path.Combine(game, "grid2_avx.exe");
        var fingerprint = GameExecutable.Validate(executable, true);
        var start = new ProcessStartInfo(python)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true,
            WorkingDirectory = scripts
        };
        foreach (var arg in new[] { "-I", "-S", "-B", "-c",
            "import sys; from pathlib import Path; sys.path[:0] = sys.argv[1:3]; " +
            "from create_completed import main; main(Path(sys.argv[3]), sys.argv[4], Path(sys.argv[5]), sys.argv[6])",
            scripts, runtime, remote, name, executable, fingerprint })
            start.ArgumentList.Add(arg);
        try
        {
            using var process = Process.Start(start) ?? throw new IOException("Could not start the career writer.");
            var error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(60000))
            {
                process.Kill();
                process.WaitForExit();
                throw new IOException("The career writer timed out.");
            }
            if (process.ExitCode != 0)
            {
                var detail = error.GetAwaiter().GetResult().Trim().Split('\n').LastOrDefault();
                throw new IOException($"Could not verify the personalized career. {detail}");
            }
        }
        catch (Win32Exception e) { throw new IOException("Could not run the completed-career writer.", e); }
    }
}
