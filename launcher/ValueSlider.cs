using System.ComponentModel;

namespace GRID2VR;

// Same integer slider and live value used for laps/opponents in DiRT2VR.
internal sealed class ValueSlider : UserControl
{
    private readonly TrackBar track;
    private readonly Label valueText = new() { AutoSize = true, Anchor = AnchorStyles.Right, Margin = new Padding(8, 0, 0, 0) };
    public event EventHandler? ValueChanged;
    public ValueSlider(string name, int minimum, int maximum, int value)
    {
        Name = name; AccessibleName = name; Dock = DockStyle.Top;
        Height = 42; Width = 280; MinimumSize = new Size(150, 42); TabStop = false;
        track = new TrackBar { Name = name + "Slider", AccessibleName = name, Minimum = minimum, Maximum = maximum,
            Value = value, SmallChange = 1, LargeChange = 1, TickFrequency = 1, Dock = DockStyle.Fill, AutoSize = false, Margin = Padding.Empty };
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Margin = Padding.Empty };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100)); layout.ColumnStyles.Add(new(SizeType.AutoSize));
        layout.Controls.Add(track); layout.Controls.Add(valueText); Controls.Add(layout);
        track.ValueChanged += (_, _) => { RefreshValue(); ValueChanged?.Invoke(this, EventArgs.Empty); };
        RefreshValue();
    }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int Value { get => track.Value; set => track.Value = value; }
    private void RefreshValue() { valueText.Text = Value.ToString(); AccessibleDescription = $"{Value}; {track.Minimum}–{track.Maximum}"; }
}
