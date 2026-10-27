using System.Diagnostics;
using System.Text.Json;
using System.Runtime.InteropServices;

namespace GRID2VR;

internal static class ProfileTests
{
    internal static void Run(string fixture)
    {
        var workspace = Path.Combine(fixture, "profiles-fixture");
        var source = Path.Combine(fixture, "steam-career-fixture");
        Directory.CreateDirectory(source);
        var sourceSave = Path.Combine(source, "savegame@profile0#1234567890");
        byte[] original = [0, 1, 2, 255, 99];
        File.WriteAllBytes(sourceSave, original);
        File.WriteAllBytes(Path.Combine(source, "savegame@profile0#1234567890.LKR"), [3, 4]);
        var closedChecks = 0;
        var store = new ProfileStore(workspace, () => ++closedChecks, () => source);
        var selection = Path.Combine(workspace, "artifacts", "profile-selection.json");

        Check(store.SelectedId() == ProfileStore.CurrentCareer);
        Check(store.List().Single().Info!.FileCount == 2 && !Directory.Exists(store.Root));
        var fresh = store.Create("Driver1");
        Check(fresh.Kind == "fresh" && fresh.FileCount == 0 && fresh.LastSavedUtc == null);
        Check(store.SelectedId() == fresh.Id && store.List().Count == 2);
        Check(Directory.Exists(Path.Combine(store.Root, fresh.Id, "remote")));
        var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(store.Root, fresh.Id, "profile.json")));
        using (metadata)
        {
            Check(metadata.RootElement.GetProperty("schema").GetInt32() == 1);
            Check(!metadata.RootElement.TryGetProperty("FileCount", out _));
        }

        foreach (var invalid in new[] { "", " ", "two words", " driver ", "../outside", "x/y", "x\\y", "é", new string('A', 25), "driver1" })
        {
            Reject(() => store.Create(invalid));
            Check(store.SelectedId() == fresh.Id && store.List().Count == 2);
        }
        foreach (var invalid in new[] { "../outside", fresh.Id.ToUpperInvariant(), "not-a-guid", Guid.NewGuid().ToString("N") })
        {
            Reject(() => store.Select(invalid));
            Reject(() => store.Read(invalid));
            Reject(() => store.Delete(invalid));
            Check(store.SelectedId() == fresh.Id);
        }

        var copied = store.Create("Copy1", copyCurrent: true);
        Check(copied.Kind == "copied" && copied.FileCount == 2 && copied.Bytes == 7 && copied.LastSavedUtc != null);
        var copiedFile = Path.Combine(store.Root, copied.Id, "remote", Path.GetFileName(sourceSave));
        Check(File.ReadAllBytes(copiedFile).SequenceEqual(original));
        File.WriteAllBytes(copiedFile, [66]);
        Check(File.ReadAllBytes(sourceSave).SequenceEqual(original));
        var reopened = new ProfileStore(workspace, () => { }, () => source);
        Check(reopened.SelectedId() == copied.Id && reopened.Read(copied.Id).Bytes == 3);
        Reject(() => store.Delete(copied.Id));
        Reject(() => store.Delete(ProfileStore.CurrentCareer));
        store.Select(fresh.Id);
        Check(reopened.SelectedId() == fresh.Id);
        store.Delete(copied.Id);
        Check(!Directory.Exists(Path.Combine(store.Root, copied.Id)) && File.ReadAllBytes(sourceSave).SequenceEqual(original));

        // Native save writes remain local to the chosen career and survive switches.
        var freshSave = Path.Combine(store.Root, fresh.Id, "remote", "savegame@profile0#active");
        File.WriteAllBytes(freshSave, [8, 9]);
        store.Select(ProfileStore.CurrentCareer);
        store.Select(fresh.Id);
        Check(store.Read(fresh.Id).FileCount == 1 && File.ReadAllBytes(freshSave).SequenceEqual(new byte[] { 8, 9 }));
        var localSave = Path.Combine(store.Root, fresh.Id, "local", "savegame", "native-local-save");
        File.WriteAllBytes(localSave, [10, 11, 12]);
        var temporaryWrite = Path.Combine(store.Root, fresh.Id, "remote", ".grid2vr-write-123-1.tmp");
        File.WriteAllBytes(temporaryWrite, [99]);
        Check(store.Read(fresh.Id).FileCount == 2 && store.Read(fresh.Id).Bytes == 5);
        for (var i = 0; i < ProfileStore.MaxFiles; ++i) File.WriteAllBytes(Path.Combine(Path.GetDirectoryName(localSave)!, "extra" + i), []);
        Reject(() => store.Read(fresh.Id));
        for (var i = 0; i < ProfileStore.MaxFiles; ++i) File.Delete(Path.Combine(Path.GetDirectoryName(localSave)!, "extra" + i));

        var validSelection = File.ReadAllBytes(selection);
        foreach (var invalidSelection in new[] { "{", "{\"schema\":2,\"id\":\"current\"}", "{\"schema\":1,\"id\":\"missing\"}", "{\"schema\":1,\"id\":null}" })
        {
            File.WriteAllText(selection, invalidSelection);
            Reject(() => store.SelectedId());
            Reject(() => store.Create("MustNotCreate"));
            Check(store.List().Count == 2);
        }
        File.WriteAllBytes(selection, validSelection);
        var metadataPath = Path.Combine(store.Root, fresh.Id, "profile.json");
        var validMetadata = File.ReadAllBytes(metadataPath);
        File.WriteAllText(metadataPath, "{\"schema\":1,\"id\":\"foreign\",\"name\":\"Nope\",\"kind\":\"fresh\"}");
        Reject(() => store.SelectedId());
        Check(store.List().Single(entry => entry.Id == fresh.Id).Info == null);
        Reject(() => store.Create("AlsoMustNotCreate"));
        File.WriteAllBytes(metadataPath, validMetadata);

        var blocked = new ProfileStore(workspace, () => throw new IOException("Game is open."), () => source);
        Reject(() => blocked.Create("Busy"));
        Reject(() => blocked.Select(ProfileStore.CurrentCareer));
        Reject(() => blocked.Delete(fresh.Id));
        Check(store.SelectedId() == fresh.Id && store.List().Count == 2);
        using (var acquired = new ManualResetEventSlim())
        using (var release = new ManualResetEventSlim())
        {
            Exception? threadError = null;
            var owner = new Thread(() =>
            {
                try
                {
                    using var mutex = new Mutex(false, ProfileStore.SessionMutex);
                    var held = false;
                    try
                    {
                        try { held = mutex.WaitOne(0); } catch (AbandonedMutexException) { held = true; }
                        if (!held) throw new IOException("Profile test mutex is already busy.");
                        acquired.Set(); release.Wait();
                    }
                    finally { if (held) mutex.ReleaseMutex(); }
                }
                catch (Exception error) { threadError = error; acquired.Set(); }
            });
            owner.Start();
            try
            {
                Check(acquired.Wait(TimeSpan.FromSeconds(5)) && threadError == null);
                Reject(() => store.Select(ProfileStore.CurrentCareer));
                Reject(() => store.Create("BusyMutex"));
                Reject(() => store.Delete(fresh.Id));
            }
            finally { release.Set(); owner.Join(); }
        }
        Check(store.SelectedId() == fresh.Id);

        // Failed registration leaves the selected career and all earlier saves intact.
        File.SetAttributes(selection, File.GetAttributes(selection) | FileAttributes.ReadOnly);
        try { Reject(() => store.Create("CannotSelect")); }
        finally { File.SetAttributes(selection, FileAttributes.Normal); }
        Check(store.SelectedId() == fresh.Id && store.List().Count == 2);
        using (var lockedSource = new FileStream(sourceSave, FileMode.Open, FileAccess.Read, FileShare.None))
            Reject(() => store.Create("LockedSource", true));
        Check(store.SelectedId() == fresh.Id && store.List().Count == 2);
        Check(!Directory.EnumerateDirectories(store.Root).Any(path => Path.GetFileName(path).StartsWith(".creating-")));

        var missingSource = new ProfileStore(workspace, () => { }, () => null);
        Reject(() => missingSource.Create("NoSource", true));
        Check(missingSource.Read(ProfileStore.CurrentCareer).LastSavedUtc == null);
        var tooMany = Path.Combine(fixture, "many-save-files");
        Directory.CreateDirectory(tooMany);
        for (var i = 0; i <= ProfileStore.MaxFiles; ++i) File.WriteAllBytes(Path.Combine(tooMany, "save" + i), []);
        Reject(() => new ProfileStore(workspace, () => { }, () => tooMany).Create("TooMany", true));
        var tooLarge = Path.Combine(fixture, "oversized-save");
        Directory.CreateDirectory(tooLarge);
        using (var file = File.Create(Path.Combine(tooLarge, "save"))) file.SetLength(ProfileStore.MaxBytes + 1);
        Reject(() => new ProfileStore(workspace, () => { }, () => tooLarge).Create("TooLarge", true));
        Check(!Directory.EnumerateDirectories(store.Root).Any(path => Path.GetFileName(path).StartsWith(".creating-")));
        Check(store.SelectedId() == fresh.Id && store.List().Count == 2);

        foreach (var invalid in new[] { "../outside", "folder/file", "folder\\file", "save:stream", "CON", "NUL.dat", "COM1", "LPT9.dat", "file.", "file ", "é", "" })
            Check(!ProfileStore.ValidSaveName(invalid));
        Check(ProfileStore.ValidSaveName("savegame@profile0#123.LKR"));
        Check(!ProfileStore.ValidSaveName(new string('x', 181)));
        TestLinkedSource(fixture, workspace, source, store, fresh.Id);
        var hardLinkSource = Path.Combine(fixture, "hardlinked-save-source");
        Directory.CreateDirectory(hardLinkSource);
        var hardLink = Path.Combine(hardLinkSource, "save");
        if (!CreateHardLink(hardLink, sourceSave, IntPtr.Zero)) throw new IOException("Could not create the hard-link fixture.", Marshal.GetLastWin32Error());
        try
        {
            Reject(() => new ProfileStore(workspace, () => { }, () => hardLinkSource).Create("HardLinked", true));
            Check(File.ReadAllBytes(sourceSave).SequenceEqual(original));
        }
        finally { File.Delete(hardLink); }

        // An unexpected nested folder refuses deletion before touching valid saves.
        store.Select(ProfileStore.CurrentCareer);
        var nested = Path.Combine(store.Root, fresh.Id, "remote", "nested");
        Directory.CreateDirectory(nested);
        Reject(() => store.Delete(fresh.Id));
        Check(File.Exists(freshSave) && File.Exists(metadataPath));
        Directory.Delete(nested);
        File.SetAttributes(freshSave, File.GetAttributes(freshSave) | FileAttributes.ReadOnly);
        try { Reject(() => store.Delete(fresh.Id)); Check(File.Exists(metadataPath)); }
        finally { File.SetAttributes(freshSave, FileAttributes.Normal); }
        store.Delete(fresh.Id);
        Check(store.List().Count == 1 && File.ReadAllBytes(sourceSave).SequenceEqual(original));
        Check(closedChecks > 20);
        TestAccountOwnership(fixture, source);
        TestStandalone(fixture, source);
    }

    static void TestStandalone(string fixture, string source)
    {
        var workspace = Path.Combine(fixture, "standalone-profile-fixture");
        var steam = new ProfileStore(workspace, () => { }, () => source, () => "123");
        var managed = steam.Create("Preserved", true);
        var selection = Path.Combine(workspace, "artifacts", "profile-selection.json");
        var originalSelection = File.ReadAllBytes(selection);
        var save = Path.Combine(steam.Root, managed.Id, "remote", "savegame@profile0#1234567890");
        var originalSave = File.ReadAllBytes(save);
        string? NoSteam() => throw new InvalidOperationException("Standalone careers must not access Steam.");
        var direct = new ProfileStore(workspace, () => { }, NoSteam, NoSteam, managedProfilesSupported: false);

        Check(!direct.ManagedProfilesSupported);
        Check(direct.Read(ProfileStore.CurrentCareer).Status.Contains("by the game"));
        Check(direct.List().Single().Id == ProfileStore.CurrentCareer);
        Reject(() => direct.SelectedId()); // Preserve an old selection until the user chooses Current.
        Reject(() => direct.Read(managed.Id));
        Reject(() => direct.Select(managed.Id));
        Reject(() => direct.Create("Fresh"));
        Reject(() => direct.Create("Copy", true));
        Reject(() => direct.Delete(managed.Id));
        Check(File.ReadAllBytes(selection).SequenceEqual(originalSelection));
        Check(File.ReadAllBytes(save).SequenceEqual(originalSave));

        using var panel = new ProfilePanel(direct);
        panel.Reload(); panel.SetIdle(true);
        Check(!panel.Controls.Find("CreateProfile", true).Single().Enabled);
        Check(!panel.Controls.Find("DeleteProfile", true).Single().Enabled);
        Check(panel.Controls.Find("UseProfile", true).Single().Enabled);
        direct.Select(ProfileStore.CurrentCareer);
        panel.Reload();
        Check(direct.SelectedId() == ProfileStore.CurrentCareer);
        Check(!panel.Controls.Find("UseProfile", true).Single().Enabled);
        Check(File.ReadAllBytes(save).SequenceEqual(originalSave));
    }

    static void TestAccountOwnership(string fixture, string source)
    {
        Check(SteamAccountFile.ReadRecent("\"users\" { \"76561197973482250\" { \"MostRecent\" \"1\" } }") == "13216522");
        Check(SteamAccountFile.ReadRecent("\"users\" { \"76561197973482250\" { \"AutoLogin\" \"1\" } }") == "13216522");
        Check(SteamAccountFile.ReadRecent("\"users\" { \"76561197973482250\" { \"PersonaName\" \"A { B \\\"C\\\"\" \"MostRecent\" \"1\" } }") == "13216522");
        foreach (var invalid in new[] {
            "", "\"users\" {", "\"users\" { \"76561197973482250\" { \"MostRecent\" \"1\" }",
            "\"users\" { \"76561197973482250\" { \"MostRecent\" \"0\" } }",
            "\"users\" { \"76561197973482250\" { \"MostRecent\" \"0\" \"AutoLogin\" \"1\" } }",
            "\"users\" { \"76561197973482250\" { \"AutoLogin\" \"1\" } \"76561197973482251\" { \"AutoLogin\" \"1\" } }",
            "\"users\" { \"123\" { \"MostRecent\" \"1\" } }",
            "\"users\" { \"76561197973482250\" { \"MostRecent\" \"1\" \"MostRecent\" \"1\" } }",
            "\"users\" { \"76561197973482250\" { \"MostRecent\" \"1\" } \"76561197973482251\" { \"MostRecent\" \"1\" } }",
            "\"users\" { \"76561197973482250\" { \"MostRecent\" \"1\" } } extra",
            new string('x', 65537) }) Check(SteamAccountFile.ReadRecent(invalid) == null);
        var workspace = Path.Combine(fixture, "account-profile-fixture");
        var account = "123456789";
        var store = new ProfileStore(workspace, () => { }, () => source, () => account);
        var profile = store.Create("AccountOne", true);
        Check(profile.AccountId == account && store.SelectedId() == profile.Id);
        account = "987654321";
        Reject(() => store.Read(profile.Id));
        Reject(() => store.SelectedId());
        Reject(() => store.Select(profile.Id));
        Check(store.List().Single(entry => entry.Id == profile.Id).Info == null);
        store.Select(ProfileStore.CurrentCareer);
        var second = store.Create("AccountTwo");
        Check(second.AccountId == account);
        account = "123456789";
        Reject(() => store.SelectedId());
        store.Select(profile.Id);
        Check(store.SelectedId() == profile.Id);
        Reject(() => new ProfileStore(workspace, () => { }, () => source, () => null).Read(profile.Id));
        store.Select(ProfileStore.CurrentCareer);
        Reject(() => new ProfileStore(workspace, () => { }, () => source, () => null).Create("UnknownAccount"));
        Reject(() => new ProfileStore(workspace, () => { }, () => source, () => "../bad").Create("BadAccount"));
        Reject(() => new ProfileStore(workspace, () => { }, () => source, () => "4294967296").Create("AccountOverflow"));
        Reject(() => new ProfileStore(workspace, () => { }, () => source, () => "0").Create("ZeroAccount"));
        var accountReads = 0;
        Reject(() => new ProfileStore(workspace, () => { }, () => source, () => ++accountReads == 1 ? "123456789" : "987654321").Create("AccountChanged", true));
        Check(store.List().Count == 3 && store.SelectedId() == ProfileStore.CurrentCareer);
        Check(!Directory.EnumerateDirectories(store.Root).Any(path => Path.GetFileName(path).StartsWith(".creating-")));
    }

    static void TestLinkedSource(string fixture, string workspace, string source, ProfileStore store, string selectedId)
    {
        // Junctions work without Developer Mode/admin rights, unlike symbolic links.
        var link = Path.Combine(fixture, "linked-save-source");
        var command = new ProcessStartInfo("cmd.exe") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var argument in new[] { "/d", "/c", "mklink", "/J", link, source }) command.ArgumentList.Add(argument);
        using var process = Process.Start(command) ?? throw new IOException("Could not prepare the linked-save test.");
        var output = process.StandardOutput.ReadToEnd();
        var error = process.StandardError.ReadToEnd();
        process.WaitForExit();
        if (process.ExitCode != 0) throw new IOException("Could not prepare the linked-save test: " + output + error);
        try
        {
            Reject(() => new ProfileStore(workspace, () => { }, () => link).Create("Linked", true));
            Check(store.SelectedId() == selectedId && store.List().Count == 2);
            Reject(() => new ProfileStore(link, () => { }, () => null).Create("Escaped"));
            Check(!Directory.Exists(Path.Combine(source, "artifacts")));
        }
        finally
        {
            if ((File.GetAttributes(link) & FileAttributes.ReparsePoint) == 0) throw new IOException("Linked-save fixture changed; retained.");
            Directory.Delete(link); // Removes only the junction, never its target.
        }
    }

    static void Reject(Action action)
    {
        try { action(); }
        catch (Exception error) when (error is IOException or InvalidDataException or ArgumentException or JsonException or UnauthorizedAccessException) { return; }
        throw new InvalidOperationException("Profile operation was not rejected.");
    }

    static void Check(bool value, [System.Runtime.CompilerServices.CallerArgumentExpression(nameof(value))] string? expression = null)
    {
        if (!value) throw new InvalidOperationException("Profile regression: " + expression);
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool CreateHardLink(string path, string existing, IntPtr reserved);
}
