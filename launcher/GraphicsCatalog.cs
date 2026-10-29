using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace GRID2VR;

internal sealed record GraphicsQualityChoice(string Id, string Label, Dictionary<string, string> Attributes);
internal sealed record GraphicsCategory(string Field, string Label, string Default, GraphicsQualityChoice[] Choices, bool Available = true);
internal sealed record GraphicsPreset(string Id, string Label, Dictionary<string, string> Values);

// One embedded catalog drives the editors and preset matching. Python consumes
// the same authored file; no installed game XML or arbitrary attributes are saved.
internal static class GraphicsCatalog
{
    private sealed record Catalog(int Schema, GraphicsCategory[] Categories, GraphicsPreset[] Presets);
    private static readonly Catalog catalog = Load();
    internal static IReadOnlyList<GraphicsCategory> Categories => catalog.Categories;
    internal static IReadOnlyList<GraphicsPreset> Presets => catalog.Presets;
    internal static readonly string[] LegacyFields = ["TreeDetail", "ObjectDetail", "CrowdDetail", "TextureFiltering"];

    private static Catalog Load()
    {
        using var stream = typeof(GraphicsCatalog).Assembly.GetManifestResourceStream("GRID2VR.GraphicsOptions.json")
            ?? throw new IOException("Graphics catalog is missing.");
        var value = JsonSerializer.Deserialize<Catalog>(stream, new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
            ?? throw new IOException("Graphics catalog is invalid.");
        if (value.Schema != 1 || value.Categories.Length != 21 || value.Presets.Length != 5 ||
            value.Categories.Select(c => c.Field).Distinct().Count() != value.Categories.Length)
            throw new IOException("Graphics catalog has an unsupported schema.");
        var defaults = new GraphicsOptions();
        foreach (var category in value.Categories)
        {
            var property = typeof(GraphicsOptions).GetProperty(category.Field);
            if (property?.PropertyType != typeof(string) || category.Choices.Length == 0 ||
                category.Choices.Select(c => c.Id).Distinct().Count() != category.Choices.Length ||
                !category.Choices.Any(c => c.Id == category.Default) || (string?)property.GetValue(defaults) != category.Default)
                throw new IOException("Invalid graphics category: " + category.Field);
        }
        foreach (var preset in value.Presets)
            if (preset.Values.Count != value.Categories.Length || value.Categories.Any(c =>
                !preset.Values.TryGetValue(c.Field, out var choice) || !c.Choices.Any(o => o.Id == choice)))
                throw new IOException("Invalid graphics preset: " + preset.Id);
        return value;
    }

    internal static string Get(GraphicsOptions value, string field) =>
        (string)typeof(GraphicsOptions).GetProperty(field)!.GetValue(value)!;

    internal static void Validate(GraphicsOptions value)
    {
        foreach (var category in Categories)
        {
            var choice = Get(value, category.Field);
            if (!category.Choices.Any(c => c.Id == choice) || !category.Available && choice != category.Default)
                throw new ArgumentException("Choose a supported option for " + category.Label + ".");
        }
    }

    internal static GraphicsOptions WithQuality(GraphicsOptions value, IEnumerable<KeyValuePair<string, string>> choices)
    {
        var node = JsonSerializer.SerializeToNode(value)!.AsObject();
        foreach (var (field, choice) in choices)
        {
            var category = Categories.SingleOrDefault(c => c.Field == field) ?? throw new ArgumentException("Unknown graphics choice.");
            if (!category.Choices.Any(c => c.Id == choice) || !category.Available && choice != category.Default)
                throw new ArgumentException("Unsupported graphics choice: " + field);
            node[field] = choice;
        }
        var result = node.Deserialize<GraphicsOptions>()!;
        result.Validate();
        return result;
    }

    private static IEnumerable<KeyValuePair<string, string>> Effective(GraphicsPreset preset) => Categories.Select(c =>
        new KeyValuePair<string, string>(c.Field, c.Available ? preset.Values[c.Field] : c.Default));

    internal static GraphicsOptions ApplyPreset(GraphicsOptions value, string id) => id switch
    {
        "working" => WithQuality(value, Categories.Select(c => new KeyValuePair<string, string>(c.Field, c.Default))),
        "potato" => ApplyPreset(value, "ultralow") with { VrRenderScale = 70, VrMsaa = "off" },
        _ => WithQuality(value, Effective(Presets.SingleOrDefault(p => p.Id == id) ?? throw new ArgumentException("Unknown graphics preset.")))
    };

    internal static string PresetId(GraphicsOptions value)
    {
        bool Matches(IEnumerable<KeyValuePair<string, string>> choices) => choices.All(p => Get(value, p.Key) == p.Value);
        if (value.VrRenderScale == 70 && value.VrMsaa == "off" && Matches(Effective(Presets.Single(p => p.Id == "ultralow")))) return "potato";
        if (Matches(Categories.Select(c => new KeyValuePair<string, string>(c.Field, c.Default)))) return "working";
        return Presets.FirstOrDefault(p => Matches(Effective(p)))?.Id ?? "custom";
    }

    internal static string EncodeExpanded(GraphicsOptions value)
    {
        Validate(value);
        var fields = Categories.Where(c => !LegacyFields.Contains(c.Field)).ToDictionary(c => c.Field, c => Get(value, c.Field));
        return Convert.ToBase64String(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(fields)));
    }
}
