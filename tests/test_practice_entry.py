import copy
from pathlib import Path
import sys
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from practice_entry import CHANGES, redirect, signature, verify_roundtrip


class PracticeEntryTests(unittest.TestCase):
    def setUp(self):
        self.flow = ET.fromstring('''<flow><node id="outer" state="wrapper">
          <node id="2Ic" state="4n"><link id="yes" target="2Id"/><link id="no" target="2Iy"/></node>
          <node id="2Id" state="mt"><link id="next" target="demo"/></node>
          <node id="2Iy" state="dt"><link id="next" target="normal"/></node>
          <node id="2Im" state="4p"><link id="yes" target="2Ii"/><link id="no" target="2In"/></node>
          <node id="2Ii" state="jD"><link id="next" target="retire"/></node>
          <node id="2In" state="dP"><link id="next" target="start"/></node>
          <node id="unrelated" state="pause"><link id="quit" target="exit"/></node>
        </node></flow>''')
        self.states = ET.fromstring('''<states>
          <StateDecisionBool id="4n" data="attract_mode_chosen" ifTrue="yes" ifFalse="no"/>
          <StateDecisionBool id="4p" data="attract_mode_chosen" ifTrue="yes" ifFalse="no"/>
          <StateCutscene id="dt" cutscene="establish_root" disable_ffb="true"/>
        </states>''')

    def test_exactly_two_edges_change_and_input_is_unchanged(self):
        before = signature(self.flow)
        result = redirect(self.flow, self.states)
        for change in CHANGES:
            changed = result.find(f".//node[@id='{change['node']}']/link[@id='yes']")
            self.assertEqual(changed.get('target'), change['after'])
            changed.set('target', change['before'])
        self.assertEqual(signature(result), before)
        self.assertEqual(signature(self.flow), before)

    def test_missing_duplicate_or_changed_graph_refused(self):
        for mutation in ('duplicate', 'branch', 'state', 'destination', 'extra-child'):
            with self.subTest(mutation=mutation):
                root = copy.deepcopy(self.flow)
                node = root.find(".//node[@id='2Ic']")
                if mutation == 'duplicate': root.append(copy.deepcopy(node))
                if mutation == 'branch': node[0].set('target', 'other')
                if mutation == 'state': node.set('state', 'other')
                if mutation == 'destination': root.find(".//node[@id='2Iy']").set('state', 'other')
                if mutation == 'extra-child': ET.SubElement(node, 'node')
                with self.assertRaises(ValueError): redirect(root, self.states)
        self.states[0].set('data', 'unrelated_setting')
        with self.assertRaises(ValueError): redirect(self.flow, self.states)

    def test_changed_skip_route_is_refused(self):
        for identity in ('2Im', '2Ii', '2In'):
            root = copy.deepcopy(self.flow)
            root.find(f".//node[@id='{identity}']").set('state', 'other')
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                redirect(root, self.states)
        self.flow.find(".//node[@id='2Im']/link[@id='yes']").set('target', 'exit')
        with self.assertRaises(ValueError): redirect(self.flow, self.states)

    def test_roundtrip_catches_unrelated_semantic_change(self):
        expected = redirect(self.flow, self.states)
        actual = ET.fromstring(ET.tostring(expected))
        verify_roundtrip(expected, actual)
        actual.find(".//node[@id='unrelated']/link").set('target', 'different_exit')
        with self.assertRaises(ValueError): verify_roundtrip(expected, actual)


if __name__ == '__main__':
    unittest.main()
