import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from steamvr_theater import begin, restore


class Settings:
    def __init__(self, override=None, default=True):
        self.override, self.default = override, default
        self.calls = []
        self.fail_before = self.fail_after = False

    def get(self):
        return self.default if self.override is None else self.override

    def set(self, value):
        self.calls.append(('set', value))
        if self.fail_before:
            raise RuntimeError('write refused')
        self.override = value
        if self.fail_after:
            raise RuntimeError('connection lost after write')

    def remove(self):
        self.calls.append(('remove',))
        if self.fail_before:
            raise RuntimeError('remove refused')
        self.override = None

    def original(self):
        return dict(before=self.get(), hadOverride=self.override is not None, default=self.default)


class TheaterTransaction(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.journal = Path(self.temp.name)/'pending.json'

    def test_restore_existing_and_absent_override(self):
        for override in (None, True):
            api = Settings(override)
            self.assertEqual(begin(api, self.journal, api.original())['result'], 'temporarily_disabled')
            self.assertFalse(api.get())
            self.assertTrue(self.journal.exists())
            restore(api, self.journal)
            self.assertIs(api.override, override)
            self.assertFalse(self.journal.exists())
            self.assertEqual(restore(api, self.journal)['result'], 'no_pending_change')

    def test_already_disabled_is_untouched(self):
        api = Settings(False)
        self.assertEqual(begin(api, self.journal, api.original())['result'], 'already_disabled')
        self.assertEqual(api.calls, [])
        self.assertFalse(self.journal.exists())

    def test_interruptions_before_and_after_mutation(self):
        for after in (False, True):
            api = Settings()
            api.fail_before, api.fail_after = not after, after
            with self.assertRaises(RuntimeError):
                begin(api, self.journal, api.original())
            self.assertTrue(self.journal.exists())
            api.fail_before = api.fail_after = False
            restore(api, self.journal)
            self.assertIsNone(api.override)

    def test_failed_restore_retains_journal_and_retry_works(self):
        api = Settings()
        begin(api, self.journal, api.original())
        api.fail_before = True
        with self.assertRaises(RuntimeError):
            restore(api, self.journal)
        self.assertTrue(self.journal.exists())
        api.fail_before = False
        restore(api, self.journal)
        self.assertIsNone(api.override)

    def test_external_edit_and_pending_transaction_are_preserved(self):
        api = Settings()
        original = api.original()
        begin(api, self.journal, original)
        with self.assertRaises(ValueError):
            begin(api, self.journal, original)
        api.override = True  # User explicitly changes it while the game runs.
        calls = list(api.calls)
        restore(api, self.journal)
        self.assertIs(api.override, True)
        self.assertEqual(api.calls, calls)

    def test_mismatch_and_forged_journal_refuse_without_mutation(self):
        api = Settings()
        original = api.original()
        api.override = False
        with self.assertRaises(ValueError):
            begin(api, self.journal, original)
        self.assertFalse(self.journal.exists())
        for patch in ({'key': 'inputCaptureEnabled'}, {'requested': True}, {'before': 1}, {'before': False}):
            state = dict(schema=1, section='dashboard', key='autoShowGameTheater', requested=False, **original)
            state.update(patch)
            self.journal.write_text(json.dumps(state))
            with self.assertRaises(ValueError):
                restore(api, self.journal)
            self.assertTrue(self.journal.exists())
            self.assertEqual(api.calls, [])


if __name__ == '__main__':
    unittest.main()
