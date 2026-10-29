namespace GRID2VR;

internal sealed class LauncherViewPicker : FlowLayoutPanel
{
    private readonly RadioButton simple = new() { Name = "SimpleView", Text = "Simple", Appearance = Appearance.Button, AutoSize = true, Margin = Padding.Empty, TextAlign = ContentAlignment.MiddleCenter };
    private readonly RadioButton advanced = new() { Name = "AdvancedView", Text = "Advanced", Appearance = Appearance.Button, AutoSize = true, Margin = Padding.Empty, TextAlign = ContentAlignment.MiddleCenter, Checked = true };
    [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    internal bool Advanced { get => advanced.Checked; set { if (value) advanced.Checked = true; else simple.Checked = true; } }
    public event EventHandler? ViewChanged;
    public LauncherViewPicker()
    {
        Name = "LauncherView"; AccessibleName = "Simple or Advanced launcher view";
        AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink; WrapContents = false;
        Anchor = AnchorStyles.Right; Margin = new Padding(8, 0, 8, 0);
        Controls.AddRange([simple, advanced]);
        advanced.CheckedChanged += (_, _) => ViewChanged?.Invoke(this, EventArgs.Empty);
    }
}

// Keep view/help reachable when the update notice is present at minimum width.
// No window manipulation: this is ordinary layout of our own child controls.
internal sealed class LauncherHeader : TableLayoutPanel
{
    private readonly Control logo, title, update, view, help;
    private bool arranging;
    public LauncherHeader(Control brand, Label caption, Control notice, Control picker, Control about)
    {
        logo = brand; title = caption; update = notice; view = picker; help = about;
        Name = "LauncherHeader"; ColumnCount = 5; RowCount = 2; Dock = DockStyle.Fill; AutoSize = true;
        ColumnStyles.Add(new(SizeType.AutoSize)); ColumnStyles.Add(new(SizeType.Percent, 100));
        ColumnStyles.Add(new(SizeType.AutoSize)); ColumnStyles.Add(new(SizeType.AutoSize)); ColumnStyles.Add(new(SizeType.AutoSize));
        RowStyles.Add(new(SizeType.AutoSize)); RowStyles.Add(new(SizeType.AutoSize));
        caption.AutoSize = false; caption.AutoEllipsis = true; caption.Dock = DockStyle.Fill;
        caption.TextAlign = ContentAlignment.MiddleLeft; caption.MinimumSize = new Size(1, 44);
        Controls.Add(logo, 0, 0); Controls.Add(title, 1, 0); Controls.Add(update, 2, 0); Controls.Add(view, 3, 0); Controls.Add(help, 4, 0);
    }
    protected override void OnLayout(LayoutEventArgs e)
    {
        if (arranging || logo is null) { base.OnLayout(e); return; }
        arranging = true;
        try
        {
            var wide = logo.GetPreferredSize(Size.Empty).Width + title.GetPreferredSize(Size.Empty).Width +
                update.GetPreferredSize(Size.Empty).Width + view.GetPreferredSize(Size.Empty).Width + help.GetPreferredSize(Size.Empty).Width +
                Controls.Cast<Control>().Sum(c => c.Margin.Horizontal) + LauncherAppearance.Px(this, 16);
            var compact = Width < wide;
            SetColumnSpan(update, 1); SetCellPosition(update, new TableLayoutPanelCellPosition(compact ? 1 : 2, compact ? 1 : 0));
            SetColumnSpan(title, compact ? 2 : 1); SetColumnSpan(update, compact ? 4 : 1);
            base.OnLayout(e);
        }
        finally { arranging = false; }
    }
}
