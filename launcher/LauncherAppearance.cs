using System.Drawing.Drawing2D;
using System.Runtime.CompilerServices;
using Microsoft.Win32;

namespace GRID2VR;

// Adapted from DiRT2VR's LauncherAppearance: preserve controls and handlers when
// switching appearance, follow Windows colours, and use GRID2VR red accents.
internal static class LauncherAppearance
{
    public static bool Modern { get; set; }
    public static bool Dark => Application.IsDarkModeEnabled;
    public static Color Surface => Dark ? Color.FromArgb(27, 26, 29) : Color.FromArgb(249, 247, 247);
    public static Color Cell => Dark ? Color.FromArgb(40, 38, 42) : Color.White;
    public static Color Ink => Dark ? Color.FromArgb(242, 238, 239) : Color.FromArgb(36, 30, 32);
    public static Color Line => Dark ? Color.FromArgb(79, 71, 76) : Color.FromArgb(198, 184, 188);
    public static Color Accent => AccentFor(Dark);
    internal static Color AccentFor(bool dark) => dark ? Color.FromArgb(255, 119, 115) : Color.FromArgb(179, 34, 34);
    public static int Px(Control control, int value) => (int)Math.Round(value * control.DeviceDpi / 96.0);

    private sealed class OriginalStyle(Control control)
    {
        public Padding Padding { get; } = control.Padding;
        public Padding Margin { get; } = control.Margin;
        public FlatStyle Flat { get; } = control is Button button ? button.FlatStyle : FlatStyle.Standard;
        public bool Visual { get; } = control is Button button && button.UseVisualStyleBackColor;
    }
    private static readonly ConditionalWeakTable<Control, OriginalStyle> Originals = new();

    public static void Apply(Control root)
    {
        var original = Originals.GetValue(root, c => new(c));
        var styled = Modern && !SystemInformation.HighContrast;
        var input = (root is TextBoxBase or ListControl or NumericUpDown) && root is not SelectableStatusText;
        root.SuspendLayout();
        try
        {
            root.BackColor = styled ? (input ? Cell : Surface) : Dark && !SystemInformation.HighContrast ? Color.FromArgb(32, 32, 32) : input ? SystemColors.Window : SystemColors.Control;
            root.ForeColor = styled ? Ink : Dark && !SystemInformation.HighContrast ? Color.White : input ? SystemColors.WindowText : SystemColors.ControlText;
            if (root.Tag is "hint" or "section" && styled) root.ForeColor = Accent;
            if (root.Tag is "setting-row") root.Margin = styled ? new Padding(0, 0, 0, Px(root, 6)) : original.Margin;
            // Section headers keep their borderless heading look in both themes.
            if (root is Button button && root.Tag is not "section")
            {
                button.FlatStyle = styled ? FlatStyle.Flat : original.Flat;
                button.Padding = styled && button.Name != "HelpAbout" ? new Padding(Px(root, 9), Px(root, 3), Px(root, 9), Px(root, 3)) : original.Padding;
                button.UseVisualStyleBackColor = !styled && original.Visual;
                if (styled)
                {
                    button.BackColor = Cell;
                    button.FlatAppearance.BorderColor = button.Name == "SaveSettings" ? Accent : Line;
                    button.FlatAppearance.MouseOverBackColor = Dark ? Color.FromArgb(67, 43, 47) : Color.FromArgb(253, 231, 230);
                    button.FlatAppearance.MouseDownBackColor = Dark ? Color.FromArgb(89, 42, 48) : Color.FromArgb(249, 211, 209);
                    if (button.Name == "LaunchVR")
                    {
                        button.BackColor = Color.FromArgb(198, 40, 40); button.ForeColor = Color.White;
                        button.FlatAppearance.MouseOverBackColor = Color.FromArgb(170, 30, 30);
                        button.FlatAppearance.MouseDownBackColor = Color.FromArgb(140, 24, 24);
                    }
                }
            }
            if (root is LinkLabel link)
            {
                link.LinkColor = styled ? Accent : SystemColors.HotTrack;
                link.ActiveLinkColor = link.LinkColor; link.VisitedLinkColor = link.LinkColor;
            }
            if (root is ModernTabs tabs) tabs.UseModernStyle = styled;
            foreach (Control child in root.Controls) Apply(child);
        }
        finally { root.ResumeLayout(true); }
        root.Invalidate();
    }
}

