using System.ComponentModel;
using System.Runtime.InteropServices;

namespace GRID2VR;

internal sealed class RenderScaleSlider : UserControl
{
    internal const int MinimumScale = 50, MaximumScale = 300;
    private readonly MarkerTrackBar track = new() { Name = "RenderScaleSlider", AccessibleName = "Render scale",
        Minimum = MinimumScale, Maximum = MaximumScale, Value = 100, SmallChange = 1, LargeChange = 5,
        TickStyle = TickStyle.None, Dock = DockStyle.Fill, AutoSize = false, Margin = Padding.Empty };
    private readonly Label valueText = new() { Name = "RenderScaleValue", AutoSize = true, Anchor = AnchorStyles.Right, Margin = new Padding(8, 0, 0, 0) };
    private readonly string markerCaption = "";
    public event EventHandler? ValueChanged;

    public RenderScaleSlider() : this(MinimumScale, MaximumScale) { }
    // An optional marker labels one value under the track, as 0.1.x's
    // recommended render scale did (e.g. a suggested minimum).
    public RenderScaleSlider(int minimum, int maximum, int? marker = null, string markerCaption = "")
    {
        track.Minimum = minimum; track.Maximum = maximum; track.Marker = marker; this.markerCaption = markerCaption;
        if (marker is not null) { track.TickStyle = TickStyle.BottomRight; track.TickFrequency = maximum - minimum; }
        var height = marker is null ? 42 : 62;
        Name = "RenderScale"; Dock = DockStyle.Top; Height = height; Width = 280; MinimumSize = new Size(150, height); TabStop = false;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = marker is null ? 1 : 2, Margin = Padding.Empty };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100)); layout.ColumnStyles.Add(new(SizeType.AutoSize));
        layout.RowStyles.Add(new(SizeType.Percent, 100));
        layout.Controls.Add(track, 0, 0); layout.Controls.Add(valueText, 1, 0);
        if (marker is not null)
        {
            layout.RowStyles.Add(new(SizeType.AutoSize));
            var caption = new MarkerLabel(track, markerCaption) { Name = "RenderScaleMarker", Tag = "hint",
                Height = 22, Dock = DockStyle.Fill, Margin = Padding.Empty };
            layout.Controls.Add(caption, 0, 1); layout.SetColumnSpan(caption, 2);
        }
        Controls.Add(layout);
        track.ValueChanged += (_, _) => { RefreshValue(); ValueChanged?.Invoke(this, EventArgs.Empty); };
        RefreshValue();
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int Value { get => track.Value; set => track.Value = value; }
    internal bool HasMarker => track.Marker is not null;
    private void RefreshValue()
    {
        valueText.Text = $"{Value}%";
        track.AccessibleDescription = AccessibleDescription = HasMarker ? $"{Value}%; {markerCaption}" : $"{Value}%";
    }

    private sealed class MarkerLabel(MarkerTrackBar track, string caption) : Label
    {
        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.Clear(BackColor);
            var colour = Enabled ? ForeColor : SystemColors.GrayText;
            var arrowWidth = TextRenderer.MeasureText("▲", Font).Width;
            var x = track.MarkerPixel;
            TextRenderer.DrawText(e.Graphics, "▲", Font, new Point(x - arrowWidth / 2, 0), colour);
            var size = TextRenderer.MeasureText(caption, Font);
            var captionX = x + arrowWidth / 2;
            if (captionX + size.Width > Width) captionX = Math.Max(0, x - arrowWidth / 2 - size.Width);
            TextRenderer.DrawText(e.Graphics, caption, Font, new Point(captionX, 0), colour);
        }
    }

    // Draws one tick at Marker (TBM_SETTIC) and reports where the track put it.
    private sealed class MarkerTrackBar : TrackBar
    {
        [DllImport("user32.dll", EntryPoint = "SendMessageW")]
        private static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        internal int? Marker { get; set; }
        internal int MarkerPixel => IsHandleCreated ? SendMessage(Handle, 0x40F, IntPtr.Zero, IntPtr.Zero).ToInt32() : // TBM_GETTICPOS
            8 + (Width - 16) * ((Marker ?? Minimum) - Minimum) / Math.Max(1, Maximum - Minimum);
        protected override void OnHandleCreated(EventArgs e) { base.OnHandleCreated(e); RestoreMarker(); }
        protected override void OnSizeChanged(EventArgs e) { base.OnSizeChanged(e); RestoreMarker(); }
        private void RestoreMarker()
        {
            if (!IsHandleCreated || Marker is not { } marker) return;
            SendMessage(Handle, 0x409, IntPtr.Zero, IntPtr.Zero); // TBM_CLEARTICS
            SendMessage(Handle, 0x404, IntPtr.Zero, new IntPtr(marker)); // TBM_SETTIC
            Invalidate();
        }
    }
}
