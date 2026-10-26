"""Prove package-local runtime selection, including refusal to fall back globally."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def verify(package, output, game):
    stage=package.resolve()/'stage';mod=stage/'GRID2VR'
    output=output.resolve();output.mkdir(parents=True,exist_ok=True)
    runtime=mod/'payload/dotnet'
    assert (runtime/'shared/Microsoft.NETCore.App/10.0.13/coreclr.dll').is_file()
    assert not (mod/'payload/powershell/coreclr.dll').exists()
    assert (mod/'payload/powershell/ref/System.Runtime.dll').is_file()
    config=json.loads((mod/'payload/powershell/pwsh.runtimeconfig.json').read_text())
    assert 'includedFrameworks' not in config['runtimeOptions']
    env=dict(os.environ,DOTNET_ROOT=str(output/'absent'),DOTNET_ROOT_X64=str(output/'absent'),
             DOTNET_MULTILEVEL_LOOKUP='0',DOTNET_DISABLE_GUI_ERRORS='1',
             DOTNET_HOST_TRACE='1',COREHOST_TRACE='1',
             PSModulePath=str(mod/'payload/powershell/Modules'),PYTHONNOUSERSITE='1',PYTHONDONTWRITEBYTECODE='1')
    env['PATH']=str(Path(os.environ['SystemRoot'])/'System32')
    env.pop('PYTHONPATH',None);env.pop('PYTHONHOME',None)
    results=[]

    def run(name,args,expected=0,environment=None):
        trace=output/(name+'-host.txt')
        child=dict(environment or env,DOTNET_HOST_TRACEFILE=str(trace),COREHOST_TRACEFILE=str(trace))
        result=subprocess.run([str(a) for a in args],capture_output=True,text=True,timeout=90,env=child)
        (output/(name+'.txt')).write_text(result.stdout+'\n'+result.stderr,encoding='utf-8')
        assert (result.returncode==0)==(expected==0),(name,result.returncode,result.stdout,result.stderr)
        results.append(dict(name=name,exitCode=result.returncode))
        return trace

    launcher_trace=run('launcher',[stage/'GRID2VR.exe','--check-install','--game',game,'--error-log',output/'launcher-error.txt'])
    script=output/'powershell-proof.ps1'
    script.write_text('''param($Mod)
$ErrorActionPreference='Stop'
. (Join-Path $Mod 'tools/Game-Executable.ps1')
Add-Type 'public static class PackageCompilerProof { public static int Value=17; }'
if([PackageCompilerProof]::Value -ne 17) { throw 'Compiler failed.' }
Add-Type -AssemblyName System.Windows.Forms
$core=[object].Assembly.Location
$desktop=[System.Windows.Forms.Form].Assembly.Location
if(!$core.StartsWith((Join-Path $Mod 'payload/dotnet/shared/')) -or !$desktop.StartsWith((Join-Path $Mod 'payload/dotnet/shared/'))) { throw 'External .NET runtime was loaded.' }
Get-Process -Id $PID | Out-Null
Get-FileHash -LiteralPath (Join-Path $Mod 'package.json') | Out-Null
@{core=$core;desktop=$desktop;version=[Environment]::Version.ToString()} | ConvertTo-Json
''',encoding='utf-8')
    run('powershell',[mod/'payload/powershell/pwsh.exe','-NoProfile','-NonInteractive','-File',script,mod])
    converter_env=dict(env,DOTNET_ROOT=str(runtime),DOTNET_ROOT_X64=str(runtime)) # Paths.ps1 sets these for every worker.
    converter_trace=run('converter',[mod/'payload/xml-convert/xml-convert.exe','decode',game/'system/flow.bin',output/'flow.xml'],environment=converter_env)
    for trace in (launcher_trace,converter_trace):
        text=trace.read_text(encoding='utf-8-sig').replace('\\','/').lower()
        assert str(runtime).replace('\\','/').lower()+'/shared/microsoft.netcore.app/10.0.13/coreclr.dll' in text,trace
    with tempfile.TemporaryDirectory(prefix='missing-runtime-',dir=output) as temporary:
        scratch=Path(temporary);assert scratch.resolve().parent==output
        shutil.copyfile(stage/'GRID2VR.exe',scratch/'GRID2VR.exe')
        ps=scratch/'GRID2VR/payload/powershell';ps.mkdir(parents=True)
        for name in ('pwsh.exe','pwsh.dll','pwsh.runtimeconfig.json','pwsh.deps.json'):
            shutil.copyfile(mod/'payload/powershell'/name,ps/name)
        fallback=dict(env,DOTNET_ROOT='C:/Program Files/dotnet',DOTNET_ROOT_X64='C:/Program Files/dotnet')
        run('launcher-refuses-global',[scratch/'GRID2VR.exe','--check-install','--game',game],expected=1,environment=fallback)
        run('powershell-refuses-global',[ps/'pwsh.exe','-NoProfile','-NonInteractive','-Command','exit 0'],expected=1,environment=fallback)
    manifest=json.loads((mod/'package.json').read_text())
    for name,expected in manifest['files'].items():
        assert hashlib.sha256((stage/name).read_bytes()).hexdigest()==expected,name
    receipt=dict(passed=True,package=str(package),checks=results,privateRuntime=str(runtime),
                 globalFallbackRefused=True,stageUnchanged=True,gameLaunched=False)
    (output/'runtime-verification.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--game-root',type=Path,default=Path(r'E:\SteamLibrary\steamapps\common\grid 2'))
    args=parser.parse_args();verify(args.package,args.output,args.game_root)