internal class LauncherForm : Form
{
    private readonly SettingsWheelGuard wheelGuard;
    public LauncherForm() { Icon = Branding.LoadIcon(); wheelGuard = new SettingsWheelGuard(this); }
    protected override void OnLoad(EventArgs e)
    {
        LauncherAppearance.Apply(this);
        SystemEvents.UserPreferenceChanged += PreferencesChanged;
        base.OnLoad(e);
    }
    private void PreferencesChanged(object sender, UserPreferenceChangedEventArgs e)
    {
        if (e.Category is not (UserPreferenceCategory.Color or UserPreferenceCategory.VisualStyle or UserPreferenceCategory.Accessibility or UserPreferenceCategory.General) || IsDisposed || !IsHandleCreated) return;
        try { BeginInvoke(() => { if (!IsDisposed) LauncherAppearance.Apply(this); }); }
        catch (InvalidOperationException) { /* Window closed during the notification. */ }
    }
    protected override void Dispose(bool disposing)
    {
        if (disposing) { wheelGuard.Dispose(); SystemEvents.UserPreferenceChanged -= PreferencesChanged; Icon?.Dispose(); }
        base.Dispose(disposing);
    }
}

// Real CheckBox semantics keep keyboard focus, Space and accessibility working.
internal sealed class AppearanceSwitch : CheckBox
{
    public AppearanceSwitch()
    {
        Name = "ModernInterface"; AccessibleName = "Modern interface";
        AutoSize = false; Size = new Size(225, 34);
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        UpdateText();
    }
    protected override void OnCheckedChanged(EventArgs e) { UpdateText(); base.OnCheckedChanged(e); Invalidate(); }
    private void UpdateText() => Text = Checked ? "On · Modern" : "Off · Classic";
    public override Size GetPreferredSize(Size proposedSize) => new(LauncherAppearance.Px(this, 225), LauncherAppearance.Px(this, 34));
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(BackColor); e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        var height = LauncherAppearance.Px(this, 22); var width = LauncherAppearance.Px(this, 42); var y = (Height - height) / 2;
        var colour = !Enabled ? SystemColors.GrayText : SystemInformation.HighContrast ? (Checked ? SystemColors.Highlight : SystemColors.GrayText) : Checked ? LauncherAppearance.Accent : LauncherAppearance.Line;
        using var path = new GraphicsPath(); using var brush = new SolidBrush(colour);
        path.AddArc(1, y, height, height, 90, 180); path.AddArc(width - height, y, height, height, 270, 180); path.CloseFigure();
        e.Graphics.FillPath(brush, path);
        var inset = LauncherAppearance.Px(this, 3); var diameter = height - inset * 2;
        using var thumb = new SolidBrush(SystemInformation.HighContrast ? SystemColors.HighlightText : Color.White);
        e.Graphics.FillEllipse(thumb, Checked ? width - inset - diameter : inset, y + inset, diameter, diameter);
        var gap = LauncherAppearance.Px(this, 10);
        TextRenderer.DrawText(e.Graphics, Text, Font, new Rectangle(width + gap, 0, Math.Max(1, Width - width - gap), Height), Enabled ? ForeColor : SystemColors.GrayText, TextFormatFlags.Left | TextFormatFlags.VerticalCenter);
        if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(e.Graphics, ClientRectangle, ForeColor, BackColor);
    }
}

internal sealed class ModernTabs : TabControl
{
    private bool modern;
    [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    public bool UseModernStyle
    {
        get => modern;
        set
        {
            if (modern == value) return;
            modern = value;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, value);
            DrawMode = value ? TabDrawMode.OwnerDrawFixed : TabDrawMode.Normal;
            Padding = new Point(LauncherAppearance.Px(this, value ? 14 : 6), LauncherAppearance.Px(this, value ? 9 : 3));
            Invalidate();
        }
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        if (!modern) { base.OnPaint(e); return; }
        e.Graphics.Clear(LauncherAppearance.Surface);
        for (var index = 0; index < TabCount; ++index)
            OnDrawItem(new DrawItemEventArgs(e.Graphics, Font, GetTabRect(index), index, index == SelectedIndex && Focused ? DrawItemState.Focus : DrawItemState.None));
        using var pen = new Pen(LauncherAppearance.Line);
        e.Graphics.DrawRectangle(pen, 0, DisplayRectangle.Top - 1, Math.Max(1, Width - 1), Math.Max(1, Height - DisplayRectangle.Top));
    }
    protected override void OnDrawItem(DrawItemEventArgs e)
    {
        if (!modern || e.Index < 0) { base.OnDrawItem(e); return; }
        var selected = e.Index == SelectedIndex;
        using var brush = new SolidBrush(LauncherAppearance.Surface); e.Graphics.FillRectangle(brush, e.Bounds);
        TextRenderer.DrawText(e.Graphics, TabPages[e.Index].Text, Font, e.Bounds, selected ? LauncherAppearance.Accent : LauncherAppearance.Ink, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        if (selected)
        {
            using var pen = new Pen(LauncherAppearance.Accent, LauncherAppearance.Px(this, 3));
            e.Graphics.DrawLine(pen, e.Bounds.Left + 6, e.Bounds.Bottom - 2, e.Bounds.Right - 6, e.Bounds.Bottom - 2);
        }
        if (Focused && selected) e.DrawFocusRectangle();
    }
}
