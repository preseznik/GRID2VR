using EgoEngineLibrary.Language;

namespace GRID2VR.Tracks;

// Race-intro title for a custom location. GRID 2's database has no row for a g2vr_
// location, so events fall back to its placeholder location/route, whose language entries
// are db_various ("VARIOUS") and db_skipfe ("{SKIPFE}") - the title iztok saw in the 0.2.23
// test build. Overlay copies of every installed language file (loaded only by custom-track
// sessions through -mod) give those two entries the track's names; every other entry stays
// identical, and an unmodified file must round-trip entry for entry before it is used.
internal static class TrackTitle
{
    internal const string LocationKey = "db_various", RouteKey = "db_skipfe";

    internal static object Write(string gameRoot, string overlay, string location, string route, IDictionary<string, string> inputs)
    {
        string languages = Files.Inside(gameRoot, "language");
        var written = new List<object>();
        foreach (var source in Directory.EnumerateFiles(languages, "language_*.lng").Order(StringComparer.Ordinal))
        {
            Files.NoLinks(source); inputs[source] = Files.Hash(source);
            var original = File.ReadAllBytes(source);
            LngFile Read(byte[] bytes) { using var stream = new MemoryStream(bytes, false); return new LngFile(stream); }
            byte[] Bytes(LngFile file) { using var stream = new MemoryStream(); file.Write(stream); return stream.ToArray(); }
            var file = Read(original);
            // EGO writes the key/value blocks in hash-bucket order (same size, different byte
            // order; the game looks entries up through the hash table), so the unmodified
            // round trip must reproduce every entry exactly rather than every byte.
            var roundTrip = Bytes(file);
            var originalEntries = Entries(file); var roundTripEntries = Entries(Read(roundTrip));
            if (roundTrip.Length != original.Length || roundTripEntries.Count != originalEntries.Count || originalEntries.Any(e => !roundTripEntries.TryGetValue(e.Key, out var v) || v != e.Value))
                throw new InvalidDataException("Language file does not round-trip: " + Path.GetFileName(source));
            if (!file.ContainsKey(LocationKey) || !file.ContainsKey(RouteKey)) throw new InvalidDataException("Language file lacks the placeholder names: " + Path.GetFileName(source));
            var before = Entries(file);
            file.Add(LocationKey, location); file.Add(RouteKey, route);
            var bytes = Bytes(file);
            var after = Entries(Read(bytes));
            if (after.Count != before.Count || after[LocationKey] != location || after[RouteKey] != route ||
                before.Any(e => e.Key is not (LocationKey or RouteKey) && after[e.Key] != e.Value))
                throw new InvalidDataException("Language overlay changed other entries: " + Path.GetFileName(source));
            string target = Path.Combine(overlay, "language", Path.GetFileName(source));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.WriteAllBytes(target, bytes);
            if (Files.Hash(source) != inputs[source]) throw new InvalidDataException("Language file changed while converting.");
            written.Add(new { File = Path.GetFileName(source), Entries = after.Count, Previous = new { Location = before[LocationKey], Route = before[RouteKey] } });
        }
        if (written.Count == 0) throw new InvalidDataException("No installed language files.");
        return new { LocationKey, RouteKey, Location = location, Route = route, Files = written };
    }

    static Dictionary<string, string> Entries(LngFile file)
    {
        var entries = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var bucket in file.hsht.Table) foreach (var entry in bucket) entries[entry.Key] = entry.Value;
        return entries;
    }
}
