import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import contextlib
import io

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from steamvr_theater import begin, restore
import steamvr_theater as theater


class Settings:
    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass

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

    def test_installed_data_root_cli_receipts_and_recovery(self):
        # Player data is outside the installed tools directory, unlike the dev workspace.
        backup = self.journal.parent/'stereo-probe-20261026-214003-560'
        backup.mkdir()
        api = Settings()
        with patch.object(theater, 'JOURNAL', self.journal), patch.object(theater, 'Settings', return_value=api), \
                patch.object(theater, 'configured', return_value=api.original()), contextlib.redirect_stdout(io.StringIO()):
            theater.main(['begin', '--receipt', str(backup/'theater-begin.json')])
            self.assertFalse(api.get())
            self.assertEqual(json.loads((backup/'theater-begin.json').read_text())['result'], 'temporarily_disabled')
            theater.main(['restore', '--receipt', str(backup/'theater-restored.json')])
            self.assertTrue(api.get())
            self.assertFalse(self.journal.exists())
            self.assertEqual(json.loads((backup/'theater-restored.json').read_text())['result'], 'restored_default')

    def test_no_pending_restore_never_connects_to_steamvr(self):
        backup = self.journal.parent/'stereo-probe-20261026-214003-560'
        backup.mkdir()
        with patch.object(theater, 'JOURNAL', self.journal), \
                patch.object(theater, 'Settings', side_effect=AssertionError('must not open SteamVR')), contextlib.redirect_stdout(io.StringIO()):
            theater.main(['restore', '--receipt', str(backup/'theater-restored.json')])
        self.assertEqual(json.loads((backup/'theater-restored.json').read_text())['result'], 'no_pending_change')

    def test_grouped_run_receipts_and_recovery(self):
        backup = self.journal.parent/'logs/20261026-214003-560/backup'
        backup.mkdir(parents=True)
        api = Settings()
        with patch.object(theater, 'JOURNAL', self.journal), patch.object(theater, 'Settings', return_value=api), \
                patch.object(theater, 'configured', return_value=api.original()), contextlib.redirect_stdout(io.StringIO()):
            theater.main(['begin', '--receipt', str(backup/'theater-begin.json')])
            self.assertFalse(api.get())
            theater.main(['restore', '--receipt', str(backup/'theater-restored.json')])
            self.assertTrue(api.get())
            self.assertFalse(self.journal.exists())
            self.assertEqual(json.loads((backup/'theater-restored.json').read_text())['result'], 'restored_default')
        with patch.object(theater, 'JOURNAL', self.journal), \
                patch.object(theater, 'Settings', side_effect=AssertionError('must not open SteamVR')):
            for parent in (backup.parent, backup/'extra', backup.parent.parent/'invalid/backup'):
                with self.assertRaises(ValueError):
                    theater.main(['begin', '--receipt', str(parent/'theater-begin.json')])

    def test_receipt_escape_and_existing_receipt_refuse_before_api(self):
        backup = self.journal.parent/'stereo-probe-20261026-214003-560'
        backup.mkdir()
        existing = backup/'theater-begin.json'
        existing.write_text('retain me')
        with patch.object(theater, 'JOURNAL', self.journal), \
                patch.object(theater, 'Settings', side_effect=AssertionError('must not open SteamVR')):
            for receipt in (existing, backup/'foreign.json', self.journal.parent/'theater-begin.json'):
                with self.assertRaises(ValueError):
                    theater.main(['begin', '--receipt', str(receipt)])
        self.assertEqual(existing.read_text(), 'retain me')
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
