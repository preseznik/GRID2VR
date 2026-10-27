using System.ComponentModel;

namespace GRID2VR;

internal sealed class RenderScaleSlider : UserControl
{
    internal const int MinimumScale = 50, MaximumScale = 300;
    private readonly TrackBar track = new() { Name = "RenderScaleSlider", AccessibleName = "Render scale",
        Minimum = MinimumScale, Maximum = MaximumScale, Value = 100, SmallChange = 1, LargeChange = 5,
        TickStyle = TickStyle.None, Dock = DockStyle.Fill, AutoSize = false, Margin = Padding.Empty };
    private readonly Label valueText = new() { Name = "RenderScaleValue", AutoSize = true, Anchor = AnchorStyles.Right, Margin = new Padding(8, 0, 0, 0) };
    public event EventHandler? ValueChanged;

    public RenderScaleSlider()
    {
        Name = "RenderScale"; Dock = DockStyle.Top; Height = 42; Width = 280; MinimumSize = new Size(150, 42); TabStop = false;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Margin = Padding.Empty };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100)); layout.ColumnStyles.Add(new(SizeType.AutoSize));
        layout.RowStyles.Add(new(SizeType.Percent, 100));
        layout.Controls.Add(track, 0, 0); layout.Controls.Add(valueText, 1, 0);
        Controls.Add(layout);
        track.ValueChanged += (_, _) => { RefreshValue(); ValueChanged?.Invoke(this, EventArgs.Empty); };
        RefreshValue();
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int Value { get => track.Value; set => track.Value = value; }
    private void RefreshValue() { valueText.Text = $"{Value}%"; track.AccessibleDescription = AccessibleDescription = $"{Value}%"; }
}
