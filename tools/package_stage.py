"""Build a bounded player package. No personal data or game-derived assets are shipped."""
import argparse, ast, hashlib, json, shutil, sys, zipfile
from pathlib import Path

ROOT=Path(__file__).resolve().parent.parent
DATA_FILES=['cockpit-assets.json','cockpit-cars.json','cockpit-defaults.json','car-classes.json','dlc-content.json']
SCRIPTS=['Paths','Setup-Package','Start-UserSession','Run-StereoProbe','Run-Diagnostic','Write-DiagnosticReceipt','Save-BenchmarkReports','User-Session','Session-Control','Finish-Diagnostic','Profiles','Cockpit-Camera','Cockpit-Assets','Recenter-Device','Recenter-Key','Headset-Settings','OpenXR-Runtime','Practice-Flow','Native-Startup','Startup-Movies','Cinema-Matte','Instance-Capacity','Driving-Controls','Flicker-Automation','Launcher-VrPreview']
SCRIPTS += ['Borderless-Desktop']
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def copy(source,target):
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
def tree(source,target,ignore=()):
    shutil.copytree(source,target,ignore=shutil.ignore_patterns(*ignore))
def stage(a):
    out=a.output.resolve();stage=out/'stage';mod=stage/'GRID2VR';payload=mod/'payload';payload.mkdir(parents=True)
    copy(out/'publish/GRID2VR.exe',stage/'GRID2VR.exe')
    (stage/'Start-GRID2VR.cmd').write_text('@echo off\nstart "" "%~dp0GRID2VR.exe"\n',encoding='ascii')
    copy(ROOT/'build/win32/bin/RelWithDebInfo/d3d11.dll',payload/'d3d11.dll')
    copy(ROOT/'build/driving-input-vs-x64/Release/driving_input.dll',payload/'driving_input.dll')
    for name in SCRIPTS:copy(ROOT/f'tools/{name}.ps1',mod/f'tools/{name}.ps1')
    # Analysis modules are imported on demand by bounded post-run cleanup.
    for p in (ROOT/'tools').glob('*.py'):
        if p.name.startswith(('analyze_','observe_')) or p.name in ['session_config.py','graphics_profile.py','practice_entry.py','startup_movies.py','cockpit_assets.py','cockpit_camera.py','cockpit_alignment.py','steamvr_theater.py','unpack_snapshots.py','summarize_diagnostic.py','recorded_views.py','dynamic_views.py']:
            copy(p,mod/'tools'/p.name)
    # Include local imports even when they are used only during post-run recovery.
    pending=list((mod/'tools').glob('*.py'));included={p.stem for p in pending}
    while pending:
        source=pending.pop()
        for node in ast.walk(ast.parse(source.read_text(encoding='utf-8-sig'))):
            names=[node.module] if isinstance(node,ast.ImportFrom) else [n.name for n in node.names] if isinstance(node,ast.Import) else []
            for name in names:
                name=(name or '').split('.')[0];dependency=ROOT/'tools'/(name+'.py')
                if name not in included and dependency.is_file():
                    target=mod/'tools'/dependency.name;copy(dependency,target);included.add(name);pending.append(target)
    for name in ['stereo-probe-settings.xml','practice-settings.xml']:
        copy(ROOT/'tools'/name,mod/'tools'/name)
    for name in DATA_FILES:copy(ROOT/'tools/data'/name,mod/'tools/data'/name)
    copy(ROOT/'README.md',mod/'README.md');copy(ROOT/'CHANGELOG.md',mod/'CHANGELOG.md')
    tree(ROOT/'licenses',mod/'licenses')
    tree(Path('C:/Program Files/PowerShell/7'),payload/'powershell',('*.pdb','*.xml','ref','SDK','en-US'))
    py=a.python.parent
    for p in py.iterdir():
        if p.is_file() and (p.suffix in ['.exe','.dll'] or p.name=='LICENSE.txt'):copy(p,payload/'python'/p.name)
    tree(py/'Lib',payload/'python/Lib',('site-packages','__pycache__','test','tests','idlelib','tkinter','ensurepip','turtledemo'))
    tree(py/'DLLs',payload/'python/DLLs',('*.pdb','_tkinter.pyd','tcl*.dll','tk*.dll'))
    for p in (py/'Lib/site-packages').iterdir():
        if p.is_dir() and (p.name in ['numpy','numpy.libs','PIL','pillow.libs'] or p.name.startswith(('numpy-','pillow-'))):
            tree(p,payload/'python/Lib/site-packages'/p.name,('__pycache__','tests'))
    # Preserve the proven converter's binary identity and all managed dependencies.
    converter=ROOT.parent/'DiRT2VR/artifacts/xml-convert'
    tree(converter,payload/'xml-convert',('*.pdb',))
    dotnet=Path('C:/Program Files/dotnet');runtime='10.0.13'
    copy(dotnet/'dotnet.exe',payload/'dotnet/dotnet.exe')
    for name in ['LICENSE.txt','ThirdPartyNotices.txt']:copy(dotnet/name,payload/'dotnet'/name)
    tree(dotnet/'host/fxr'/runtime,payload/'dotnet/host/fxr'/runtime)
    tree(dotnet/'shared/Microsoft.NETCore.App'/runtime,payload/'dotnet/shared/Microsoft.NETCore.App'/runtime)
    redist=Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Redist/MSVC/v143')
    for arch in ['x86','x64']:copy(redist/f'vc_redist.{arch}.exe',payload/f'prerequisites/vc_redist.{arch}.exe')
    for source,target in [(ROOT/'.deps/OpenXR-SDK/LICENSE','OpenXR-Apache-2.0.txt'),(ROOT.parent/'DiRT2VR/artifacts/xml-convert/EGO-LICENSE.txt','EGO-MIT.txt')]:copy(source,mod/'licenses'/target)
    copy(ROOT/'.deps/OpenXR-SDK/LICENSES/Apache-2.0.txt',mod/'licenses/Apache-2.0.txt')
    copy(ROOT.parent/'DiRT2VR/licenses/MiscUtil.txt',mod/'licenses/MiscUtil.txt')
    # Keep the original NuGet metadata/notices with the unchanged converter dependencies.
    packages={'bcnencoder.net':'2.2.1','zstdsharp.port':'0.7.2','sharpgltf.core':'1.0.6','sharpgltf.runtime':'1.0.6','sharpgltf.toolkit':'1.0.6','communitytoolkit.highperformance':'8.4.0','system.io.hashing':'10.0.11','system.io.packaging':'10.0.11','sixlabors.imagesharp':'2.1.13','k4os.compression.lz4':'1.3.8'}
    for name,version in packages.items():
        folder=Path.home()/'.nuget/packages'/name/version;target=mod/'licenses/converter'/name
        copy(folder/(name+'.nuspec'),target/(name+'.nuspec'))
        with zipfile.ZipFile(folder/(name+'.'+version+'.nupkg')) as z:
            for entry in z.namelist():
                if any(word in entry.lower() for word in ['license','notice','copying']) and not entry.endswith('/'):
                    target.mkdir(parents=True,exist_ok=True);(target/Path(entry).name).write_bytes(z.read(entry))
    copy(Path('C:/Program Files/dotnet/LICENSE.txt'),mod/'licenses/Microsoft-MIT.txt')
    sources={str(p.relative_to(ROOT)).replace('\\','/'):sha(p) for folder in ['launcher','src','tools','installer'] for p in (ROOT/folder).rglob('*') if p.is_file() and not any(x in p.parts for x in ['obj','bin','__pycache__'])}
    manifest={'schema':1,'version':a.version,'channel':a.channel,'builtUtc':a.built,'revision':a.revision,'sourceDigest':hashlib.sha256(json.dumps(sources,sort_keys=True).encode()).hexdigest(),'files':{p.relative_to(stage).as_posix():sha(p) for p in sorted(stage.rglob('*')) if p.is_file()}}
    (mod/'package.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    (out/'source-manifest.json').write_text(json.dumps(sources,indent=2)+'\n')
    print(json.dumps({'stage':str(stage),'files':len(manifest['files']),'bytes':sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())}))
def finalize(out):
    out=out.resolve();stage=out/'stage';manifest=json.loads((stage/'GRID2VR/package.json').read_text())
    for name,expected in manifest['files'].items():
        assert sha(stage/name)==expected,name
    version=manifest['version'];archive=out/f'GRID2VR-{version}.zip'
    with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for p in sorted(stage.rglob('*')):
            if p.is_file():z.write(p,p.relative_to(stage).as_posix())
    setup=out/f'GRID2VR-{version}-Setup.exe';assert setup.is_file()
    (out/'SHA256SUMS.txt').write_text(''.join(f'{sha(p)}  {p.name}\n' for p in [setup,archive]),encoding='ascii')
    print(json.dumps({'installer':str(setup),'zip':str(archive),'channel':manifest['channel'],'published':False}))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--python',type=Path);p.add_argument('--version');p.add_argument('--channel');p.add_argument('--revision');p.add_argument('--built');p.add_argument('--finalize',type=Path);a=p.parse_args()
    finalize(a.finalize) if a.finalize else stage(a)
