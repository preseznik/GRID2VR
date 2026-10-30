namespace GRID2VR;

// Match DiRT2VR's responsive layout without rebuilding controls during resize.
// A focusable section heading. Collapsing hides the rows after it in its stack,
// up to the next heading; rows are never removed, recreated or reordered.
internal sealed class SectionHeader : Button
{
    private bool collapsed;
    public SectionHeader(string title)
    {
        Title = title; Name = "Section" + new string(title.Where(char.IsLetterOrDigit).ToArray());
        Tag = "section"; AutoSize = true; TextAlign = ContentAlignment.MiddleLeft; UseMnemonic = false;
        FlatStyle = FlatStyle.Flat; FlatAppearance.BorderSize = 0; AccessibleName = title;
        UpdateText(); Click += (_, _) => Collapsed = !Collapsed;
    }
    public string Title { get; }
    public event EventHandler? CollapsedChanged;
    [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    public bool Collapsed
    {
        get => collapsed;
        set
        {
            if (collapsed == value) return;
            collapsed = value; UpdateText(); Parent?.PerformLayout();
            CollapsedChanged?.Invoke(this, EventArgs.Empty);
        }
    }
    private void UpdateText()
    {
        Text = (collapsed ? "▸ " : "▾ ") + Title;
        AccessibleDescription = collapsed ? "Collapsed section; press to expand" : "Expanded section; press to collapse";
    }
}

internal class VerticalStack : Panel
{
    private readonly HashSet<Control> excluded = [];
    private readonly HashSet<Control> collapsedHidden = [];
    internal static void SetShown(Control control, bool shown)
    {
        if (control.Parent is VerticalStack parent)
        {
            if (shown) parent.excluded.Remove(control); else parent.excluded.Add(control);
            control.Visible = shown; parent.PerformLayout();
        }
        else control.Visible = shown;
    }
    private bool arranging;
    public VerticalStack() { Dock = DockStyle.Top; Margin = Padding.Empty; TabStop = false; }
    protected override void OnControlAdded(ControlEventArgs e)
    {
        e.Control!.Dock = DockStyle.None; e.Control.SizeChanged += ChildSizeChanged;
        base.OnControlAdded(e);
    }
    protected override void OnControlRemoved(ControlEventArgs e)
    {
        excluded.Remove(e.Control!);
        // A row moved to another stack (Simple view) keeps no collapse state here.
        if (collapsedHidden.Remove(e.Control!)) e.Control!.Visible = true;
        e.Control!.SizeChanged -= ChildSizeChanged; base.OnControlRemoved(e);
    }
    private void ChildSizeChanged(object? sender, EventArgs e) => PerformLayout();
    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        if (arranging || ClientSize.Width <= 0) return;
        arranging = true;
        try
        {
            var y = Padding.Top;
            var collapsed = false;
            foreach (Control child in Controls)
            {
                if (child is SectionHeader header) collapsed = header.Collapsed;
                // Hide only rows this stack would otherwise show, and restore exactly
                // those, so Simple-view exclusions are never undone by expanding.
                if (excluded.Contains(child)) { collapsedHidden.Remove(child); child.Bounds = Rectangle.Empty; continue; }
                if (collapsed && child is not SectionHeader)
                {
                    // Also on hidden pages: a zero-size row must not keep tab focus.
                    collapsedHidden.Add(child); child.Visible = false;
                    child.Bounds = Rectangle.Empty; continue;
                }
                if (collapsedHidden.Remove(child)) child.Visible = true;
                var width = Math.Max(1, ClientSize.Width - Padding.Horizontal - child.Margin.Horizontal);
                if (child is Label label) label.MaximumSize = new Size(width, 0);
                else if (child is SelectableStatusText status) status.FitWidth(width);
                var fixedWidth = child is Button or LinkLabel;
                child.SetBounds(Padding.Left + child.Margin.Left, y + child.Margin.Top,
                    fixedWidth ? Math.Min(width, child.PreferredSize.Width) : width, child.Height);
                if (child is Label or Button or CheckBox or FlowLayoutPanel)
                    child.Height = child.GetPreferredSize(new Size(width, 0)).Height;
                child.PerformLayout();
                y = child.Bottom + child.Margin.Bottom;
            }
            Height = y + Padding.Bottom;
        }
        finally { arranging = false; }
    }
    public override Size GetPreferredSize(Size proposedSize) => new(Width, Height);
}

