using System.Runtime.InteropServices;
using System.Text;

namespace GRID2VR;

// Launcher-only, nonexclusive capture. It never injects or synthesizes game input.
internal sealed class DrivingInput : IDisposable
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct Sample
    {
        public uint Connected, Axes;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public float[] Values;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)] public byte[] Buttons;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public uint[] Pov;
        public static Sample Empty() => new() { Values = new float[8], Buttons = new byte[128], Pov = [uint.MaxValue, uint.MaxValue, uint.MaxValue, uint.MaxValue] };
        public readonly Sample Copy() => new() { Connected = Connected, Axes = Axes, Values = (float[])Values.Clone(), Buttons = (byte[])Buttons.Clone(), Pov = (uint[])Pov.Clone() };
    }
    internal sealed record Device(uint Index, string Id, string Name, string Product)
    {
        public bool Xbox => Product == "xinput_pad";
        public override string ToString() => Name;
    }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate nint OpenFn(nint window);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void CloseFn(nint handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate uint CountFn(nint handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)] private delegate int NameFn(nint handle, uint index, StringBuilder id, uint idSize, StringBuilder name, uint nameSize);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)] private delegate int ProductFn(nint handle, uint index, StringBuilder product, uint size);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ReadFn(nint handle, uint index, ref Sample sample);
    private nint library, handle;
    private CloseFn? close;
    private ReadFn? read;
    public List<Device> Devices { get; } = [];
    private T Export<T>(string name) where T : Delegate => Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(library, name));
    public DrivingInput(nint window)
    {
        var path = Path.Combine(AppContext.BaseDirectory, "driving_input.dll");
        if (File.Exists(Path.Combine(AppContext.BaseDirectory,"GRID2VR","package.json"))) path=Path.Combine(AppContext.BaseDirectory,"GRID2VR","payload","driving_input.dll");
        if (!File.Exists(path)) throw new IOException("Controller capture is not installed. Rebuild the complete launcher; keyboard binding remains available.");
        try
        {
            library = NativeLibrary.Load(path);
            close = Export<CloseFn>("CaptureClose"); read = Export<ReadFn>("CaptureRead");
            handle = Export<OpenFn>("CaptureOpen")(window);
            if (handle == 0) throw new IOException("Windows controller capture could not start.");
            var count = Export<CountFn>("CaptureCount")(handle);
            if (count > 256) throw new IOException("Unexpected controller count.");
            var name = Export<NameFn>("CaptureName"); var product = Export<ProductFn>("CaptureProduct");
            for (uint index = 0; index < count; ++index)
            {
                var id = new StringBuilder(128); var label = new StringBuilder(512); var type = new StringBuilder(128);
                if (name(handle, index, id, 128, label, 512) != 0 && product(handle, index, type, 128) != 0)
                    Devices.Add(new(index, id.ToString(), label.ToString(), type.ToString()));
            }
        }
        catch { Dispose(); throw; }
    }
    public Sample Read(Device device)
    {
        ObjectDisposedException.ThrowIf(handle == 0, this);
        var sample = Sample.Empty(); read!(handle, device.Index, ref sample); return sample;
    }
    internal static int PovDirection(uint value)
    {
        // Ignore diagonals so one gesture cannot silently bind two directions.
        if (value == uint.MaxValue || (value & 0xffff) == 0xffff) return -1;
        return value switch { <= 2250 or >= 33750 and < 36000 => 0, >= 6750 and <= 11250 => 1, >= 15750 and <= 20250 => 2, >= 24750 and <= 29250 => 3, _ => -1 };
    }
    public static DrivingBinding? Detect(string action, Device device, Sample baseline, Sample current, bool axesOnly = false, bool buttonsOnly = false)
    {
        if (baseline.Connected == 0 || current.Connected == 0) return null;
        var result = new DrivingBinding { Action = action, DeviceId = device.Id, Device = device.Name, DeviceType = device.Product };
        if (!buttonsOnly)
        {
            var best = -1; var movement = 0f; var steering = action.StartsWith("Steer ", StringComparison.Ordinal);
            for (var i = 0; i < 8; ++i)
            {
                if ((current.Axes & baseline.Axes & (1u << i)) == 0) continue;
                var delta = Math.Abs(current.Values[i] - baseline.Values[i]);
                var centered = Math.Abs(baseline.Values[i]) < .35f;
                if (steering && (!centered || device.Xbox && i >= 4)) continue;
                var threshold = steering && !device.Xbox ? .15f : centered || device.Xbox ? .55f : .9f;
                if (float.IsFinite(delta) && delta >= threshold && delta > movement) { best = i; movement = delta; }
            }
            if (best >= 0 && (!device.Xbox || best < 6))
            {
                var centered = steering || Math.Abs(baseline.Values[best]) < .35f;
                var negative = current.Values[best] < baseline.Values[best];
                var valid = true;
                if (steering) { valid = Math.Abs(current.Values[best]) >= .15f; negative = current.Values[best] < 0; }
                if (device.Xbox)
                {
                    if (best >= 4) { valid &= baseline.Values[best] <= .15f && current.Values[best] >= .4f; centered = false; negative = false; }
                    else { valid &= Math.Abs(baseline.Values[best]) <= .25f; centered = true; negative = current.Values[best] < 0; }
                    // GRID 2's stock Menu Up uses biDirUpper: XInput Y is already up-positive.
                }
                if (valid) return result with { Input = (device.Xbox ? DrivingControls.XboxAxes : DrivingControls.DirectAxes)[best],
                    Calibration = centered ? negative ? "biDirLower" : "biDirUpper" : negative ? "uniDirNeg" : "uniDirPos",
                    DeadZone = device.Xbox && best < 4 ? .2m : .03m };
            }
        }
        if (axesOnly) return null;
        for (var i = 0; i < 128; ++i)
        {
            if (current.Buttons[i] == 0 || baseline.Buttons[i] != 0) continue;
            if (device.Xbox && (i >= DrivingControls.XboxButtons.Length || DrivingControls.XboxButtons[i].Length == 0)) continue;
            return result with { Input = device.Xbox ? DrivingControls.XboxButtons[i] : "di_button_" + i };
        }
        if (!device.Xbox)
            for (var i = 0; i < 4; ++i)
            {
                var direction = PovDirection(current.Pov[i]);
                if (PovDirection(baseline.Pov[i]) < 0 && direction >= 0)
                    return result with { Input = $"di_dpad_{i}_" + new[] { "up", "right", "down", "left" }[direction] };
            }
        return null;
    }
    public static string Description(DrivingBinding? binding)
    {
        if (binding is null) return "Use game binding";
        if (binding.Keyboard) return new KeysConverter().ConvertToString((Keys)Convert.ToInt32(binding.Input[10..], 16)) ?? binding.Input;
        var direction = binding.Calibration switch { "biDirLower" => " (−)", "biDirUpper" => " (+)", "uniDirNeg" => " (inverted)", _ => "" };
        return binding.Device + " · " + binding.Input.Replace("xinput_", "").Replace("di_", "").Replace('_', ' ') + direction;
    }
    public void Dispose()
    {
        if (handle != 0) { close!(handle); handle = 0; }
        if (library != 0) { NativeLibrary.Free(library); library = 0; }
    }
}

