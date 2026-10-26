using System.Text.Json;
using System.Text.RegularExpressions;

namespace GRID2VR;

internal sealed record SeatOffset(int Height = 0, int Forward = 0, int Right = 0)
{
    public void Validate()
    {
        if (new[] { Height, Forward, Right }.Any(v => v is < -50 or > 50))
            throw new IOException("Seat adjustments must be within 50 cm of the original cockpit.");
    }
}
internal sealed class SeatAlignment
{
    public int Version { get; set; } = 1;
    public Dictionary<string, SeatOffset> Cars { get; set; } = new(StringComparer.Ordinal);
    internal static readonly JsonSerializerOptions Json = new() { WriteIndented = true, PropertyNameCaseInsensitive = true };
    public void Validate()
    {
        if (Version != 1 || Cars is null || Cars.Count > 96) throw new IOException("Invalid cockpit alignments.");
        foreach (var (car, offset) in Cars)
        {
            if (!Regex.IsMatch(car, @"\A[a-z0-9]{3}\z") || offset is null) throw new IOException("Invalid cockpit car.");
            offset.Validate();
        }
    }
    public static SeatAlignment Load(string root)
    {
        var path = Path.Combine(AppPaths.Data(root), "seat-positions.json");
        if (!File.Exists(path)) return new();
        if (new FileInfo(path).Length > 32768) throw new IOException("Seat settings exceed their size limit.");
        var result = JsonSerializer.Deserialize<SeatAlignment>(File.ReadAllText(path), Json) ?? throw new IOException("Invalid seat settings.");
        result.Validate(); return result;
    }
    public void Save(string root)
    {
        Validate();
        var path = Path.Combine(AppPaths.Data(root), "seat-positions.json");
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { File.WriteAllText(temp, JsonSerializer.Serialize(this, Json)); File.Move(temp, path, true); }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }
}

internal sealed class SeatAlignmentPanel : UserControl
{
    private readonly ComboBox car = new() { Name = "SeatCar", Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ValueSlider height = new("Seat height (cm)", -50, 50, 0), forward = new("Seat forward (cm)", -50, 50, 0), right = new("Seat right (cm)", -50, 50, 0);
    private SeatAlignment settings = new();
    private string? selected;
    private bool loading;
    public SeatAlignmentPanel()
    {
        Name = "CockpitAlignment"; AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink; Dock = DockStyle.Top;
        var layout = new TableLayoutPanel { ColumnCount = 1, AutoSize = true, Dock = DockStyle.Top };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100));
        void Label(string text) => layout.Controls.Add(new Label { Text = text, AutoSize = true, Margin = new(0, 8, 0, 3) });
        Label("Align car"); layout.Controls.Add(car);
        Label("Height (cm)"); layout.Controls.Add(height);
        Label("Forward / back (cm)"); layout.Controls.Add(forward);
        Label("Right / left (cm)"); layout.Controls.Add(right);
        var reset = new Button { Text = "Reset this car's alignment", Name = "ResetSeat", AutoSize = true };
        reset.Click += (_, _) => { height.Value = forward.Value = right.Value = 0; Store(); };
        layout.Controls.Add(reset);
        var hint = new Label { AutoSize = true, Text = "Negative height lowers the view. Positive values move up, forward and right. Saved separately for each car; Save settings or launch to apply on the next session, including Normal mode." };
        layout.Controls.Add(hint);
        layout.SizeChanged += (_, _) => hint.MaximumSize = new(Math.Max(1, layout.Width - 12), 0);
        car.SelectedIndexChanged += (_, _) => { if (loading) return; Store(); selected = (car.SelectedItem as ContentItem)?.Id; ShowOffset(); };
        foreach (var slider in new[] { height, forward, right }) slider.ValueChanged += (_, _) => Store();
        Controls.Add(layout);
    }
    public void LoadCars(ContentItem[] cars, string current, SeatAlignment value)
    {
        cars = cars.Where(c => c.CockpitAvailable).ToArray();
        loading = true; settings = value; selected = null;
        try { car.Items.Clear(); car.Items.AddRange(cars); car.SelectedItem = cars.FirstOrDefault(c => c.Id == current) ?? cars.FirstOrDefault(); selected = (car.SelectedItem as ContentItem)?.Id; }
        finally { loading = false; }
        ShowOffset();
    }
    private void Store()
    {
        if (loading || selected is null) return;
        var value = new SeatOffset(height.Value, forward.Value, right.Value);
        if (value == new SeatOffset()) settings.Cars.Remove(selected); else settings.Cars[selected] = value;
    }
    private void ShowOffset()
    {
        loading = true;
        try { var value = selected is not null && settings.Cars.TryGetValue(selected, out var saved) ? saved : new(); height.Value = value.Height; forward.Value = value.Forward; right.Value = value.Right; }
        finally { loading = false; }
    }
    public void Save(string root) { Store(); settings.Save(root); }
}
