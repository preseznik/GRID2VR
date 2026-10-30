namespace GRID2VR;

internal sealed partial class MainForm
{
    private readonly LauncherViewPicker viewPicker = new();
    private readonly Func<bool> confirmAdvancedView;
    private bool restoringViewPicker;
    private readonly List<ViewPlacement> viewPlacements = [];
    private readonly List<Control> simpleEventRows = [], simpleRaceRows = [];
    private TabControl? mainTabs;
    private TabPage[] fullPages = [];
    private TabPage? quickPage, lastFullPage;
    private Button? simpleSeat, simpleProfile;
    private sealed record ViewPlacement(Control Control, Control FullParent, int FullIndex, Control SimpleParent);
    private sealed class QuickStartColumns : ResponsiveColumns { protected override int WideBreakpoint => 740; }

    private void InitializeViews(TabControl tabs)
    {
        mainTabs = tabs; fullPages = tabs.TabPages.Cast<TabPage>().ToArray(); lastFullPage = fullPages[0];
        quickPage = new TabPage("Quick start") { Name = "QuickStart", Padding = new Padding(16), AutoScroll = true };
        var root = new VerticalStack { Name = "QuickStartContent" }; quickPage.Controls.Add(root);
        var columns = new QuickStartColumns(); root.Controls.Add(columns); var play = columns.First; var performance = columns.Second;
        Section(play, "Play");
        MoveRow(runtime, play); MoveRow(launchMode, play);
        foreach (var editor in new Control[] { eventChoice, track, carSearch, car }) simpleEventRows.Add(MoveRow(editor, play));
        Move(carCount, play); simpleEventRows.Add(carCount);
        foreach (var editor in new Control[] { opponents, opponentCars, laps })
        {
            var row = MoveRow(editor, play);
            if (editor == laps) simpleEventRows.Add(row); else simpleRaceRows.Add(row);
        }
        Move(lapsHint, play); simpleEventRows.Add(lapsHint);
        Section(performance, "Performance");
        MoveRow(graphicsPreset, performance); Move(presetActions, performance); Move(presetHint, performance); MoveRow(lastPerformance, performance);
        MoveRow(renderScale, performance); Move(refreshHeadset, performance); MoveRow(vrResolutionSummary, performance);
        performance.Controls.Add(TextLabel("Quality is shared with desktop. VR scale affects scene detail, not headset output. Recommend for this PC suggests a VR scale, and a lighter preset when needed, from your last VR session or your graphics card and headset. Potato selects Ultra Low, VR MSAA Off and 70% VR scale. Save or launch to keep your choices."));
        Section(performance, "Controls & comfort");
        simpleSeat = Button("Seat adjustment…", "SimpleSeat"); simpleProfile = Button("Career profiles…", "SimpleProfiles");
        var comfort = new UniformButtonColumn("SimpleComfortActions", simpleSeat, simpleProfile);
        performance.Controls.Add(comfort); Move(drivingControls, comfort);
        simpleSeat.Click += (_, _) =>
        {
            if (!EnterAdvancedView()) return;
            tabs.SelectedTab = fullPages.Single(p => p.Text == "Controls");
            ((TabControl)Controls.Find("ControlTabs", true).Single()).SelectedTab =
                ((TabControl)Controls.Find("ControlTabs", true).Single()).TabPages.Cast<TabPage>().Single(p => p.Text == "Seat");
        };
        simpleProfile.Click += (_, _) => { if (EnterAdvancedView()) tabs.SelectedTab = fullPages.Single(p => p.Text == "Profile"); };
        performance.Controls.Add(TextLabel("Advanced exposes all graphics, VR presentation, profile and diagnostic options. Switching views keeps every choice, including hidden custom settings."));
        viewPicker.ViewChanged += (_, _) =>
        {
            if (restoringViewPicker) return;
            if (windowLoaded && viewPicker.Advanced && tabs.TabPages.Contains(quickPage) && !confirmAdvancedView())
            {
                restoringViewPicker = true;
                try { viewPicker.Advanced = false; }
                finally { restoringViewPicker = false; }
                return; // Cancel leaves tabs, edits and persisted view untouched.
            }
            ApplyView(viewPicker.Advanced);
            if (!windowLoaded) return;
            window = window with { AdvancedView = viewPicker.Advanced };
            try { client.SaveWindow(window); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { notice.Text = "View preference could not be saved: " + error.Message; }
        };
        tips.SetToolTip(viewPicker, "Simple shows essential setup. Advanced shows all options. Switching views never changes or saves gameplay settings.");
        Disposed += (_, _) =>
        {
            foreach (var page in fullPages.Append(quickPage!)) if (page.Parent is null) page.Dispose();
        };

        Control MoveRow(Control editor, Control destination)
        {
            var row = editor.Parent is SettingRow value ? value : throw new InvalidOperationException("Missing settings row: " + editor.Name);
            Move(row, destination); return row;
        }
        void Move(Control control, Control destination) => viewPlacements.Add(new(control, control.Parent!, control.Parent!.Controls.GetChildIndex(control), destination));
        // Record destinations without reparenting until Simple is actually selected.
    }

    private bool EnterAdvancedView()
    {
        viewPicker.Advanced = true;
        return viewPicker.Advanced;
    }

    private bool ConfirmAdvancedView()
    {
        using var dialog = new AdvancedViewConfirmationForm();
        return dialog.ShowDialog(this) == DialogResult.OK;
    }

    private void ApplyView(bool advanced)
    {
        if (mainTabs is null || quickPage is null) return;
        mainTabs.SuspendLayout();
        try
        {
            if (!advanced)
            {
                if (mainTabs.SelectedTab is { } selected && fullPages.Contains(selected)) lastFullPage = selected;
                foreach (var placement in viewPlacements)
                {
                    placement.SimpleParent.Controls.Add(placement.Control);
                    // Preserve the recorded quick-start order before its explanatory copy.
                }
                ArrangeQuickStart();
                mainTabs.TabPages.Clear(); mainTabs.TabPages.Add(quickPage);
                LauncherAppearance.Apply(quickPage);
            }
            else
            {
                foreach (var group in viewPlacements.GroupBy(p => p.FullParent))
                    foreach (var placement in group.OrderBy(p => p.FullIndex))
                    {
                        placement.FullParent.Controls.Add(placement.Control);
                        placement.FullParent.Controls.SetChildIndex(placement.Control, placement.FullIndex);
                        VerticalStack.SetShown(placement.Control, true);
                    }
                mainTabs.TabPages.Clear(); mainTabs.TabPages.AddRange(fullPages);
                mainTabs.SelectedTab = lastFullPage ?? fullPages[0];
            }
            UpdateSimpleContext(ChoiceId(launchMode) == "Normal", ChoiceId(launchMode) == "Race");
            EnableActions();
        }
        finally { mainTabs.ResumeLayout(true); }
    }

    private void ArrangeQuickStart()
    {
        // Original quick-start headings/copy/buttons stay in their own order;
        // insert moved controls immediately after their corresponding headings.
        foreach (var group in viewPlacements.GroupBy(p => p.SimpleParent))
        {
            var stack = group.Key;
            var index = stack is UniformButtonColumn ? 0 :
                stack.Controls.GetChildIndex(stack.Controls.Cast<Control>().First(c => c.Tag is "section")) + 1;
            foreach (var placement in group)
            {
                stack.Controls.SetChildIndex(placement.Control, index++);
            }
        }
    }

    private void UpdateSimpleContext(bool normal, bool race)
    {
        if (viewPicker.Advanced || quickPage is null) return;
        foreach (var row in simpleEventRows) VerticalStack.SetShown(row, !normal);
        foreach (var row in simpleRaceRows) VerticalStack.SetShown(row, race);
        if (simpleSeat is not null) simpleSeat.Enabled = !busy && !active && !pending;
        if (simpleProfile is not null) simpleProfile.Enabled = !busy && !active && !pending;
    }
}