// Learn stable rest, require deliberate movement and release. A held switch,
// noisy unused pedal, lost focus or disconnect cannot assign a stale gesture.
internal sealed class DrivingCapture(DrivingInput.Device device, string action)
{
    private DrivingInput.Sample baseline;
    private bool initialized, buttonsReady;
    private uint presentAxes;
    private readonly long[] axisSince = new long[8];
    private long buttonsSince, candidateSince, releaseSince = -1;
    private DrivingBinding? candidate;
    public bool AxesOnly { get; init; }
    public bool ButtonsOnly { get; init; }
    public bool Ready { get; private set; }
    public bool WaitingForRelease { get; private set; }
    public DrivingBinding? Completed { get; private set; }
    public DrivingBinding? Detected => candidate;
    public void Reset() { initialized = Ready = WaitingForRelease = buttonsReady = false; Completed = candidate = null; releaseSince = -1; }
    public void Update(DrivingInput.Sample current, long now)
    {
        if (current.Connected == 0) { Reset(); return; }
        if (Completed is not null) return;
        if (!initialized)
        {
            baseline = current.Copy(); presentAxes = current.Axes; baseline.Axes = 0;
            Array.Fill(axisSince, now); buttonsSince = now; initialized = true; return;
        }
        if (presentAxes != current.Axes) { Reset(); return; }
        for (var i = 0; i < 8; ++i)
        {
            var bit = 1u << i;
            if (ButtonsOnly || (presentAxes & bit) == 0 || (baseline.Axes & bit) != 0) continue;
            var shifted = Math.Abs(current.Values[i] - baseline.Values[i]) > .06f || !float.IsFinite(current.Values[i]);
            if (device.Xbox && i < 6 && Math.Abs(current.Values[i]) > (i < 4 ? .25f : .15f)) shifted = true;
            if (shifted) { baseline.Values[i] = current.Values[i]; axisSince[i] = now; }
            if (now - axisSince[i] >= 600) baseline.Axes |= bit;
        }
        if (!buttonsReady && !AxesOnly)
        {
            if (!baseline.Buttons.SequenceEqual(current.Buttons) || !baseline.Pov.SequenceEqual(current.Pov))
            { baseline.Buttons = (byte[])current.Buttons.Clone(); baseline.Pov = (uint[])current.Pov.Clone(); buttonsSince = now; }
            buttonsReady = now - buttonsSince >= 600;
        }
        Ready = !ButtonsOnly && baseline.Axes != 0 || buttonsReady;
        if (!Ready) return;
        for (var i = 0; i < 128; ++i) if (current.Buttons[i] == 0) baseline.Buttons[i] = 0;
        for (var i = 0; i < 4; ++i) if (DrivingInput.PovDirection(current.Pov[i]) < 0) baseline.Pov[i] = uint.MaxValue;
        if (!WaitingForRelease)
        {
            var detected = DrivingInput.Detect(action, device, baseline, current, AxesOnly || !buttonsReady, ButtonsOnly);
            if (detected is null) { candidate = null; return; }
            if (candidate != detected) { candidate = detected; candidateSince = now; }
            var analog = DrivingControls.DirectAxes.Contains(candidate.Input) || DrivingControls.XboxAxes.Contains(candidate.Input);
            if (analog && now - candidateSince < 100) return;
            WaitingForRelease = true;
        }
        var axis = Array.IndexOf(device.Xbox ? DrivingControls.XboxAxes : DrivingControls.DirectAxes, candidate!.Input);
        bool released;
        if (axis >= 0) released = Math.Abs(current.Values[axis] - baseline.Values[axis]) < .12f;
        else if (candidate.Input.StartsWith("di_dpad_", StringComparison.Ordinal)) released = DrivingInput.PovDirection(current.Pov[candidate.Input[8] - '0']) < 0;
        else
        {
            var button = device.Xbox ? Array.IndexOf(DrivingControls.XboxButtons, candidate.Input) : int.Parse(candidate.Input[10..]);
            released = button >= 0 && current.Buttons[button] == 0;
        }
        if (!released) { releaseSince = -1; return; }
        if (releaseSince < 0) releaseSince = now;
        if (now - releaseSince >= 160) Completed = candidate;
    }
}
