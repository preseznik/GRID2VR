namespace GRID2VR;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (args is ["--self-test"]) return SelfTests.Run();
        if (args.Length > 0 && args[0] is "--discover" or "--check-install" or "--check-uninstall" or "--setup" or "--verify-package" or "--migrate-from") return Installation.Command(args);
        if (args is ["--borderless-desktop", var desktopRun, var desktopPid, var desktopTicks])
        {
            try
            {
                Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
                return DesktopWindow.Watch(SessionClient.FindRoot(AppContext.BaseDirectory), desktopRun, int.Parse(desktopPid), long.Parse(desktopTicks));
            }
            catch { return 1; }
        }
        if (args is ["--watch-recenter", var run, var binding, var pid, var ticks])
        {
            try { return DeviceRecenter.Watch(SessionClient.FindRoot(AppContext.BaseDirectory), run, binding, int.Parse(pid), long.Parse(ticks)); }
            catch { return 1; } // Hidden helper must never interrupt the game with a dialog.
        }
        if (args.Length != 0) return 1;
        ApplicationConfiguration.Initialize();
        Application.SetColorMode(SystemColorMode.System);
        try
        {
            var root = SessionClient.FindRoot(AppContext.BaseDirectory);
            if (AppPaths.Packaged(root))
            {
                using var preparing = new PreparationForm(root);
                Application.Run(preparing);
                if (preparing.Failure is not null) throw preparing.Failure;
            }
            Application.Run(new MainForm(new SessionClient(root)));
            return 0;
        }
        catch (Exception error)
        {
            MessageBox.Show(error.Message, "GRID2VR", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
