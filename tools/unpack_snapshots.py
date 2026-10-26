"""Validate a bounded native snapshot archive before writing any CPU fixtures."""
import hashlib
import json
import os
import re
import struct
from pathlib import Path

NAME=re.compile(r'(?:stage-\d+-[A-Za-z0-9-]+|engine-\d+-camera-(?:before|after))\.bin')

def read_archive(run):
    archive=run/'cpu-snapshots.bin'
    if not archive.exists():return None
    if archive.is_symlink() or archive.stat().st_size>64*1024*1024:raise ValueError('Invalid snapshot archive size/link')
    data=archive.read_bytes()
    if data[:8]!=b'G2VRSNP1':raise ValueError('Invalid snapshot header')
    position=8;records={};count=0
    while position<len(data):
        if len(data)-position<4:raise ValueError('Truncated snapshot record')
        name_size,size=struct.unpack_from('<HH',data,position);position+=4
        if not 1<=name_size<=95 or not 1<=size<=512 or position+name_size+size>len(data):raise ValueError('Invalid snapshot record length')
        name=data[position:position+name_size].decode('ascii');position+=name_size
        if not NAME.fullmatch(name):raise ValueError('Invalid snapshot filename')
        value=data[position:position+size];position+=size;count+=1
        # Native snapshots previously used truncating writes for repeated names.
        # Retain the final record, exactly matching that existing behavior.
        records[name]=value
    return data,records,count


def unpack(run):
    archive=read_archive(run)
    if archive is None:return None
    data,records,count=archive
    for name,value in records.items():
        path=run/name
        if path.is_symlink() or (path.exists() and (not path.is_file() or path.read_bytes()!=value)):
            raise ValueError('Existing snapshot differs; retained: '+name)
    for name,value in records.items():
        path=run/name
        if not path.exists():
            with path.open('xb') as stream:stream.write(value)
    receipt=dict(archiveSha256=hashlib.sha256(data).hexdigest(),records=count,files=len(records),bytes=sum(map(len,records.values())))
    (run/'cpu-snapshots-unpacked.json').write_text(json.dumps(receipt,indent=2)+'\n')
    return receipt


def retire_expanded(run):
    # Finish-Diagnostic already verifies game/process recovery. Also constrain
    # this deletion helper to an owned legacy or grouped run directory.
    root=Path(os.environ.get('GRID2VR_DATA_ROOT', str(Path(__file__).resolve().parents[1]/'artifacts')))
    resolved=run.resolve()
    legacy=resolved.parent==root.resolve()
    grouped=resolved.parent==root.resolve()/'logs' and re.fullmatch(r'\d{8}-\d{6}-\d{3}', resolved.name)
    if not (legacy or grouped) or any(p.exists() and (p.is_symlink() or getattr(p.lstat(),'st_file_attributes',0)&0x400) for p in (run,*run.parents)):
        raise ValueError('Snapshot retirement escapes owned session logs')
    archive=read_archive(run)
    if archive is None:return None
    data,records,count=archive
    sha=hashlib.sha256(data).hexdigest()
    cleanup=json.loads((run/'capture-cleanup.json').read_text(encoding='utf-8-sig'))
    unpacked=json.loads((run/'cpu-snapshots-unpacked.json').read_text())
    if cleanup.get('status')!='complete' or unpacked.get('archiveSha256')!=sha:
        raise ValueError('Snapshot retirement requires completed cleanup and the exact unpacked archive')
    def check(path,value):
        if path.is_symlink() or (path.exists() and (getattr(path.lstat(),'st_file_attributes',0)&0x400 or path.resolve().parent!=resolved or not path.is_file() or path.read_bytes()!=value)):
            raise ValueError('Changed or linked expanded snapshot retained: '+path.name)
    for name,value in records.items():check(run/name,value)
    removed=0
    for name,value in records.items():
        path=run/name;check(path,value)
        if path.exists():path.unlink();removed+=1
    receipt=dict(archiveSha256=sha,records=count,recoverableFiles=len(records),removedThisPass=removed,status='complete')
    (run/'cpu-snapshots-retired.json').write_text(json.dumps(receipt,indent=2)+'\n')
    return receipt

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path)
    parser.add_argument('--retire-expanded',action='store_true');args=parser.parse_args()
    print(retire_expanded(args.run) if args.retire_expanded else unpack(args.run))
