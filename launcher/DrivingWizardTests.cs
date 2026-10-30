using System.Reflection;

namespace GRID2VR;

internal static class DrivingWizardTests
{
    internal static void Run()
    {
        static void Check(bool condition, string message) { if (!condition) throw new Exception("Driving wizard: " + message); }
        static void Throws(Action action, string message) { try { action(); } catch (InvalidOperationException) { return; } throw new Exception("Driving wizard: " + message); }
        static DrivingBinding Pad(string action, string input = "xinput_button_a") =>
            new() { Action = action, Device = "Xbox controller", DeviceId = "xinput:0", DeviceType = "xinput_pad", Input = input };
        var batches = DrivingControls.WizardBatches;
        Check(batches.Select(b => b.Name).SequenceEqual(["Essentials", "H-pattern shifter", "Replay controls", "Menu controls", "Look, horn and boost"]), "batch order");
        var all = batches.SelectMany(b => b.Actions).ToArray();
        Check(all.Length == DrivingControls.Actions.Length && all.Distinct().Count() == all.Length && DrivingControls.Actions.All(all.Contains), "batches cover every action exactly once");
        Check(new[] { "Steer Left", "Steer Right", "Accelerate", "Brake", "Handbrake", "Gear Up", "Gear Down" }.All(batches[0].Actions.Contains), "essentials hold the driving basics");
        Check(batches[1].Actions.SequenceEqual(Enumerable.Range(1, 7).Select(i => "Gear " + i).Append("Gear Reverse")), "H-pattern batch");
        Check(DrivingControls.Actions.Where(a => a.StartsWith("Replay ", StringComparison.Ordinal)).Append("Instant Replay").Order().SequenceEqual(batches[2].Actions.Order()), "replay batch");
        Check(DrivingControls.Actions.Where(a => a.StartsWith("Menu ", StringComparison.Ordinal)).Order().SequenceEqual(batches[3].Actions.Order()), "menu batch");

        // Essentials run without a question; every later batch waits for an answer.
        var wizard = new DrivingWizard(batches);
        Check(wizard.Action == "Steer Left" && !wizard.Asking && !wizard.Done && wizard.BatchIndex == 0, "starts with essentials");
        Throws(wizard.Accept, "accept while capturing"); Throws(wizard.Skip, "skip while capturing");
        Throws(() => wizard.Record(Pad("Brake")), "binding for another step");
        foreach (var action in batches[0].Actions) { Check(wizard.Action == action, "essentials order"); wizard.Record(action == "Clutch" ? null : Pad(action)); }
        Check(wizard.Asking && wizard.Action is null && wizard.BatchIndex == 1 && wizard.FinishedBatches == 1, "asks before the H-pattern batch");
        Check(wizard.Bindings.Count == batches[0].Actions.Length - 1 && wizard.Bindings.All(b => batches[0].Actions.Contains(b.Action)), "a skipped step adds nothing");
        Throws(() => wizard.Record(null), "record while asking");
        for (var i = 2; i < batches.Length; ++i) { wizard.Skip(); Check(wizard.Asking && wizard.BatchIndex == i && wizard.Action is null, "asks batch by batch"); }
        wizard.Skip(); Check(wizard.Done && !wizard.Asking && wizard.Action is null && wizard.FinishedBatches == 1, "skipping the last batch finishes");

        // Skipped and cancelled batches keep their existing bindings once applied.
        var shifter = new DrivingBinding { Action = "Gear 1", Device = "Separate shifter", DeviceId = "{01234568-0000-0000-0000-504944564944}",
            DeviceType = "{00200EB7-0000-0000-0000-504944564944}", Input = "di_button_0" };
        var settings = new DrivingControls { Bindings = [Pad("Gear 1", "xinput_button_y"), Pad("Instant Replay", "xinput_button_y"), Pad("Replay Pause", "xinput_button_x"), Pad("Menu Up", "xinput_dpad_up"), shifter] };
        var existing = settings.Bindings.ToArray();
        wizard = new(batches);
        foreach (var action in batches[0].Actions) wizard.Record(Pad(action));
        wizard.Skip(); Check(wizard.Batch.Name == "Replay controls", "skip moves to the replay question");
        wizard.Accept(); Check(wizard.Action == "Instant Replay" && !wizard.Asking, "accept starts the batch");
        wizard.Record(Pad("Instant Replay", "xinput_button_b")); wizard.Finish();
        Check(wizard.Done && wizard.FinishedBatches == 1 && wizard.Bindings.Count == batches[0].Actions.Length, "an unfinished batch is discarded");
        foreach (var binding in wizard.Bindings) settings.Assign(binding);
        settings.Validate();
        Check(existing.All(settings.Bindings.Contains) && settings.Bindings.Count == existing.Length + batches[0].Actions.Length, "existing bindings outside essentials are untouched");

        // Accepting every batch reaches every action.
        wizard = new(batches);
        while (!wizard.Done) { if (wizard.Asking) wizard.Accept(); else wizard.Record(Pad(wizard.Action!)); }
        Check(wizard.FinishedBatches == batches.Length && wizard.Bindings.Select(b => b.Action).SequenceEqual(all), "full run binds every action");
        var full = new DrivingControls(); foreach (var binding in wizard.Bindings) full.Assign(binding);
        full.Validate(); Check(DrivingControls.Actions.All(a => full.Bindings.Any(b => b.Action == a)), "full run applies to every action");
        try { _ = new DrivingWizard([]); throw new Exception("Driving wizard: an empty wizard was accepted."); } catch (ArgumentException) { }

        // Hidden forms: the question replaces the capture controls between batches,
        // and closing keeps only finished batches.
        const BindingFlags privateInstance = BindingFlags.NonPublic | BindingFlags.Instance;
        static T Field<T>(DrivingBindingForm form, string name) => (T)typeof(DrivingBindingForm).GetField(name, privateInstance)!.GetValue(form)!;
        static void Next(DrivingBindingForm form, DrivingBinding? binding) => typeof(DrivingBindingForm).GetMethod("Next", privateInstance)!.Invoke(form, [binding]);
        static void Click(Button button) => typeof(Button).GetMethod("OnClick", privateInstance)!.Invoke(button, [EventArgs.Empty]);
        static Button Cancel(DrivingBindingForm form) => Field<FlowLayoutPanel>(form, "captureButtons").Controls.OfType<Button>().Single(b => b.Text == "Cancel");
        var form = new DrivingBindingForm(null, false);
        try
        {
            var heading = Field<Label>(form, "heading"); var batch = Field<Label>(form, "batch");
            var capture = Field<FlowLayoutPanel>(form, "captureButtons"); var question = Field<FlowLayoutPanel>(form, "batchButtons");
            var source = Field<ComboBox>(form, "source"); var bindBatch = Field<Button>(form, "bindBatch");
            var layout = form.Controls[0]; form.Controls.Remove(layout);
            try
            {
                layout.Visible = true;
                Check(heading.Text == "STEER LEFT  (1/11)" && batch.Text == "Essentials — batch 1 of 5" && batch.Visible, "first essentials step");
                Check(capture.Visible && source.Visible && !question.Visible, "capture controls start visible");
                foreach (var action in batches[0].Actions) Next(form, Pad(action));
                Check(heading.Text == "H-PATTERN SHIFTER?" && batch.Text == "Next: H-pattern shifter — batch 2 of 5" && bindBatch.Text == "Bind H-pattern shifter", "H-pattern question");
                Check(question.Visible && !capture.Visible && !source.Visible, "the question hides capture controls");
                Click((Button)question.Controls.Find("WizardSkipBatch", false).Single());
                Check(heading.Text == "REPLAY CONTROLS?" && question.Visible, "skip asks about replay controls");
                Click(bindBatch);
                Check(heading.Text == "INSTANT REPLAY  (1/9)" && batch.Text == "Replay controls — batch 3 of 5" && capture.Visible && source.Visible && !question.Visible, "accept starts replay capture");
                Next(form, Pad("Instant Replay"));
            }
            finally { form.Controls.Add(layout); }
            Click(Cancel(form));
            Check(form.DialogResult == DialogResult.OK && form.Bindings.Count == batches[0].Actions.Length && form.Bindings.All(b => b.Action != "Instant Replay"),
                "cancel keeps finished batches only");
        }
        finally { if (!form.IsDisposed) form.Dispose(); }
        form = new DrivingBindingForm(null, false);
        try
        {
            Next(form, Pad("Steer Left")); Click(Cancel(form));
            Check(form.DialogResult == DialogResult.Cancel && form.Bindings.Count == 0, "cancel during essentials keeps nothing");
        }
        finally { if (!form.IsDisposed) form.Dispose(); }
        form = new DrivingBindingForm(null, false);
        try
        {
            foreach (var action in batches[0].Actions) Next(form, null);
            Click((Button)Field<FlowLayoutPanel>(form, "batchButtons").Controls.Find("WizardFinish", false).Single());
            Check(form.DialogResult == DialogResult.OK && form.Bindings.Count == 0, "finish after skipped essentials closes with OK");
        }
        finally { if (!form.IsDisposed) form.Dispose(); }
        using var single = new DrivingBindingForm("Gear 3", false);
        var singleLayout = single.Controls[0]; single.Controls.Remove(singleLayout);
        try { singleLayout.Visible = true; Check(Field<Label>(single, "heading").Text == "GEAR 3" && !Field<Label>(single, "batch").Visible, "single-action binding has no batches"); }
        finally { single.Controls.Add(singleLayout); }
    }
}
