namespace GRID2VR;

// Same two launch actions on the left and utility actions on the right as
// DiRT2VR/launcher/ResponsiveLayout.vb. Wrap utilities below on narrow windows.
internal sealed class LauncherFooter : Panel
{
    private readonly Button launch, vr, save, recover, logs;
    private bool arranging;
    public LauncherFooter(Button desktop, Button headset, Button settings, Button restore, Button openLogs)
    {
        launch = desktop; vr = headset; save = settings; recover = restore; logs = openLogs;
        Dock = DockStyle.Fill; Margin = new Padding(0, 12, 0, 0);
        Controls.AddRange([launch, vr, save, recover, logs]);
    }
    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        if (arranging || launch == null) return;
        arranging = true;
        try
        {
            var gap = (int)Math.Round(8 * DeviceDpi / 96.0);
            foreach (var button in Controls.OfType<Button>()) button.Size = button.GetPreferredSize(Size.Empty);
            var row = Controls.OfType<Button>().Max(b => b.Height);
            var compact = Width < launch.Width + vr.Width + save.Width + recover.Width + logs.Width + gap * 5;
            launch.Location = Point.Empty; vr.Location = new Point(launch.Right + gap, 0);
            logs.Location = new Point(Width - logs.Width, compact ? row + gap : 0);
            recover.Location = new Point(logs.Left - gap - recover.Width, logs.Top);
            save.Location = new Point(compact ? Width - save.Width : recover.Left - gap - save.Width, 0);
            Height = row * (compact ? 2 : 1) + (compact ? gap : 0) + gap / 2;
        }
        finally { arranging = false; }
    }
    public override Size GetPreferredSize(Size proposedSize) => new(Width, Height);
}
