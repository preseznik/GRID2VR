"""Bounded seat-only edit of an approved GRID2VR BinXml camera asset.

The section layout follows EGO BinXml (string pool, string offsets, 24-byte
elements and 8-byte attributes). Existing records remain ordered and unchanged;
only the six coordinate references get new, appended strings. No game writes.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

from session_config import plain_path


def adjust(source, car, height=0, forward=0, right=0):
    for value in (height, forward, right):
        if type(value) is not int or not -50 <= value <= 50:
            raise ValueError('Seat adjustments must be whole centimetres within -50..50')
    if not 24 <= len(source) <= 512*1024:
        raise ValueError('Invalid camera size')
    def word(offset): return struct.unpack_from('<I',source,offset)[0]
    if word(0)!=0x7252221a or word(4)!=len(source)-8 or word(8)!=0x72522217:
        raise ValueError('Invalid camera header')
    cursor=16
    def section(magic, stride=1):
        nonlocal cursor
        if cursor+8>len(source) or word(cursor)!=magic:
            raise ValueError('Invalid camera section')
        size=word(cursor+4); cursor+=8
        if size%stride or cursor+size>len(source): raise ValueError('Invalid camera section size')
        data=source[cursor:cursor+size];cursor+=size
        return data
    pool=section(0x7252221d)
    locations=list(struct.unpack('<'+'I'*(len(data:=section(0x7252221e,4))//4),data))
    if word(12)!=cursor-16 or not locations or len(locations)>10000:
        raise ValueError('Invalid camera strings')
    strings=[]
    for offset in locations:
        if offset>=len(pool) or (offset and pool[offset-1]!=0): raise ValueError('Invalid string offset')
        end=pool.find(b'\0',offset)
        if end<0: raise ValueError('Unterminated camera string')
        strings.append(pool[offset:end].decode('utf-8',errors='strict'))
    elements=list(struct.iter_unpack('<6I',section(0x7252221b,24)))
    attributes=[list(row) for row in struct.iter_unpack('<2I',section(0x7252221c,8))]
    if cursor!=len(source) or not elements or len(elements)>20000:
        raise ValueError('Invalid camera records')
    for name,value,ac,at,cc,ch in elements:
        if name>=len(strings) or value>=len(strings) or at+ac>len(attributes) or ch+cc>len(elements):
            raise ValueError('Invalid camera element reference')
    if any(n>=len(strings) or v>=len(strings) for n,v in attributes): raise ValueError('Invalid attribute reference')
    def attrs(index):
        row=elements[index];ids=range(row[3],row[3]+row[2])
        result={strings[attributes[i][0]]:i for i in ids}
        if len(result)!=row[2]: raise ValueError('Duplicate camera attribute')
        return result
    def get(index,name):
        i=attrs(index).get(name)
        return None if i is None else strings[attributes[i][1]]
    def children(index):
        row=elements[index];return range(row[5],row[5]+row[4])
    managers=[i for i in children(0) if strings[elements[i][0]]=='ViewManager' and get(i,'name')==car]
    if strings[elements[0][0]]!='VehicleViewDefinition' or len(managers)!=1:
        raise ValueError('Wrong car camera manager')
    changed=0
    for name in ('bonnet','bonnet_reverse'):
        views=[i for i in children(managers[0]) if get(i,'ident')==name]
        if len(views)!=1 or get(views[0],'type')!='Mounted': raise ValueError('Missing mounted cockpit')
        offsets=[i for i in children(views[0]) if get(i,'name')=='offset' and get(i,'type')=='vector3']
        if len(offsets)!=1: raise ValueError('Missing cockpit offset')
        node=offsets[0]
        for axis,delta in zip('xyz',(-right,height,forward)):
            value=float(get(node,axis))
            if not math.isfinite(value) or abs(value)>4: raise ValueError('Invalid base seat')
            if delta:
                # Append instead of editing shared strings (other cameras can use them).
                value_text=f'{value+delta/100:.4f}'
                strings.append(value_text)
                attributes[attrs(node)[axis]][1]=len(strings)-1
                changed+=1
    if not changed: return source
    pool=bytearray();locations=[]
    for value in strings:
        locations.append(len(pool));pool.extend(value.encode('utf-8')+b'\0')
    pool.extend(b'\0'*((-len(pool))%16))
    def block(magic,data): return struct.pack('<II',magic,len(data))+data
    values=block(0x7252221d,pool)+block(0x7252221e,struct.pack('<'+'I'*len(locations),*locations))
    rest=block(0x7252221b,b''.join(struct.pack('<6I',*row) for row in elements))+block(0x7252221c,b''.join(struct.pack('<2I',*row) for row in attributes))
    return struct.pack('<IIII',0x7252221a,8+len(values)+len(rest),0x72522217,len(values))+values+rest


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True);p.add_argument('--sha256',required=True)
    p.add_argument('--car',required=True)
    for axis in ('height','forward','right'): p.add_argument('--'+axis,type=int,default=0)
    p.add_argument('--output',type=Path)
    a=p.parse_args();source=plain_path(a.source).read_bytes()
    if hashlib.sha256(source).hexdigest()!=a.sha256.lower(): raise ValueError('Approved camera changed')
    result=adjust(source,a.car,a.height,a.forward,a.right)
    if a.output:
        target=plain_path(a.output)
        with target.open('xb') as f: f.write(result)
    print(json.dumps(dict(sha256=hashlib.sha256(result).hexdigest(),bytes=len(result))))
