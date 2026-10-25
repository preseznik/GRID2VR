using System.ComponentModel;

namespace GRID2VR;

internal sealed class HudDistanceSlider : UserControl
{
    private readonly TrackBar track = new() { Name = "HudDistanceSlider", AccessibleName = "HUD distance", Minimum = 2, Maximum = 40,
        Value = 2, SmallChange = 1, LargeChange = 2, TickFrequency = 2, Dock = DockStyle.Fill, AutoSize = false, Margin = Padding.Empty };
    private readonly Label valueText = new() { AutoSize = true, Anchor = AnchorStyles.Right, Margin = new Padding(8, 0, 0, 0) };
    public HudDistanceSlider()
    {
        Name = "HudDistance"; Dock = DockStyle.Top; Height = 42; Width = 280; MinimumSize = new Size(150, 42); TabStop = false;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Margin = Padding.Empty };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100)); layout.ColumnStyles.Add(new(SizeType.AutoSize));
        layout.Controls.Add(track); layout.Controls.Add(valueText); Controls.Add(layout);
        track.ValueChanged += (_, _) => RefreshValue(); RefreshValue();
    }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public decimal Value
    {
        get => track.Value / 2m;
        set { (new GraphicsOptions { HudDistance = value }).Validate(); track.Value = (int)(value * 2m); }
    }
    private void RefreshValue() { valueText.Text = $"{Value:0.#} m"; AccessibleDescription = $"{Value:0.#} metres; 1–20 metres in 0.5 m steps"; }
}
