"""Build a bounded player package. No personal data or game-derived assets are shipped."""
import argparse, ast, hashlib, json, shutil, subprocess, sys, zipfile
from pathlib import Path
import release_assets

ROOT=Path(__file__).resolve().parent.parent
DATA_FILES=['cockpit-assets.json','cockpit-cars.json','cockpit-defaults.json','car-classes.json','dlc-content.json']
SCRIPTS=['Paths','Setup-Package','Start-UserSession','Run-StereoProbe','Run-Diagnostic','Write-DiagnosticReceipt','Save-BenchmarkReports','User-Session','Session-Control','Finish-Diagnostic','Profiles','Cockpit-Camera','Cockpit-Assets','Recenter-Device','Recenter-Key','Headset-Settings','OpenXR-Runtime','Practice-Flow','Native-Startup','Startup-Movies','Cinema-Matte','Instance-Capacity','Driving-Controls','Flicker-Automation','Launcher-VrPreview']
SCRIPTS += ['Borderless-Desktop', 'Game-Executable', 'Hardware-Settings']
SCRIPTS += ['Focus-CockpitPack', 'Cockpit-TexturePack']
DATA_FILES += ['cockpit-texture-pack.json']
DATA_FILES += ['cockpit-dlc.json']
DATA_FILES += ['graphics-options.json']
SCRIPTS += ['Dlc-Cockpit']
SCRIPTS += ['Launch-Backend']
SCRIPTS += ['Chase-Camera']
SCRIPTS += ['Headset-Resolution', 'Get-VrResolution']
SCRIPTS += ['Device-Preset']
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def copy(source,target):
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
def tree(source,target,ignore=()):
    shutil.copytree(source,target,ignore=shutil.ignore_patterns(*ignore))

def stage_powershell(source, target, runtime):
    """Keep PowerShell's application dependencies; share the two .NET frameworks."""
    config=json.loads((source/'pwsh.runtimeconfig.json').read_text(encoding='utf-8-sig'))
    frameworks=config['runtimeOptions'].pop('includedFrameworks')
    assert {f['name'] for f in frameworks}=={'Microsoft.NETCore.App','Microsoft.WindowsDesktop.App'}
    assert all(f['version']==runtime for f in frameworks), 'PowerShell and private runtime versions differ'
    config['runtimeOptions']['frameworks']=frameworks
    deps=json.loads((source/'pwsh.deps.json').read_text(encoding='utf-8-sig'))
    framework_files=set()
    framework_packages={f"runtimepack.{f['name']}.Runtime.win-x64/{runtime}" for f in frameworks}
    found=set()
    for libraries in deps['targets'].values():
        for name in framework_packages:
            package=libraries.pop(name,None)
            if package is None: continue
            found.add(name)
            for kind in ('runtime','native','resources'):
                framework_files.update(package.get(kind,{}))
        for library in libraries.values():
            for name in framework_packages:
                library.get('dependencies',{}).pop(name.rsplit('/',1)[0],None)
    assert found==framework_packages, 'PowerShell framework dependency metadata is incomplete'
    for name in framework_packages: deps['libraries'].pop(name)
    target.mkdir(parents=True)
    # These are interactive/package-management modules, never used by our workers.
    unused_modules={'PSReadLine','PSDiagnostics','Microsoft.PowerShell.PSResourceGet','PowerShellGet','PackageManagement'}
    for p in source.iterdir():
        if p.is_file() and p.name not in framework_files and p.name not in ('pwsh.exe','pwsh.deps.json','pwsh.runtimeconfig.json') and p.suffix not in ('.pdb','.xml'):
            copy(p,target/p.name)
        elif p.name=='ref':
            tree(p,target/p.name,('*.xml',)) # Add-Type's reference assemblies are required.
        elif p.name=='Modules':
            tree(p,target/p.name,tuple(unused_modules)+('*.pdb','*.xml','en-US'))
    (target/'pwsh.runtimeconfig.json').write_text(json.dumps(config,indent=2)+'\n')
    (target/'pwsh.deps.json').write_text(json.dumps(deps,indent=2)+'\n')
    package_apphost(target/'pwsh.exe','pwsh.dll',source/'pwsh.exe',runtime)

def package_apphost(output, assembly, resources, runtime):
    subprocess.run(['pwsh','-NoProfile','-NonInteractive','-File',str(ROOT/'tools/Package-AppHost.ps1'),
                    '-Output',str(output),'-Assembly',assembly,'-Resources',str(resources),
                    '-RuntimeVersion',runtime],check=True)
