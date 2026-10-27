"""Offline cross-language regression against supported files; never executes them."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import game_executable
import session_config

STANDALONE = {
    'grid2.exe': 'd8ec0c631b453d6e5e9adebae4cfc9b612823fec316344c28544bc65245ba35d',
    'grid2_avx.exe': '825f30a49a668870c7bfab0309c3afb01a396fc1cec0be8df9764e32bf2fb53f',
}


def run(args):
    results = []
    sources = list(args.game_root.glob('grid2*.exe'))
    if args.samples:
        sources += list(args.samples.rglob('grid2*.exe'))
    if args.standalone_root:
        sources += list(args.standalone_root.glob('grid2*.exe'))
    sources = list(dict.fromkeys(p for p in sources if p.name in ('grid2.exe', 'grid2_avx.exe')))
    originals = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    (ROOT/'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='game-executable-', dir=ROOT/'build') as temporary:
        folder = Path(temporary)
        assert folder.resolve().parent == (ROOT/'build').resolve()
        cases = []
        for path in sources:
            avx = path.name == 'grid2_avx.exe'
            cases.append(dict(path=str(path), avx=avx, accepted=True, name=str(path)))
            cases.append(dict(path=str(path), avx=not avx, accepted=False, name='wrong variant: '+str(path)))
        unique = {originals[str(path)]: path for path in sources}
        for path in unique.values():
            avx = path.name == 'grid2_avx.exe'
            original = path.read_bytes()
            label = path.stem+'-'+originals[str(path)][:8]
            # Includes excluded regions: their corruption must fail Authenticode even
            # though normalization alone would accept them. Imports stay fingerprinted.
            offsets = dict(code=0x1000, data=0xd8ee00 if avx else 0xd7ce00,
                imports=0xd89394-0xe00 if avx else 0xd7757c-0x1000,
                layout=0x180, dos_stub=0x50, steam_data=0xbb9368 if avx else 0xba716c,
                steam_instance=0xdf005c if avx else 0xdde05c, signature=len(original)-128)
            for name, offset in offsets.items():
                data = bytearray(original); data[offset] ^= 1
                candidate = folder/(label+'-'+name+'.exe'); candidate.write_bytes(data)
                cases.append(dict(path=str(candidate), avx=avx, accepted=False, name=label+' '+name))
            malformed = [('truncated', original[:512]), ('truncated-byte', original[:-1]),
                         ('overlay', original+b'extra'), ('oversized', original+bytes(65537))]
            if originals[str(path)] not in STANDALONE.values():
                malformed.append(('unsigned', original[:0xef8800 if avx else 0xee7800]))
            for name, data in malformed:
                candidate = folder/(label+'-'+name+'.exe'); candidate.write_bytes(data)
                cases.append(dict(path=str(candidate), avx=avx, accepted=False, name=label+' '+name))
        if not sources:
            candidate = folder/'invalid.exe'; candidate.write_bytes(b'MZ'+bytes(1022))
            cases.append(dict(path=str(candidate), avx=True, accepted=False, name='malformed'))
        installations = []
        for game in sorted({p.parent for p in sources}):
            if all((game/name).is_file() for name in STANDALONE):
                backend = 'Direct' if originals[str(game/'grid2_avx.exe')] in STANDALONE.values() else 'Steam'
                installations.append(dict(path=str(game), accepted=True, backend=backend, name=str(game)))
        regular = next((p for p in sources if p.name == 'grid2.exe'), None)
        avx = next((p for p in sources if p.name == 'grid2_avx.exe'), None)
        if regular and avx:
            wrong = folder/'wrong-variant'; wrong.mkdir()
            shutil.copyfile(avx, wrong/'grid2.exe'); shutil.copyfile(regular, wrong/'grid2_avx.exe')
            installations.append(dict(path=str(wrong), accepted=False, name='swapped executable variants'))
            missing = folder/'missing-variant'; missing.mkdir(); shutil.copyfile(avx, missing/'grid2_avx.exe')
            installations.append(dict(path=str(missing), accepted=False, name='missing regular executable'))
        for direct_avx in (False, True):
            regular = next((p for p in sources if p.name == 'grid2.exe' and
                            (originals[str(p)] in STANDALONE.values()) != direct_avx), None)
            avx = next((p for p in sources if p.name == 'grid2_avx.exe' and
                        (originals[str(p)] in STANDALONE.values()) == direct_avx), None)
            if regular and avx:
                mixed = folder/('mixed-'+str(direct_avx)); mixed.mkdir()
                shutil.copyfile(regular, mixed/'grid2.exe'); shutil.copyfile(avx, mixed/'grid2_avx.exe')
                installations.append(dict(path=str(mixed), accepted=False, name='mixed families '+str(direct_avx)))
        identities = [dict(identity=value, standalone=True) for value in STANDALONE.values()]
        identities += [dict(identity=value, standalone=False) for value in
                       ('', 'unknown', '519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502',
                        STANDALONE['grid2_avx.exe'].upper(), STANDALONE['grid2_avx.exe']+'0')]
        case_file = folder/'cases.json'
        case_file.write_text(json.dumps(dict(files=cases, installations=installations, identities=identities)), encoding='utf-8')
        script = folder/'validate.ps1'
        script.write_text('''param($Source,$Cases)
$ErrorActionPreference='Stop'
. $Source
$cases=Get-Content -LiteralPath $Cases -Raw | ConvertFrom-Json
$rows=@(foreach($case in $cases.files) {
    try {
        $hash=Get-Grid2ExecutableHash $case.path $(if($case.avx){'Avx'}else{'Regular'})
        @{name=$case.name;accepted=$true;sha256=$hash}
    } catch { @{name=$case.name;accepted=$false;error=$_.Exception.Message} }
})
$installations=@(foreach($case in $cases.installations) {
    try { @{name=$case.name;accepted=$true;backend=(Get-Grid2LaunchBackend $case.path)} }
    catch { @{name=$case.name;accepted=$false;error=$_.Exception.Message} }
})
$identities=@(foreach($case in $cases.identities) {
    @{identity=$case.identity;standalone=[GRID2VR.GameExecutable]::IsStandalone($case.identity)}
})
ConvertTo-Json -InputObject @{files=$rows;installations=$installations;identities=$identities} -Compress -Depth 5
''', encoding='utf-8')
        command = [shutil.which('pwsh'), '-NoProfile', '-NonInteractive', '-File', str(script),
                   '-Source', str(ROOT/'tools/Game-Executable.ps1'), '-Cases', str(case_file)]
        output = subprocess.check_output(command, text=True, timeout=180)
        checks = json.loads(output)
        managed = checks['files']
        for case, result in zip(cases, managed):
            assert result['accepted'] == case['accepted'], (case, result)
            if case['accepted']:
                assert result['sha256'] == originals[case['path']]
            results.append(dict(language='managed/powershell', **result))
            if args.native:
                native = subprocess.run([str(args.native), 'avx' if case['avx'] else 'regular', case['path']],
                                        capture_output=True, text=True, timeout=30)
                assert (native.returncode == 0) == case['accepted'], (case, native.stderr)
                if case['accepted']:
                    assert native.stdout.strip() == originals[case['path']]
                results.append(dict(language='native', name=case['name'], accepted=native.returncode == 0))
        assert len(managed) == len(cases)
        assert len(checks['installations']) == len(installations)
        for case, result in zip(installations, checks['installations']):
            assert result['accepted'] == case['accepted'], (case, result)
            if case['accepted']:
                assert result['backend'] == case['backend'], (case, result)
            elif case['name'].startswith('mixed'):
                assert 'Mixed Steam and standalone' in result['error'], result
            results.append(dict(language='managed/powershell installation', **result))
        for case, result in zip(identities, checks['identities']):
            assert case == result, (case, result)
            results.append(dict(language='managed identity', **result))
            if args.native:
                native = subprocess.run([str(args.native), 'identity', case['identity']],
                                        capture_output=True, text=True, timeout=30)
                assert (native.returncode == 0) == case['standalone'], (case, native)
                results.append(dict(language='native identity', **result))
        for path in sources:
            if path.name != 'grid2_avx.exe':
                continue
            assert game_executable.validate_avx(path) == originals[str(path)]
            results.append(dict(language='python', name=str(path), accepted=True))
        # Hash-keyed validation cache must not accept changed bytes at the same path.
        avx = next((p for p in sources if p.name == 'grid2_avx.exe'), None)
        if avx:
            changing = folder/'changing.exe'; changing.write_bytes(avx.read_bytes())
            game_executable.validate_avx(changing)
            changed = bytearray(changing.read_bytes()); changed[0xbb9368] ^= 1; changing.write_bytes(changed)
            try:
                game_executable.validate_avx(changing)
            except ValueError:
                results.append(dict(language='python', name='changed cached path', accepted=False))
            else:
                raise AssertionError('Changed file reused cached validation')
            # A second supported executable must invalidate a prepared session,
            # even when both executable identities are accepted independently.
            alternate = next((p for p in sources if p.name == avx.name and originals[str(p)] != originals[str(avx)]), None)
            if alternate:
                game = folder/'game'; game.mkdir(); (game/'grid2_avx.exe').write_bytes(avx.read_bytes())
                catalog = dict(gameRoot=str(game))
                with patch.object(session_config, 'catalog', return_value=catalog), \
                     patch.object(session_config, 'configuration', return_value=b'<session/>'), \
                     patch.object(session_config, 'car_content', return_value=dict(dlc=False, cockpitAvailable=True)):
                    prepared = folder/'prepared'
                    session_config.prepare(game, prepared, car='foc', track='cities/dubai/route_0')
                    session_config.verify(game, prepared)
                    (game/'grid2_avx.exe').write_bytes(alternate.read_bytes())
                    try:
                        session_config.verify(game, prepared)
                    except ValueError as error:
                        assert 'fingerprint differs' in str(error), error
                    else:
                        raise AssertionError('Changed executable identity accepted an old session')
                results.append(dict(language='python', name='changed supported executable invalidates session', accepted=False))
        if args.launcher:
            for case in installations:
                result = subprocess.run([str(args.launcher), '--check-install', '--game', case['path']],
                                        capture_output=True, text=True, timeout=90)
                assert (result.returncode == 0) == case['accepted'], (case, result.stderr)
                results.append(dict(language='launcher', name=case['name'], accepted=result.returncode == 0))
    assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == sha for p, sha in originals.items())
    report = dict(supportedInputs=len(sources), checks=len(results), preservedInputs=originals, results=results,
                  runtimeTested=False, temporaryCopiesRemoved=True)
    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(f"Executable validation: {len(results)} checks passed; {len(sources)} supported inputs preserved; temporary copies removed.")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path)
    parser.add_argument('--launcher', type=Path)
    parser.add_argument('--game-root', type=Path, default=Path(r'E:\SteamLibrary\steamapps\common\grid 2'))
    parser.add_argument('--samples', type=Path)
    parser.add_argument('--standalone-root', type=Path)
    parser.add_argument('--receipt', type=Path)
    run(parser.parse_args())
