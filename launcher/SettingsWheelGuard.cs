using System.Runtime.CompilerServices;

namespace GRID2VR;

// Application-local message filtering, not an input hook. Wheel messages addressed
// to this form's value editors scroll their containing page before native controls
// can change a value. Focus never arms wheel editing. No input/window messages are
// synthesized or forwarded to other windows, and focus is never moved.
internal sealed class SettingsWheelGuard : IMessageFilter, IDisposable
{
    internal const int MouseWheel = 0x020A, MouseHWheel = 0x020E;
    private readonly Form owner;
    private readonly Func<int> scrollLines;
    private readonly Func<ComboBox, bool> dropdownOpen;
    private readonly ConditionalWeakTable<ScrollableControl, WheelRemainder> remainders = new();
    internal bool IsDisposed { get; private set; }
    private sealed class WheelRemainder { internal int Vertical, Horizontal; }

    internal SettingsWheelGuard(Form owner, Func<int>? scrollLines = null, Func<ComboBox, bool>? dropdownOpen = null)
    {
        this.owner = owner;
        this.scrollLines = scrollLines ?? (() => SystemInformation.MouseWheelScrollLines);
        this.dropdownOpen = dropdownOpen ?? (combo => combo.DroppedDown);
        Application.AddMessageFilter(this);
    }

    public bool PreFilterMessage(ref Message message)
    {
        if (IsDisposed || owner.IsDisposed || owner.Disposing || message.Msg is not (MouseWheel or MouseHWheel)) return false;
        var target = Control.FromChildHandle(message.HWnd);
        if (target is null || target.IsDisposed || !ReferenceEquals(target.FindForm(), owner)) return false;
        Control? editor = target;
        // NumericUpDown and editable ComboBox have native child windows. Resolve
        // the editor from its ancestry, not just the message's immediate handle.
        while (editor is not null && editor != owner && editor is not (TrackBar or ComboBox or NumericUpDown)) editor = editor.Parent;
        if (editor is null || editor == owner || editor is ComboBox combo && dropdownOpen(combo)) return false;
        var delta = unchecked((short)((message.WParam.ToInt64() >> 16) & 0xffff));
        ScrollPage(editor, delta, message.Msg == MouseHWheel);
        message.Result = IntPtr.Zero;
        return true; // Even a dialog/page without scrolling must not edit a value.
    }

    private void ScrollPage(Control editor, int delta, bool horizontal)
    {
        if (delta == 0) return;
        var direction = horizontal ? delta : -delta;
        for (var parent = editor.Parent; parent is not null; parent = parent.Parent)
        {
            if (parent is not ScrollableControl { AutoScroll: true, IsDisposed: false } page) continue;
            ScrollProperties scroll = horizontal ? page.HorizontalScroll : page.VerticalScroll;
            if (!scroll.Visible || !scroll.Enabled) continue;
            var minimum = scroll.Minimum;
            var maximum = Math.Max(minimum, scroll.Maximum - scroll.LargeChange + 1);
            var current = scroll.Value;
            if (maximum <= minimum || (direction > 0 ? current >= maximum : current <= minimum)) continue;
            var remainder = remainders.GetOrCreateValue(page);
            ref var pending = ref (horizontal ? ref remainder.Horizontal : ref remainder.Vertical);
            var lines = scrollLines();
            if (lines == 0) { pending = 0; return; } // Respect Windows' disabled wheel scrolling.
            var total = pending + direction;
            var steps = total / SystemInformation.MouseWheelScrollDelta;
            pending = total % SystemInformation.MouseWheelScrollDelta;
            if (steps == 0) return; // Keep high-resolution wheel/trackpad deltas on the page.
            var unit = lines < 0 ? scroll.LargeChange : Math.Min(int.MaxValue, (long)scroll.SmallChange * lines);
            var next = (int)Math.Clamp(current + steps * unit, minimum, maximum);
            var position = page.AutoScrollPosition;
            page.AutoScrollPosition = horizontal ? new Point(next, -position.Y) : new Point(-position.X, next);
            return;
        }
    }

    public void Dispose()
    {
        if (IsDisposed) return;
        IsDisposed = true;
        Application.RemoveMessageFilter(this);
        remainders.Clear();
    }
}
