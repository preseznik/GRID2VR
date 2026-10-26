"""Offline cross-language regression against local signed files; never executes them."""
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


def run(args):
    results = []
    sources = list(args.game_root.glob('grid2*.exe'))
    if args.samples:
        sources += list(args.samples.rglob('grid2*.exe'))
    sources = [p for p in sources if p.name in ('grid2.exe', 'grid2_avx.exe')]
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
        for path in sources[:2]:
            avx = path.name == 'grid2_avx.exe'
            original = path.read_bytes()
            # Includes excluded regions: their corruption must fail Authenticode even
            # though normalization alone would accept them. Imports stay fingerprinted.
            offsets = dict(code=0x1000, data=0xd8ee00 if avx else 0xd7ce00,
                imports=0xd89394-0xe00 if avx else 0xd7757c-0x1000,
                layout=0x180, dos_stub=0x50, steam_data=0xbb9368 if avx else 0xba716c,
                steam_instance=0xdf005c if avx else 0xdde05c, signature=len(original)-128)
            for name, offset in offsets.items():
                data = bytearray(original); data[offset] ^= 1
                candidate = folder/(path.stem+'-'+name+'.exe'); candidate.write_bytes(data)
                cases.append(dict(path=str(candidate), avx=avx, accepted=False, name=path.stem+' '+name))
            for name, data in [('truncated', original[:512]), ('unsigned', original[:0xef8800 if avx else 0xee7800]),
                               ('overlay', original+b'extra')]:
                candidate = folder/(path.stem+'-'+name+'.exe'); candidate.write_bytes(data)
                cases.append(dict(path=str(candidate), avx=avx, accepted=False, name=path.stem+' '+name))
        if not sources:
            candidate = folder/'invalid.exe'; candidate.write_bytes(b'MZ'+bytes(1022))
            cases.append(dict(path=str(candidate), avx=True, accepted=False, name='malformed'))
        case_file = folder/'cases.json'; case_file.write_text(json.dumps(cases), encoding='utf-8')
        script = folder/'validate.ps1'
        script.write_text('''param($Source,$Cases)
$ErrorActionPreference='Stop'
. $Source
$rows=@(foreach($case in (Get-Content -LiteralPath $Cases -Raw | ConvertFrom-Json)) {
    try {
        $hash=Get-Grid2ExecutableHash $case.path $(if($case.avx){'Avx'}else{'Regular'})
        @{name=$case.name;accepted=$true;sha256=$hash}
    } catch { @{name=$case.name;accepted=$false;error=$_.Exception.Message} }
})
ConvertTo-Json -InputObject $rows -Compress
''', encoding='utf-8')
        command = [shutil.which('pwsh'), '-NoProfile', '-NonInteractive', '-File', str(script),
                   '-Source', str(ROOT/'tools/Game-Executable.ps1'), '-Cases', str(case_file)]
        output = subprocess.check_output(command, text=True, timeout=90)
        managed = json.loads(output)
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
            # A second legitimate Steam executable must invalidate a prepared session,
            # even though both executables belong to the same supported build.
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
                        raise AssertionError('Changed Steam variant accepted an old session')
                results.append(dict(language='python', name='changed legitimate Steam variant invalidates session', accepted=False))
        if args.launcher:
            for game in sorted({p.parent for p in sources}):
                if all((game/name).is_file() for name in ('grid2.exe','grid2_avx.exe')):
                    result = subprocess.run([str(args.launcher), '--check-install', '--game', str(game)],
                                            capture_output=True, text=True, timeout=90)
                    assert result.returncode == 0, (game, result.stderr)
                    results.append(dict(language='launcher', name=str(game), accepted=True))
    assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == sha for p, sha in originals.items())
    report = dict(signedInputs=len(sources), checks=len(results), preservedInputs=originals, results=results,
                  runtimeTested=False, temporaryCopiesRemoved=True)
    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(f"Executable validation: {len(results)} checks passed; {len(sources)} signed inputs preserved; temporary copies removed.")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path)
    parser.add_argument('--launcher', type=Path)
    parser.add_argument('--game-root', type=Path, default=Path(r'E:\SteamLibrary\steamapps\common\grid 2'))
    parser.add_argument('--samples', type=Path)
    parser.add_argument('--receipt', type=Path)
    run(parser.parse_args())
