using System.ComponentModel;
using System.Drawing.Drawing2D;

namespace GRID2VR;

// Port of the DiRT2VR mouse illustrations and keyboard-accessible mode picker.
internal sealed class MouseLookPicker : TableLayoutPanel
{
    private readonly List<MouseChoice> choices = [];
    public MouseLookPicker()
    {
        Name = "ChaseMouseMode"; AccessibleName = "Mouse look activation";
        AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink; ColumnCount = 3; RowCount = 1; Margin = Padding.Empty;
        foreach (var mode in new[] { "left", "right", "always" })
        {
            ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f / 3));
            var choice = new MouseChoice(mode) { Dock = DockStyle.Fill };
            choices.Add(choice); Controls.Add(choice);
        }
        SelectedMode = "right";
    }
    [DefaultValue("right")]
    public string SelectedMode
    {
        get => choices.Single(c => c.Checked).Mode;
        set => (choices.SingleOrDefault(c => c.Mode == value) ?? throw new ArgumentException("Unknown mouse look mode.")).Checked = true;
    }

    private sealed class MouseChoice : RadioButton
    {
        public string Mode { get; }
        public MouseChoice(string mode)
        {
            Mode = mode; Name = "ChaseMouse_" + mode; Text = mode == "always" ? "Always on" : "Hold " + mode;
            AccessibleName = "Mouse look: " + Text;
            AccessibleDescription = mode == "always" ? "Move the mouse without holding a button." : "Hold the " + mode + " mouse button while moving.";
            Appearance = Appearance.Button; FlatStyle = FlatStyle.Flat; TextImageRelation = TextImageRelation.ImageAboveText;
            TextAlign = ImageAlign = ContentAlignment.MiddleCenter; AutoSize = true; Margin = new Padding(2); Padding = new Padding(5);
            RefreshIcon();
        }
        private void RefreshIcon()
        {
            if (string.IsNullOrEmpty(Mode)) return;
            var accent = SystemInformation.HighContrast ? SystemColors.Highlight : LauncherAppearance.Accent;
            FlatAppearance.BorderColor = Checked ? accent : LauncherAppearance.Line; FlatAppearance.BorderSize = Checked ? 2 : 1;
            FlatAppearance.CheckedBackColor = BackColor;
            var bitmap = new Bitmap(LauncherAppearance.Px(this, 30), LauncherAppearance.Px(this, 34));
            using (var graphics = Graphics.FromImage(bitmap))
            using (var outline = new Pen(Enabled ? ForeColor : SystemColors.GrayText, 1.5f))
            using (var fill = new SolidBrush(Enabled ? accent : SystemColors.GrayText))
            {
                graphics.ScaleTransform(DeviceDpi / 96f, DeviceDpi / 96f); graphics.SmoothingMode = SmoothingMode.AntiAlias;
                using var body = new GraphicsPath();
                body.AddArc(3, 1, 24, 22, 180, 180); body.AddArc(3, 11, 24, 22, 0, 180); body.CloseFigure();
                var state = graphics.Save(); graphics.SetClip(body);
                if (Mode is "left" or "always") graphics.FillRectangle(fill, 3, 1, 12, 14);
                if (Mode is "right" or "always") graphics.FillRectangle(fill, 15, 1, 12, 14);
                graphics.Restore(state); graphics.DrawPath(outline, body);
                graphics.DrawLine(outline, 15, 1, 15, 15); graphics.DrawLine(outline, 3, 15, 27, 15); graphics.DrawRectangle(outline, 13, 6, 4, 6);
                if (Mode == "always")
                {
                    graphics.DrawLines(outline, new Point[] { new(10, 21), new(7, 24), new(10, 27) });
                    graphics.DrawLines(outline, new Point[] { new(20, 21), new(23, 24), new(20, 27) });
                }
            }
            var previous = Image; Image = bitmap; previous?.Dispose();
        }
        protected override void OnCheckedChanged(EventArgs e) { base.OnCheckedChanged(e); RefreshIcon(); }
        protected override void OnForeColorChanged(EventArgs e) { base.OnForeColorChanged(e); RefreshIcon(); }
        protected override void OnBackColorChanged(EventArgs e) { base.OnBackColorChanged(e); RefreshIcon(); }
        protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); RefreshIcon(); }
        protected override void OnDpiChangedAfterParent(EventArgs e) { base.OnDpiChangedAfterParent(e); RefreshIcon(); }
        protected override void Dispose(bool disposing)
        {
            if (disposing) { var previous = Image; Image = null; previous?.Dispose(); }
            base.Dispose(disposing);
        }
    }
}
