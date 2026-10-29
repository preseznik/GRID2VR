namespace GRID2VR;

internal sealed class AdvancedViewConfirmationForm : LauncherForm
{
    internal Control Content { get; }

    internal AdvancedViewConfirmationForm()
    {
        Text = "Switch to Advanced mode"; Font = new Font("Segoe UI", 10);
        AutoScaleMode = AutoScaleMode.Dpi; FormBorderStyle = FormBorderStyle.FixedDialog;
        StartPosition = FormStartPosition.CenterParent; ShowInTaskbar = false;
        MinimizeBox = MaximizeBox = false; ClientSize = new Size(420, 165);
        var body = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(20), ColumnCount = 1, RowCount = 2 };
        body.ColumnStyles.Add(new(SizeType.Percent, 100));
        body.RowStyles.Add(new(SizeType.Percent, 100)); body.RowStyles.Add(new(SizeType.AutoSize));
        var message = new Label { Name = "AdvancedViewMessage", Text = "This switches to Advanced mode.\nYour settings will stay unchanged.",
            AutoSize = true, Dock = DockStyle.Fill, UseMnemonic = false };
        var actions = new FlowLayoutPanel { Name = "AdvancedViewConfirmationActions", AutoSize = true, Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.RightToLeft, WrapContents = false, Margin = new Padding(0, 12, 0, 0) };
        var cancel = new Button { Name = "CancelAdvancedView", Text = "Cancel", DialogResult = DialogResult.Cancel, AutoSize = true, Padding = new Padding(8, 3, 8, 3) };
        var approve = new Button { Name = "ApproveAdvancedView", Text = "Approve", DialogResult = DialogResult.OK, AutoSize = true, Padding = cancel.Padding };
        actions.Controls.AddRange([cancel, approve]); body.Controls.Add(message, 0, 0); body.Controls.Add(actions, 0, 1);
        Controls.Add(body); Content = body; AcceptButton = approve; CancelButton = cancel;
        ActiveControl = cancel;
    }
}
