namespace GRID2VR;

internal sealed class AboutForm : LauncherForm
{
    private readonly UpdatesPanel updates;
    public AppRelease? AvailableUpdate => updates.AvailableUpdate;
    public event Action<AppRelease?>? UpdateAvailable;
    public AboutForm(string root, int recenterKey, int recenterModifiers, string? topic = null, bool sessionIdle = true,
        AppRelease? knownUpdate = null, Func<bool, CancellationToken, Task<AppRelease?>>? releaseCheck = null)
    {
        Text = "GRID2VR — Help / About";
        Font = new Font("Segoe UI", 10);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(820, 680); MinimumSize = new Size(560, 540);
        StartPosition = FormStartPosition.CenterParent;
        ShowInTaskbar = false; MinimizeBox = false;

        var tabs = new ModernTabs { Name = "HelpTabs", Dock = DockStyle.Fill };
        var about = new TabPage("About") { BackColor = SystemColors.Control, AutoScroll = true };
        var instructions = new TabPage("Instructions") { BackColor = SystemColors.Control, Padding = new Padding(16) };
        tabs.TabPages.AddRange([about, instructions]);
        var content = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(20), ColumnCount = 1 };
        content.ColumnStyles.Add(new(SizeType.Percent, 100)); about.Controls.Add(content);
        var title = AddText(content, "GRID2VR"); title.Font = new Font(Font.FontFamily, 20, FontStyle.Bold);
        AddText(content, $"Version {Updates.CurrentVersion} — {BuildInfo.Channel}").Name = "BuildVersion";
        AddText(content, $"Build: {BuildInfo.Revision} — {BuildInfo.Built}").Name = "BuildDetails";
        AddText(content, "Developed by Bohloney").Font = new Font(Font, FontStyle.Bold);
        AddText(content, "Native cockpit VR and a desktop launcher for GRID 2, with direct practice, AI races and configurable controls.");
        var read = new Button { Text = "Instructions", Name = "ShowInstructions", AutoSize = true };
        read.Click += (_, _) => tabs.SelectedTab = instructions;
        var links=new FlowLayoutPanel { AutoSize=true, Dock=DockStyle.Top, WrapContents=true };
        links.Controls.Add(read);
        Button Link(string text,string target) { var button=new Button{Text=text,AutoSize=true,Tag=target};button.Click+=(_,_)=>{try{System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(target){UseShellExecute=true});}catch(Exception ex){MessageBox.Show(this,ex.Message,"GRID2VR");}};links.Controls.Add(button);return button; }
        var readme=new Button { Text="Open README",AutoSize=true };
        readme.Click+=(_,_)=>{
            try {
                var command=new System.Diagnostics.ProcessStartInfo("notepad.exe"){UseShellExecute=false};
                command.ArgumentList.Add(Path.Combine(root,"README.md"));System.Diagnostics.Process.Start(command);
            } catch(Exception ex){MessageBox.Show(this,ex.Message,"GRID2VR");}
        };
        links.Controls.Add(readme);
        Link("GitHub / report an issue",Updates.Repository+"/issues"); Link("Releases",Updates.Repository+"/releases");
        Link("Support on Patreon",Branding.Patreon).Name="SupportPatreon";content.Controls.Add(links);
        updates=new UpdatesPanel(root,about:true,releaseCheck:releaseCheck);content.Controls.Add(updates);
        updates.UpdateAvailable += result => UpdateAvailable?.Invoke(result);
        if (knownUpdate is not null) updates.SetRelease(knownUpdate);
        Shown+=async (_,_)=>{updates.SetIdle(sessionIdle);await updates.Start(forceCheck:true);};

        var instructionLayout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
        instructionLayout.ColumnStyles.Add(new(SizeType.Percent, 100));
        instructionLayout.RowStyles.Add(new(SizeType.AutoSize)); instructionLayout.RowStyles.Add(new(SizeType.Percent, 100));
        var topics = new ComboBox { Name = "HelpTopic", AccessibleName = "Instruction topic", Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList, Margin = new Padding(0, 0, 0, 12) };
        var body = new RichTextBox { Name = "InstructionText", AccessibleName = "Instructions", Dock = DockStyle.Fill, ReadOnly = true, BorderStyle = BorderStyle.None, BackColor = SystemColors.Control, DetectUrls = false, ScrollBars = RichTextBoxScrollBars.Vertical, TabStop = true };
        topics.Items.AddRange(LauncherPresentation.Instructions(recenterKey, recenterModifiers));
        topics.SelectedIndexChanged += (_, _) => body.Text = ((HelpTopic)topics.SelectedItem!).Text;
        topics.SelectedItem = topics.Items.Cast<HelpTopic>().FirstOrDefault(t => t.Title == topic) ?? topics.Items[0];
        instructionLayout.Controls.Add(topics); instructionLayout.Controls.Add(body); instructions.Controls.Add(instructionLayout);
        if (topic != null) tabs.SelectedTab = instructions;

        var close = new Button { Text = "Close", Name = "CloseAbout", AutoSize = true, DialogResult = DialogResult.Cancel };
        var footer = new FlowLayoutPanel { Dock = DockStyle.Bottom, AutoSize = true, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(12) };
        footer.Controls.Add(close); CancelButton = close;
        Controls.Add(tabs); Controls.Add(footer);
        content.SizeChanged += (_, _) =>
        {
            foreach (var label in content.Controls.OfType<Label>()) label.MaximumSize = new Size(Math.Max(1, content.ClientSize.Width - content.Padding.Horizontal - 16), 0);
        };
        Shown += (_, _) =>
        {
            var work = Screen.FromControl(this).WorkingArea;
            MinimumSize = new Size(Math.Min(MinimumSize.Width, work.Width), Math.Min(MinimumSize.Height, work.Height));
            Size = new Size(Math.Min(Width, work.Width), Math.Min(Height, work.Height));
        };
    }

    private static Label AddText(TableLayoutPanel content, string text)
    {
        var label = new Label { Text = text, AutoSize = true, MaximumSize = new Size(720, 0), Margin = new Padding(0, 0, 0, 12) };
        content.Controls.Add(label); return label;
    }
}
