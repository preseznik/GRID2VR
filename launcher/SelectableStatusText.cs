using System.Diagnostics.CodeAnalysis;

namespace GRID2VR;

// Native read-only edit semantics (selection, Ctrl+A/C and the standard context
// menu) with label-like colours/borders and bounded, wrapping status-row height.
internal sealed class SelectableStatusText : TextBox
{
    private const int MaximumLines = 6;
    private bool fitting;

    internal SelectableStatusText()
    {
        ReadOnly = true; Multiline = true; WordWrap = true;
        BorderStyle = BorderStyle.None; AutoSize = false;
        ShortcutsEnabled = true; HideSelection = false; TabStop = true;
        MaxLength = int.MaxValue;
        Anchor = AnchorStyles.Left | AnchorStyles.Right;
        AccessibleDescription = "Read-only status. Select text or press Ctrl+A, then Ctrl+C to copy.";
    }

    // Polling an unchanged session message must not destroy a user's selection.
    [AllowNull]
    public override string Text
    {
        get => base.Text;
        set
        {
            var next = value ?? "";
            if (!string.Equals(base.Text, next, StringComparison.Ordinal)) base.Text = next;
        }
    }

    internal void FitWidth(int width)
    {
        width = Math.Max(1, width);
        MaximumSize = new Size(width, 0);
        Width = width;
        FitHeight();
    }

    protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); FitHeight(); }
    protected override void OnFontChanged(EventArgs e) { base.OnFontChanged(e); FitHeight(); }
    protected override void OnDpiChangedAfterParent(EventArgs e) { base.OnDpiChangedAfterParent(e); FitHeight(); }

    private void FitHeight()
    {
        if (fitting || IsDisposed || !Multiline) return;
        fitting = true;
        try
        {
            if (TextLength == 0) { Height = 0; return; }
            var lineHeight = Math.Max(1, Font.Height);
            var padding = Math.Max(2, LauncherAppearance.Px(this, 2));
            var maximum = MaximumLines * lineHeight + padding;
            int RequiredHeight(int width)
            {
                width = Math.Max(1, width);
                var measured = TextRenderer.MeasureText(Text, Font, new Size(width, int.MaxValue),
                    TextFormatFlags.WordBreak | TextFormatFlags.TextBoxControl | TextFormatFlags.NoPadding | TextFormatFlags.NoPrefix);
                // DT_EDITCONTROL may return a wide unbroken path segment.
                var wrapped = (int)Math.Ceiling(measured.Width / (double)width) * lineHeight;
                return Math.Max(Math.Max(lineHeight, wrapped), measured.Height) + padding;
            }
            // Do not query/recreate native handles from size/handle events:
            // those events can re-enter TableLayoutPanel's autosize pass.
            var needed = RequiredHeight(Width - padding);
            var scroll = needed > maximum ? ScrollBars.Vertical : ScrollBars.None;
            if (ScrollBars != scroll)
            {
                var start = SelectionStart; var length = SelectionLength;
                ScrollBars = scroll;
                Select(Math.Min(start, TextLength), Math.Min(length, Math.Max(0, TextLength - start)));
            }
            if (scroll == ScrollBars.Vertical) needed = RequiredHeight(Width - padding - SystemInformation.VerticalScrollBarWidth);
            Height = Math.Min(maximum, needed);
        }
        finally { fitting = false; }
    }
}
