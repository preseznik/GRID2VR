namespace GRID2VR;

// A compact group of actions with one shared size, including translated/scaled text.
internal class UniformButtonColumn : Panel
{
    private bool arranging;
    private readonly Dictionary<Button, (bool AutoSize, Padding Margin)> original = [];
    protected virtual int LayoutDpi => DeviceDpi;
    internal UniformButtonColumn(string name, params Button[] buttons)
    {
        Name = name; Dock = DockStyle.Top; Margin = Padding.Empty; TabStop = false;
        Controls.AddRange(buttons);
    }

    protected override void OnControlAdded(ControlEventArgs e)
    {
        base.OnControlAdded(e);
        if (e.Control is not Button button) throw new ArgumentException("Only buttons belong in an action column.");
        original.Add(button, (button.AutoSize, button.Margin));
        button.AutoSize = false; button.Margin = Padding.Empty;
        button.TextChanged += Reflow; button.FontChanged += Reflow; button.PaddingChanged += Reflow;
    }

    protected override void OnControlRemoved(ControlEventArgs e)
    {
        if (e.Control is Button button)
        {
            button.TextChanged -= Reflow; button.FontChanged -= Reflow; button.PaddingChanged -= Reflow;
            if (original.Remove(button, out var previous)) { button.AutoSize = previous.AutoSize; button.Margin = previous.Margin; }
        }
        base.OnControlRemoved(e);
    }

    private void Reflow(object? sender, EventArgs e) => PerformLayout();

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        if (arranging || ClientSize.Width <= 0) return;
        arranging = true;
        try
        {
            var buttons = Controls.OfType<Button>().ToArray();
            if (buttons.Length == 0) { Height = Padding.Vertical; return; }
            var available = Math.Max(1, ClientSize.Width - Padding.Horizontal);
            var width = Math.Min(available, buttons.Max(b => b.GetPreferredSize(Size.Empty).Width));
            var height = buttons.Max(b => b.GetPreferredSize(new Size(width, 0)).Height);
            var gap = (int)Math.Round(6 * LayoutDpi / 96.0);
            var y = Padding.Top;
            foreach (var button in buttons)
            { button.SetBounds(Padding.Left, y, width, height); y += height + gap; }
            Height = y - gap + Padding.Bottom;
        }
        finally { arranging = false; }
    }

    public override Size GetPreferredSize(Size proposedSize) => new(Width, Height);
}
