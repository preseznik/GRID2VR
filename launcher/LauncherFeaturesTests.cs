using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace GRID2VR;

internal static class LauncherFeaturesTests
{
    private static void Check(bool value, [System.Runtime.CompilerServices.CallerArgumentExpression(nameof(value))] string? text = null)
    { if (!value) throw new InvalidOperationException("Launcher feature check failed: " + text); }
    private static void Reject(Action action)
    { bool failed = false; try { action(); } catch (IOException) { failed = true; } Check(failed); }
    private sealed class Handler(Func<HttpRequestMessage, HttpResponseMessage> reply) : HttpMessageHandler
    { protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellation) => Task.FromResult(reply(request)); }
    private static JsonElement Release(string tag = "v0.2.0", bool draft = false, bool preview = false, string? url = null, string? hash = null, long size = 3) => JsonSerializer.SerializeToElement(new
    {
        tag_name = tag, draft, prerelease = preview, assets = new[] { new { name = $"GRID2VR-{tag.TrimStart('v')}-Setup.exe", state = "uploaded", size,
            browser_download_url = url ?? Updates.Repository + $"/releases/download/{tag}/GRID2VR-{tag.TrimStart('v')}-Setup.exe", digest = hash ?? "sha256:" + new string('a',64) } }
    });
    public static void Run(string fixture)
    {
        var client = new SessionClient(fixture);
        var settings = client.Load(); var before = JsonSerializer.Serialize(settings);
        Check(new Choices().CockpitExposureLimit);
        Check(JsonSerializer.Deserialize<Choices>("{}")!.CockpitExposureLimit);
        var exposureChoices = settings with { CockpitExposureLimit = false };
        client.Save(exposureChoices); Check(!client.Load().CockpitExposureLimit);
        Check(client.LaunchCommand(exposureChoices, true).ArgumentList.Contains("-NativeCockpitExposure"));
        Check(!client.LaunchCommand(exposureChoices, false).ArgumentList.Contains("-NativeCockpitExposure"));
        client.Save(settings);
        Check(new Choices().KeepHorizonLevel);
        Check(JsonSerializer.Deserialize<Choices>("{}")!.KeepHorizonLevel);
        Check(!client.LaunchCommand(settings with { KeepHorizonLevel=true },true).ArgumentList.Contains("-RecenterTilt"));
        var tilted = settings with { KeepHorizonLevel=false };
        client.Save(tilted); Check(!client.Load().KeepHorizonLevel); client.Save(settings);
        Check(client.LaunchCommand(tilted,true).ArgumentList.Contains("-RecenterTilt"));
        Check(!client.LaunchCommand(tilted,false).ArgumentList.Contains("-RecenterTilt"));
        var seats = new SeatAlignment { Cars = new() { ["s20"] = new(-10, 2, 3), ["pag"] = new(-5, 0, 0) } }; seats.Save(fixture);
        Check(SeatAlignment.Load(fixture).Cars["s20"] == new SeatOffset(-10,2,3));
        Check(JsonSerializer.Serialize(client.Load()) == before);
        Reject(() => new SeatOffset(51).Validate()); Reject(() => new SeatAlignment { Cars = new() { ["../"] = new() } }.Save(fixture));
        using (var panel = new SeatAlignmentPanel())
        {
            panel.LoadCars([new("s20","Honda"),new("pag","Pagani")],"s20",seats);
            var car = (ComboBox)panel.Controls.Find("SeatCar",true).Single();
            var height = (ValueSlider)panel.Controls.Find("Seat height (cm)",true).Single();
            Check(height.Value == -10); height.Value = -12; car.SelectedIndex = 1; Check(height.Value == -5);
            car.SelectedIndex = 0; Check(height.Value == -12); panel.Save(fixture); Check(SeatAlignment.Load(fixture).Cars["s20"].Height == -12);
        }
        var xbox = new DrivingBinding { Action="Recenter",Device="Xbox",DeviceType="xinput_pad",DeviceId="xinput:0",Input="xinput_button_back" };
        Check(DeviceRecenter.Decode(DeviceRecenter.Encode(xbox)) == xbox);
        var wheel = xbox with { Device="Wheel",DeviceId="{5a6ef250-7c39-4dbe-9314-c4f4415ca301}",DeviceType="{5a6ef250-7c39-4dbe-9314-c4f4415ca302}",Input="di_button_127" };
        DeviceRecenter.Validate(wheel);
        foreach (var bad in new[] { xbox with { Input="xinput_left_trigger" }, wheel with { Input="di_button_128" }, wheel with { Input="di_y_axis" }, xbox with { DeviceId="xinput:4" } }) Reject(() => DeviceRecenter.Validate(bad));
        var sample = DrivingInput.Sample.Empty(); sample.Connected=1; sample.Buttons[5]=1; sample.Buttons[127]=1;
        Check(DeviceRecenter.Down(xbox,sample)); Check(DeviceRecenter.Down(wheel,sample));
        wheel=wheel with { Input="di_dpad_0_left" };sample.Pov[0]=27000;Check(DeviceRecenter.Down(wheel,sample));sample.Pov[0]=4500;Check(!DeviceRecenter.Down(wheel,sample));
        var edge = new RecenterEdge(); Check(!edge.Update(true,true,true)); Check(!edge.Update(true,true,false)); Check(edge.Update(true,true,true)); Check(!edge.Update(true,true,true));
        Check(!edge.Update(true,false,false)); Check(!edge.Update(true,true,true)); Check(!edge.Update(false,true,false)); Check(!edge.Update(true,true,true));
        Check(!edge.Update(true,true,false)); Check(edge.Update(true,true,true));
        var options = settings with { RecenterDevice=xbox };client.Save(options);Check(client.Load().RecenterDevice==xbox);
        var args=client.LaunchCommand(options,true).ArgumentList;Check(DeviceRecenter.Decode(args[args.IndexOf("-RecenterDeviceBase64")+1])==xbox);
        Check(!client.LaunchCommand(options,false).ArgumentList.Contains("-RecenterDeviceBase64"));client.Save(settings);
        Reject(() => DeviceRecenter.OwnedRun(fixture,Path.Combine(fixture,"artifacts","..","foreign")));
        Task.Run(() => TestUpdates(fixture)).GetAwaiter().GetResult();
    }
    private static async Task TestUpdates(string fixture)
    {
        Check(ReleaseVersion.Parse("v1.10.0")!.CompareTo(ReleaseVersion.Parse("1.9.99"))>0);
        Check(ReleaseVersion.Parse("1.0.0-alpha.10")!.CompareTo(ReleaseVersion.Parse("1.0.0-alpha.2"))>0);
        Check(ReleaseVersion.Parse("1.0.0")!.CompareTo(ReleaseVersion.Parse("1.0.0-rc.99"))>0);
        Check(ReleaseVersion.Parse("1.0.0+abc")!.CompareTo(ReleaseVersion.Parse("1.0.0+def"))==0);
        Check(ReleaseVersion.Parse("1.0.0-01")==null && ReleaseVersion.Parse("garbage")==null);
        Check(Updates.ReadRelease(Release(draft:true),true)==null && Updates.ReadRelease(Release(preview:true),false)==null);
        Check(Updates.ReadRelease(Release("v0.3.0-beta.1"),false)==null);
        foreach(var bad in new[] { Release(url:"https://example.com/foreign.exe"),Release(size:Updates.MaxInstaller+1),Release(hash:""),Release(hash:"sha256:bad") }) Check(Updates.ReadRelease(bad,true)!.Installer==null);
        var calls=0;
        using(var http=new HttpClient(new Handler(request =>
        {
            calls++;Check(request.RequestUri!.Host=="api.github.com");
            var rows=calls==1 ? Enumerable.Repeat(Release("v0.2.0"),100).ToArray() : new[] {Release("v0.9.0-beta.1",preview:true),Release("v0.4.0")};
            return new(HttpStatusCode.OK){Content=new StringContent(JsonSerializer.Serialize(rows))};
        })))
        { Check((await new Updates(http).Check(false,CancellationToken.None))!.Tag=="v0.4.0" && calls==2); }
        using(var http=new HttpClient(new Handler(_ => new(HttpStatusCode.OK) { Content=new StringContent(JsonSerializer.Serialize(new[]{Release("v0.0.9"),Release("v9.0.0-beta.1",preview:true)})) })))
        {
            Check(await new Updates(http).Check(false,CancellationToken.None)==null);
            Check((await new Updates(http).Check(false,CancellationToken.None,allowOlder:true))!.Tag=="v0.0.9");
        }
        using(var http=new HttpClient(new Handler(_ => new(HttpStatusCode.NotFound))))
        { bool failed=false;try{await new Updates(http).Check(false,CancellationToken.None);}catch(IOException){failed=true;}Check(failed); }
        var bytes=Encoding.UTF8.GetBytes("verified fixture, not an executable");var hash=Convert.ToHexStringLower(SHA256.HashData(bytes));
        var release=Updates.ReadRelease(Release(hash:"sha256:"+hash,size:bytes.Length),false)!;
        using(var http=new HttpClient(new Handler(_ => new(HttpStatusCode.OK){Content=new ByteArrayContent(bytes)})))
        {
            var service=new Updates(http);var asset=release.Installer!;
            var path=await service.Download(asset,fixture,null,CancellationToken.None);
            Check(File.ReadAllBytes(path).SequenceEqual(bytes));
            var command=Updates.InstallerCommand(fixture,path,asset);
            Check(command.UseShellExecute && command.ArgumentList.SequenceEqual(new[]{"/DIR="+Path.GetFullPath(AppPaths.Game(fixture)),"/NORESTART"}));
            // A pending session blocks installer execution; no process can be launched by this fixture.
            var pending=Path.Combine(fixture,"artifacts","session-pending.json");File.WriteAllText(pending,"{}");
            try{Reject(()=>Updates.Install(fixture,path,asset));}finally{File.Delete(pending);}
            File.WriteAllText(path,"tampered");Reject(()=>Updates.InstallerCommand(fixture,path,asset));File.Delete(path);
            foreach(var invalid in new[]{asset with { Sha256=new string('0',64) },asset with { Size=asset.Size+1 }})
            { bool failed=false;try{await service.Download(invalid,fixture,null,CancellationToken.None);}catch(IOException){failed=true;}Check(failed);Check(!File.Exists(path)); }
            using var cancel=new CancellationTokenSource();cancel.Cancel();
            try{await service.Download(asset,fixture,null,cancel.Token);throw new Exception("Cancellation ignored");}catch(OperationCanceledException){}
            Check(!Directory.EnumerateFiles(Path.GetDirectoryName(path)!,"*.part").Any());
        }
        var prefs=new UpdatePreferences(false,true);prefs.Save(fixture);Check(UpdatePreferences.Load(fixture)==prefs);
    }
}
