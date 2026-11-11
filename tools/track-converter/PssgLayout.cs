using EgoEngineLibrary.Graphics.Pssg;
using EgoEngineLibrary.Graphics.Pssg.Elements;

namespace GRID2VR.Tracks;

// Makes RenderDataSourceWriter output look like Codemasters' own tracksplit files.
// A Brands Hatch control proved the vertex and index bytes identical to stock, yet terrain
// rewritten with the writer's naming and placement did not draw. Stock files name each
// RENDERSTREAM "<source id>_<n>", keep every DATABLOCK before the TEXTUREs of the
// RENDERINTERFACEBOUND library, and give index sources and blocks their own ids.
internal static class PssgLayout
{
    internal static (int Sources, int Blocks) StockStyle(PssgFile file, string prefix)
    {
        var dataLib = file.Elements<PssgLibrary>().Single(l => l.Type == "RENDERINTERFACEBOUND");
        var blocks = dataLib.ChildElements.OfType<PssgDataBlock>().ToDictionary(b => b.Id);
        int sources = 0, renamedBlocks = 0;
        foreach (var source in file.Elements<PssgRenderDataSource>().Where(s => s.Id.StartsWith(prefix, StringComparison.Ordinal)))
        {
            sources++;
            foreach (var index in source.ChildElements.OfType<PssgRenderIndexSource>()) index.Id = source.Id + "_ix";
            var renamed = new Dictionary<string, string>();
            int n = 0;
            foreach (var stream in source.ChildElements.OfType<PssgRenderStream>())
            {
                stream.Id = source.Id + "_" + n++;
                string old = stream.DataBlock.TrimStart('#');
                if (!renamed.TryGetValue(old, out var name))
                {
                    if (!blocks.Remove(old, out var block)) throw new InvalidDataException("Data block shared or missing: " + old);
                    name = source.Id + "_db" + (renamed.Count == 0 ? "" : renamed.Count.ToString(System.Globalization.CultureInfo.InvariantCulture));
                    block.Id = name; blocks[name] = block; renamed[old] = name; renamedBlocks++;
                }
                stream.DataBlock = "#" + name;
            }
        }
        // Stock order: all DATABLOCKs, then all TEXTUREs.
        var textures = dataLib.ChildElements.OfType<PssgTexture>().ToArray();
        foreach (var texture in textures) dataLib.RemoveChild(texture);
        foreach (var texture in textures) dataLib.AppendChild(texture);
        return (sources, renamedBlocks);
    }
}
