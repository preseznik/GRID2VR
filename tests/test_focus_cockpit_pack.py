"""Offline integration checks using an explicit local game and Focus pack.

Run with --game-root and --pack-root. Only temporary workspace output is written.
"""
import argparse, hashlib, json, shutil, subprocess, tempfile
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--game-root',type=Path,required=True)
p.add_argument('--pack-root',type=Path,required=True)
p.add_argument('--receipt',type=Path,required=True)
args=p.parse_args()
root=Path(__file__).resolve().parents[1]
game=args.game_root.resolve();pack=args.pack_root.resolve()
source=game/'cars/models/foc/livery_user/textures_high/foc_tex_high_user.pssg'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
before=sha(source)
tests=[]
base=root/'build/focus-cockpit-pack-tests'
base.mkdir(exist_ok=True)
temp=Path(tempfile.mkdtemp(prefix='case-',dir=base)).resolve()
assert temp.is_relative_to(base.resolve())
try:
    def run(name,pack_path,output,expected,ok=False,game_path=game):
        command=['pwsh','-NoProfile','-File',str(root/'tools/Prepare-FocusCockpitPack.ps1'),
                 '-GameRoot',str(game_path),'-PackDirectory',str(pack_path),'-OutputDirectory',str(output)]
        result=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
        text=result.stdout+result.stderr
        assert (result.returncode==0)==ok,(name,text[-2500:])
        assert expected in text,(name,text[-2500:])
        tests.append(dict(name=name,passed=True))
    valid=temp/'prepared'
    run('valid pack changes only cabin',pack,valid,'"onlyCabinChanged": true',ok=True)
    receipt=json.loads((valid/'prepared.json').read_text(encoding='utf-8-sig'))
    assert receipt['sourceUnchanged'] and receipt['samplerAttributesPreserved'] and receipt['ddsPayloadVerified']
    original_output=sha(valid/'foc_tex_high_user.pssg')
    run('existing output retained',pack,valid,'Output already exists')
    assert sha(valid/'foc_tex_high_user.pssg')==original_output
    run('installed game output refused',pack,game/'GRID2VR-focus-pack-test-never-created','Prepare outside the installed game')
    assert not (game/'GRID2VR-focus-pack-test-never-created').exists()
    run('distributable pack output refused',pack,pack/'generated-never-created','outside the distributable pack')
    assert not (pack/'generated-never-created').exists()
    copied=temp/'pack';shutil.copytree(pack,copied)
    dds=copied/'foc_cabin.dds';data=bytearray(dds.read_bytes());data[128]^=1;dds.write_bytes(data)
    run('tampered texture refused',copied,temp/'tampered-output','Changed or missing cockpit camera input')
    assert not (temp/'tampered-output').exists()
    manifest=json.loads((copied/'pack.json').read_text());manifest['textureSha256']=sha(dds)
    data=dds.read_bytes()[:-8];dds.write_bytes(data);manifest['textureSha256']=sha(dds)
    (copied/'pack.json').write_text(json.dumps(manifest))
    run('truncated mip chain refused',copied,temp/'truncated-output','complete 2048 x 2048 BC1 DDS')
    assert not (temp/'truncated-output').exists()
    manifest['texture']='../foc_cabin.dds';(copied/'pack.json').write_text(json.dumps(manifest))
    run('manifest path override refused',copied,temp/'path-output','Unsupported Focus proof-of-concept manifest')
    assert not (temp/'path-output').exists()
    fake_game=temp/'unsupported-game';fake_game.mkdir();(fake_game/'grid2_avx.exe').write_bytes(b'unsupported fixture')
    run('unsupported executable refused',pack,temp/'unsupported-output','Changed or missing cockpit camera input',game_path=fake_game)
    assert not (temp/'unsupported-output').exists()
    assert sha(source)==before
    args.receipt.parent.mkdir(parents=True,exist_ok=True)
    args.receipt.write_text(json.dumps(dict(tests=tests,sourceSha256=before,sourceUnchanged=True,
        cleanup='temporary fixture folder removed after checks'),indent=2)+'\n')
    print(f'{len(tests)} Focus pack checks passed; original game texture unchanged.')
finally:
    assert temp.resolve().is_relative_to(base.resolve()) and temp.name.startswith('case-')
    shutil.rmtree(temp)
