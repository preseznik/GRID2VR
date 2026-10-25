using System.ComponentModel;
using System.Runtime.InteropServices;

namespace GRID2VR;

// Match DiRT2VR's native ValueSlider: precise arrows, live value and a fixed recommendation.
internal sealed class RenderScaleSlider : UserControl
{
    internal const int MinimumScale = 50, MaximumScale = 300, RecommendedScale = 150;
    private readonly MarkerTrackBar track = new() { Name = "RenderScaleSlider", AccessibleName = "Render resolution", AccessibleDescription = "150% recommended",
        Minimum = MinimumScale, Maximum = MaximumScale, Value = 100, SmallChange = 1, LargeChange = 5,
        TickStyle = TickStyle.BottomRight, TickFrequency = MaximumScale - MinimumScale, Dock = DockStyle.Fill, AutoSize = false, Margin = Padding.Empty };
    private readonly Label valueText = new() { Name = "RenderScaleValue", AutoSize = true, Anchor = AnchorStyles.Right, Margin = new Padding(8, 0, 0, 0) };
    public event EventHandler? ValueChanged;

    public RenderScaleSlider()
    {
        Name = "RenderScale"; Dock = DockStyle.Top; Height = 62; Width = 280; MinimumSize = new Size(150, 62); TabStop = false;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 2, Margin = Padding.Empty };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100)); layout.ColumnStyles.Add(new(SizeType.AutoSize));
        layout.RowStyles.Add(new(SizeType.Percent, 100)); layout.RowStyles.Add(new(SizeType.AutoSize));
        layout.Controls.Add(track, 0, 0); layout.Controls.Add(valueText, 1, 0);
        var recommendation = new RecommendationLabel(track) { Name = "RenderScaleRecommendation", Text = "✓ 150% recommended", Tag = "recommendation",
            Height = 22, Dock = DockStyle.Fill, ForeColor = LauncherAppearance.Accent, Margin = Padding.Empty };
        layout.Controls.Add(recommendation, 0, 1); layout.SetColumnSpan(recommendation, 2);
        Controls.Add(layout);
        track.ValueChanged += (_, _) => { RefreshValue(); ValueChanged?.Invoke(this, EventArgs.Empty); };
        RefreshValue();
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int Value { get => track.Value; set => track.Value = value; }
    private void RefreshValue() { valueText.Text = $"{Value}%"; AccessibleDescription = $"{Value}%; {RecommendedScale}% recommended"; }

    private sealed class RecommendationLabel(MarkerTrackBar track) : Label
    {
        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.Clear(BackColor);
            var colour = Enabled ? ForeColor : SystemColors.GrayText;
            var checkWidth = TextRenderer.MeasureText("✓", Font).Width;
            var x = track.MarkerPixel;
            TextRenderer.DrawText(e.Graphics, "✓", Font, new Point(x - checkWidth / 2, 0), colour);
            const string caption = "150% recommended";
            var size = TextRenderer.MeasureText(caption, Font);
            var captionX = x + checkWidth / 2;
            if (captionX + size.Width > Width) captionX = Math.Max(0, x - checkWidth / 2 - size.Width);
            TextRenderer.DrawText(e.Graphics, caption, Font, new Point(captionX, 0), colour);
        }
    }

    private sealed class MarkerTrackBar : TrackBar
    {
        [DllImport("user32.dll", EntryPoint = "SendMessageW")]
        private static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        internal int MarkerPixel => IsHandleCreated ? SendMessage(Handle, 0x40F, IntPtr.Zero, IntPtr.Zero).ToInt32() :
            8 + (Width - 16) * (RecommendedScale - MinimumScale) / (MaximumScale - MinimumScale); // TBM_GETTICPOS
        protected override void OnHandleCreated(EventArgs e) { base.OnHandleCreated(e); RestoreMarker(); }
        protected override void OnSizeChanged(EventArgs e) { base.OnSizeChanged(e); RestoreMarker(); }
        private void RestoreMarker()
        {
            if (!IsHandleCreated) return;
            SendMessage(Handle, 0x409, IntPtr.Zero, IntPtr.Zero); // TBM_CLEARTICS
            SendMessage(Handle, 0x404, IntPtr.Zero, new IntPtr(RecommendedScale)); // TBM_SETTIC
            Invalidate();
        }
    }
}
