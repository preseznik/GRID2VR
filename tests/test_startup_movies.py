import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch as mock_patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import startup_movies as startup


class StartupMoviesTests(unittest.TestCase):
    def setUp(self):
        self.states = ET.fromstring('''<xml><World>
          <StateVideoPreStart id="4a" filename="video/sting.bik" skippable="true"/>
          <StateVideoPreStart id="4b" filename="video/intel_sting.bik" skippable="true"/>
          <StateVideoPreStart id="4c" filename="video/attract.bik" skippable="true" fadeToBlackOnSkip="true" localised="jpn"/>
          <StateCutscene id="dt" cutscene="establish_root" disable_ffb="true"/>
          <StateCutscene id="du" cutscene="buildup_grid_root" disable_ffb="true"/>
          <StateCutscene id="ds" cutscene="prerace" disable_ffb="true"/>
          <StateRaceIntroSetup id="dO"/>
          <StateCutscene id="dz" cutscene="startsequence"/>
        </World></xml>''')
        self.flow = ET.fromstring('''<flow><node id="boot" state="wrapper">
          <node id="2Q4" state="4b"><link id="next" target="2Q5"/></node>
          <node id="2Q5" state="4a"><link id="next" target="2Pw"/></node>
          <node id="2Pw" state="b"><link id="next" target="game"/></node>
          <node id="2Pj" state="4c"><link id="next" target="attract"/></node>
          <node id="2Iy" state="dt"><link id="next" target="2Iz"/></node>
          <node id="2Iz" state="dv"><link id="show" target="2IA"/><link id="skip" target="2In"/></node>
          <node id="2IA" state="du"><link id="next" target="2IB"/></node>
          <node id="2IB" state="mS"><link id="default" target="2In"/></node>
          <node id="2In" state="dP"><link id="next" target="2Io"/></node>
          <node id="2Io" state="jD"><link id="next" target="2Ip"/></node>
          <node id="2Ip" state="dX"><link id="next" target="2Iq"/></node>
          <node id="2Iq" state="ds"><link id="next" target="2Ir"/></node>
          <node id="2Ir" state="j4"><link id="next" target="2Is"/></node>
          <node id="2Is" state="iX"><link id="next" target="2It"/></node>
          <node id="2It" state="iZ"><link id="next" target="1r1"/></node>
          <node id="1r1" state="lb"/>
        </node></flow>''')

    def test_only_logos_and_two_camera_flythroughs_change(self):
        before = startup.signature(self.states)
        flow_before = startup.signature(self.flow)
        result = startup.patch(self.states, self.flow)
        for identity, original_tag in [('4a', 'StateVideoPreStart'), ('4b', 'StateVideoPreStart'),
                                        ('dt', 'StateCutscene'), ('du', 'StateCutscene')]:
            replaced = result.find(f".//StateWayPoint[@id='{identity}']")
            self.assertIsNotNone(replaced)
            self.assertEqual(replaced.attrib, dict(id=identity, timeDelay='0.0'))
            replaced.tag = original_tag
            replaced.attrib.clear()
            replaced.attrib.update(self.states.find(f".//{original_tag}[@id='{identity}']").attrib)
        self.assertEqual(startup.signature(result), before)
        self.assertEqual(startup.signature(self.states), before)
        self.assertEqual(startup.signature(self.flow), flow_before)

    def test_changed_or_ambiguous_movies_are_refused(self):
        for mutation in ('missing', 'duplicate', 'filename', 'extra-attribute', 'child', 'text'):
            with self.subTest(mutation=mutation):
                states = copy.deepcopy(self.states)
                movie = states.find(".//StateVideoPreStart[@id='4a']")
                if mutation == 'missing': states[0].remove(movie)
                if mutation == 'duplicate': states[0].append(copy.deepcopy(movie))
                if mutation == 'filename': movie.set('filename', 'video/other.bik')
                if mutation == 'extra-attribute': movie.set('condition', 'unknown')
                if mutation == 'child': ET.SubElement(movie, 'condition')
                if mutation == 'text': movie.text = 'unknown'
                with self.assertRaises(ValueError): startup.patch(states, self.flow)

    def test_changed_startup_graph_is_refused(self):
        for mutation in ('duplicate', 'state', 'target', 'extra-link', 'destination'):
            with self.subTest(mutation=mutation):
                flow = copy.deepcopy(self.flow)
                node = flow.find(".//node[@id='2Q4']")
                if mutation == 'duplicate': flow.append(copy.deepcopy(node))
                if mutation == 'state': node.set('state', 'other')
                if mutation == 'target': node[0].set('target', 'other')
                if mutation == 'extra-link': ET.SubElement(node, 'link', id='error', target='other')
                if mutation == 'destination': flow.find(".//node[@id='2Pw']").set('state', 'other')
                with self.assertRaises(ValueError): startup.patch(self.states, flow)

    def test_intro_skip_preserves_setup_countdown_and_full_cleanup(self):
        result = startup.patch(self.states, self.flow)
        for identity in ('ds', 'dO', 'dz', '4c'):
            before = next(node for node in self.states.iter() if node.get('id') == identity)
            after = next(node for node in result.iter() if node.get('id') == identity)
            self.assertEqual(startup.signature(after), startup.signature(before))
        for identity in ('dt', 'du', 'ds', 'dO', 'dz'):
            states = copy.deepcopy(self.states)
            node = next(node for node in states.iter() if node.get('id') == identity)
            node.set('unexpected', 'changed')
            with self.subTest(state=identity), self.assertRaises(ValueError): startup.patch(states, self.flow)
        for identity in ('2Iy', '2IA', '2In', '2Io', '2Ip', '2Iq', '2Ir', '2Is', '2It'):
            flow = copy.deepcopy(self.flow)
            flow.find(f".//node[@id='{identity}']/link").set('target', 'bypassed-cleanup')
            with self.subTest(node=identity), self.assertRaises(ValueError): startup.patch(self.states, flow)
        flow = copy.deepcopy(self.flow)
        ET.SubElement(flow, 'node', id='unrelated', state='dt')
        with self.assertRaises(ValueError): startup.patch(self.states, flow)

    def test_roundtrip_protects_unrelated_states(self):
        expected = startup.patch(self.states, self.flow)
        actual = copy.deepcopy(expected)
        startup.verify_roundtrip(expected, actual)
        actual.find(".//StateVideoPreStart[@id='4c']").set('filename', 'changed')
        with self.assertRaises(ValueError): startup.verify_roundtrip(expected, actual)

    def test_bundle_uses_guarded_hashes_not_manifest_claims(self):
        with tempfile.TemporaryDirectory(prefix='startup-test-', dir=ROOT/'build') as folder:
            root = Path(folder); game = root/'game'; bundle = root/'bundle'
            (game/'system').mkdir(parents=True); bundle.mkdir()
            cases = {'SUPPORTED_AVX_SHA256': (game/'grid2_avx.exe', b'executable'),
                     'FLOW_SHA256': (game/'system/flow.bin', b'flow'),
                     'STATES_SHA256': (game/'system/states.bin', b'original'),
                     'MODIFIED_SHA256': (bundle/'states.bin', b'modified')}
            for path, contents in cases.values(): path.write_bytes(contents)
            hashes = {key: hashlib.sha256(contents).hexdigest() for key, (_, contents) in cases.items()}
            with mock_patch.multiple(startup, **hashes):
                data = startup.manifest(game)
                (bundle/'manifest.json').write_text(json.dumps(data), encoding='utf-8')
                self.assertEqual(startup.verify(game, bundle), data)
                (bundle/'states.bin').write_bytes(b'foreign')
                data['modifiedStatesSha256'] = hashlib.sha256(b'foreign').hexdigest()
                (bundle/'manifest.json').write_text(json.dumps(data), encoding='utf-8')
                with self.assertRaises(ValueError): startup.verify(game, bundle)

    @unittest.skipUnless(os.name == 'nt', 'Windows atomic replacement fixture')
    def test_transaction_interruption_and_foreign_file_recovery(self):
        with tempfile.TemporaryDirectory(prefix='startup-test-', dir=ROOT/'build') as folder:
            fixture = Path(folder)
            script = fixture/'recovery.ps1'
            script.write_text(r'''
param([string]$Helper,[string]$Fixture)
$ErrorActionPreference='Stop'
. $Helper
# Synthetic assets only. Suppress real process enumeration in this isolated test.
function Get-Process { param($Name,$ErrorAction) }
$original=[Text.Encoding]::UTF8.GetBytes('original startup fixture')
$modified=[Text.Encoding]::UTF8.GetBytes('modified startup fixture')
$executable=[Text.Encoding]::UTF8.GetBytes('supported executable fixture')
function Hash-Bytes([byte[]]$Bytes) { return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant() }
$script:StartupOriginalHash=Hash-Bytes $original
$script:StartupModifiedHash=Hash-Bytes $modified
$script:StartupExecutableHash=Hash-Bytes $executable
# Fixture-only executable validator; preserves the transaction's changed-file checks.
function Get-Grid2ExecutableHash([string]$Path,[string]$Variant='Avx') {
    $actual=(Get-FileHash -LiteralPath $Path -ErrorAction Stop).Hash.ToLowerInvariant()
    if($actual -ne $script:StartupExecutableHash) { throw 'Changed fixture executable.' }
    return $actual
}

$legacy=[Text.Encoding]::UTF8.GetBytes('old logo-only startup fixture')
$script:StartupLegacyModifiedHash=Hash-Bytes $legacy
function New-Case([string]$Name) {
    $game=Join-Path $Fixture "$Name/game"; $run=Join-Path $Fixture "$Name/run"
    New-Item -ItemType Directory -Path (Join-Path $game 'system'),$run | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $game 'system/states.bin'),$original)
    [IO.File]::WriteAllBytes((Join-Path $game 'grid2_avx.exe'),$executable)
    [IO.File]::WriteAllBytes((Join-Path $run 'startup-movies.bin'),$modified)
    return @{game=$game;run=$run;paths=(Get-StartupMoviesPaths $game $run)}
}
function Assert-Rejected([scriptblock]$Action,[string]$Label) {
    $before=@(Get-ChildItem -LiteralPath $Fixture -Recurse -File | ForEach-Object { $_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash })
    $rejected=$false
    try { & $Action } catch { $rejected=$true }
    if(!$rejected) { throw "$Label was accepted" }
    $after=@(Get-ChildItem -LiteralPath $Fixture -Recurse -File | ForEach-Object { $_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash })
    if(Compare-Object $before $after) { throw "$Label mutated a fixture before rejection" }
}
foreach($phase in @('before-install','after-install','install-temp','restore-temp')) {
    $case=New-Case $phase; $entry=New-StartupMoviesDeployment $case.game $case.run
    if($phase -in @('after-install','restore-temp')) { Install-StartupMovies $case.game $case.run $entry }
    if($phase -eq 'install-temp') { [IO.File]::WriteAllBytes($case.paths.temporary,$modified) }
    if($phase -eq 'restore-temp') { [IO.File]::WriteAllBytes($case.paths.temporary,$original) }
    Restore-StartupMovies $case.game $case.run $entry
    Restore-StartupMovies $case.game $case.run $entry
    Assert-StartupHash $case.paths.target $script:StartupOriginalHash
    Assert-StartupHash $case.paths.backup $script:StartupOriginalHash
    if(Test-Path -LiteralPath $case.paths.temporary) { throw "Temporary retained in $phase" }
}
foreach($changed in @('target','backup','temporary')) {
    $case=New-Case ('foreign-'+$changed); $entry=New-StartupMoviesDeployment $case.game $case.run
    Install-StartupMovies $case.game $case.run $entry
    [IO.File]::WriteAllText($case.paths[$changed],'foreign or incomplete data')
    Assert-Rejected { Restore-StartupMovies $case.game $case.run $entry } "Foreign $changed"
}
foreach($changed in @('target','source','backup','executable','temporary')) {
    $case=New-Case ('preinstall-'+$changed); $entry=New-StartupMoviesDeployment $case.game $case.run
    [IO.File]::WriteAllText($case.paths[$changed],'changed after preparation')
    Assert-Rejected { Install-StartupMovies $case.game $case.run $entry } "Changed $changed before install"
}
$case=New-Case 'missing-backup'; $entry=New-StartupMoviesDeployment $case.game $case.run
Remove-Item -LiteralPath $case.paths.backup
Assert-Rejected { Restore-StartupMovies $case.game $case.run $entry } 'Missing backup'
$case=New-Case 'forged-entry'; $entry=New-StartupMoviesDeployment $case.game $case.run
$entry.name='../foreign.bin'
Assert-Rejected { Restore-StartupMovies $case.game $case.run $entry } 'Escaping entry'
$entry.name='system/states.bin';$entry.sha256='0'*64
Assert-Rejected { Restore-StartupMovies $case.game $case.run $entry } 'Forged entry hash'
$case=New-Case 'bundle-copy'; $bundle=Join-Path $Fixture 'bundle'
New-Item -ItemType Directory -Path $bundle | Out-Null
[IO.File]::WriteAllBytes((Join-Path $bundle 'states.bin'),$modified)
Remove-Item -LiteralPath $case.paths.source
$entry=New-StartupMoviesDeployment $case.game $case.run $bundle
Install-StartupMovies $case.game $case.run $entry
Restore-StartupMovies $case.game $case.run $entry
Assert-StartupHash $case.paths.target $script:StartupOriginalHash
$case=New-Case 'legacy-recovery'; $entry=New-StartupMoviesDeployment $case.game $case.run
$entry.sha256=$script:StartupLegacyModifiedHash
Assert-Rejected { Install-StartupMovies $case.game $case.run $entry } 'Installing legacy logo-only bundle'
[IO.File]::WriteAllBytes($case.paths.target,$legacy)
[IO.File]::WriteAllBytes($case.paths.temporary,$legacy)
Restore-StartupMovies $case.game $case.run $entry
Assert-StartupHash $case.paths.target $script:StartupOriginalHash
if(Test-Path -LiteralPath $case.paths.temporary) { throw 'Legacy temporary retained' }
Write-Output 'Startup movie transaction fixtures passed.'
''', encoding='utf-8')
            shell = Path(os.environ.get('ProgramFiles', r'C:\Program Files'))/'PowerShell/7/pwsh.exe'
            result = subprocess.run([str(shell), '-NoLogo', '-NoProfile', '-NonInteractive', '-File', str(script),
                                     '-Helper', str(ROOT/'tools/Startup-Movies.ps1'), '-Fixture', str(fixture)],
                                    text=True, capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(result.returncode, 0, result.stdout+'\n'+result.stderr)
            self.assertIn('transaction fixtures passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