def stage(a):
    release=release_assets.check_inputs(ROOT,a.output.resolve(),a.version,a.channel,a.native)
    out=a.output.resolve();stage=out/'stage';mod=stage/'GRID2VR';payload=mod/'payload';payload.mkdir(parents=True)
    copy(out/'publish/GRID2VR.exe',stage/'GRID2VR.exe')
    (stage/'Start-GRID2VR.cmd').write_text('@echo off\nstart "" "%~dp0GRID2VR.exe"\n',encoding='ascii')
    copy(release_assets.native_path(ROOT,a.native),payload/'d3d11.dll')
    copy(release_assets.native_path(ROOT,a.native).parent/'xr_runtime_probe.exe',payload/'xr_runtime_probe.exe')
    release_assets.require(sha(payload/'d3d11.dll')==release['nativeSha256'], 'Native renderer changed while staging.')
    copy(ROOT/'build/driving-input-vs-x64/Release/driving_input.dll',payload/'driving_input.dll')
    for name in SCRIPTS:copy(ROOT/f'tools/{name}.ps1',mod/f'tools/{name}.ps1')
    copy(ROOT/'tools/GameExecutable.cs',mod/'tools/GameExecutable.cs')
    copy(ROOT/'tools/HardwareSettingsPath.cs',mod/'tools/HardwareSettingsPath.cs')
    # Analysis modules are imported on demand by bounded post-run cleanup.
    for p in (ROOT/'tools').glob('*.py'):
        if p.name.startswith(('analyze_','observe_')) or p.name in ['session_config.py','graphics_profile.py','practice_entry.py','startup_movies.py','cockpit_assets.py','cockpit_start.py','cockpit_camera.py','cockpit_alignment.py','steamvr_theater.py','unpack_snapshots.py','summarize_diagnostic.py','recorded_views.py','dynamic_views.py']:
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
    notes=(ROOT/'CHANGELOG.md').read_text(encoding='utf-8-sig').split('\n# ',1)[0].removeprefix('# ').strip()+'\n'
    (mod/'ReleaseNotes.txt').write_text(notes,encoding='utf-8-sig')
    (out/'ReleaseNotes.md').write_text(notes.split('\n',1)[1].lstrip(),encoding='utf-8')
    tree(ROOT/'licenses',mod/'licenses')
    runtime='10.0.13'
    stage_powershell(Path('C:/Program Files/PowerShell/7'),payload/'powershell',runtime)
    py=a.python.parent
    for p in py.iterdir():
        if p.is_file() and (p.suffix in ['.exe','.dll'] or p.name=='LICENSE.txt'):copy(p,payload/'python'/p.name)
    tree(py/'Lib',payload/'python/Lib',('site-packages','__pycache__','test','tests','idlelib','tkinter','ensurepip','turtledemo'))
    tree(py/'DLLs',payload/'python/DLLs',('*.pdb','_test*','_ctypes_test.pyd','_tkinter.pyd','tcl*.dll','tk*.dll'))
    for p in (py/'Lib/site-packages').iterdir():
        if p.is_dir() and (p.name in ['numpy','numpy.libs','PIL','pillow.libs'] or p.name.startswith(('numpy-','pillow-'))):
            tree(p,payload/'python/Lib/site-packages'/p.name,('__pycache__','tests'))
    # Preserve the proven converter's binary identity and all managed dependencies.
    converter=ROOT.parent/'DiRT2VR/artifacts/xml-convert'
    tree(converter,payload/'xml-convert',('*.pdb',))
    # Paths.ps1 points this unchanged, fingerprinted converter at the shared runtime.
    dotnet=Path('C:/Program Files/dotnet')
    copy(dotnet/'dotnet.exe',payload/'dotnet/dotnet.exe')
    for name in ['LICENSE.txt','ThirdPartyNotices.txt']:copy(dotnet/name,payload/'dotnet'/name)
    tree(dotnet/'host/fxr'/runtime,payload/'dotnet/host/fxr'/runtime)
    for framework in ['Microsoft.NETCore.App','Microsoft.WindowsDesktop.App']:
        # Neutral resources are sufficient for the English launcher and noninteractive workers.
        for p in (dotnet/'shared'/framework/runtime).iterdir():
            if p.is_file() and p.suffix not in ('.pdb','.xml'):
                copy(p,payload/'dotnet/shared'/framework/runtime/p.name)
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
    manifest.update(releaseTag=release['tag'],cockpitTextures=release['cockpitTextures'])
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
    release=release_assets.finalize(out,manifest,setup,archive)
    print(json.dumps({'installer':str(setup),'zip':str(archive),'textures':release['cockpitTextures'],'channel':manifest['channel'],'published':False}))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--python',type=Path);p.add_argument('--version');p.add_argument('--channel');p.add_argument('--revision');p.add_argument('--built');p.add_argument('--native',type=Path);p.add_argument('--finalize',type=Path);a=p.parse_args()
    finalize(a.finalize) if a.finalize else stage(a)