internal class SettingRow : Panel
{
    private readonly Label caption;
    private readonly Control editor;
    private bool arranging;
    protected virtual int LayoutDpi => DeviceDpi;
    public SettingRow(string title, Control value)
    {
        Name = value.Name + "Row"; Tag = "setting-row"; Dock = DockStyle.Top; Margin = Padding.Empty; TabStop = false;
        caption = new Label { Name = value.Name + "Label", Text = title, AutoSize = false, TextAlign = ContentAlignment.MiddleLeft, UseMnemonic = false };
        editor = value; editor.Dock = DockStyle.None; editor.Margin = Padding.Empty;
        if (editor is Label label) label.AutoSize = false;
        editor.AccessibleName ??= title;
        Controls.Add(caption); Controls.Add(editor);
        editor.SizeChanged += (_, _) => PerformLayout();
        editor.TextChanged += (_, _) => PerformLayout();
    }
    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        if (arranging || editor == null || ClientSize.Width <= 0) return;
        arranging = true;
        try
        {
            int Px(int value) => (int)Math.Round(value * LayoutDpi / 96.0);
            var width = ClientSize.Width; var gap = Px(10); var padding = Px(5);
            var stacked = width < Px(590) && editor is not CheckBox;
            var labelWidth = Math.Min(Px(215), width / 2);
            var editorWidth = Math.Max(1, stacked ? width : width - labelWidth - gap);
            if (editor is Label label) label.MaximumSize = new Size(editorWidth, 0);
            var editorHeight = editor switch
            {
                RenderScaleSlider or HudDistanceSlider => Px(42),
                _ => Math.Max(Px(28), editor.GetPreferredSize(new Size(editorWidth, 0)).Height)
            };
            var labelHeight = string.IsNullOrEmpty(caption.Text) ? 0 : TextRenderer.MeasureText(caption.Text, Font,
                new Size(stacked ? width : labelWidth, 0), TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix).Height;
            var rowHeight = stacked ? labelHeight + editorHeight + gap : Math.Max(labelHeight, editorHeight);
            caption.SetBounds(0, padding, stacked ? width : labelWidth, stacked ? labelHeight : rowHeight);
            editor.SetBounds(stacked ? 0 : labelWidth + gap, padding + (stacked ? labelHeight + gap : (rowHeight - editorHeight) / 2), editorWidth, editorHeight);
            Height = rowHeight + padding * 2;
        }
        finally { arranging = false; }
    }
    public override Size GetPreferredSize(Size proposedSize) => new(Width, Height);
}

internal class ResponsiveColumns : Panel
{
    internal const int WideAt = 1060;
    protected virtual int WideBreakpoint => WideAt;
    public VerticalStack First { get; } = new();
    public VerticalStack Second { get; } = new();
    private bool arranging;
    protected virtual int LayoutDpi => DeviceDpi;
    public ResponsiveColumns()
    {
        Dock = DockStyle.Top; Margin = Padding.Empty; TabStop = false;
        First.Dock = Second.Dock = DockStyle.None; Controls.Add(First); Controls.Add(Second);
        First.SizeChanged += (_, _) => PerformLayout(); Second.SizeChanged += (_, _) => PerformLayout();
    }
    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        if (arranging || First == null || Second == null || Width <= 0) return;
        arranging = true;
        try
        {
            var wide = Width >= (int)Math.Round(WideBreakpoint * LayoutDpi / 96.0);
            var gap = (int)Math.Round(30 * LayoutDpi / 96.0);
            var firstWidth = wide ? (int)Math.Round((Width - gap) * 0.54) : Width;
            First.SetBounds(0, 0, firstWidth, First.Height); First.PerformLayout();
            Second.SetBounds(wide ? firstWidth + gap : 0, wide ? 0 : First.Height,
                wide ? Width - firstWidth - gap : Width, Second.Height); Second.PerformLayout();
            Height = wide ? Math.Max(First.Height, Second.Height) : First.Height + Second.Height;
        }
        finally { arranging = false; }
    }
    public override Size GetPreferredSize(Size proposedSize) => new(Width, Height);
}
