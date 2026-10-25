import copy
from pathlib import Path
import struct
import sys
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from cockpit_alignment import adjust


def encode(tree):
    strings=['']; elements=[]; attributes=[]
    def string(value):
        if value not in strings: strings.append(value)
        return strings.index(value)
    nodes=[tree]
    for node in nodes:
        start=len(nodes); nodes.extend(node)
        elements.append((string(node.tag),0,len(node.attrib),len(attributes),len(node),start))
        attributes.extend((string(k),string(v)) for k,v in node.attrib.items())
    pool=bytearray();locations=[]
    for value in strings: locations.append(len(pool));pool.extend(value.encode()+b'\0')
    pool.extend(b'\0'*((-len(pool))%16))
    def block(magic,data): return struct.pack('<II',magic,len(data))+data
    text=block(0x7252221d,pool)+block(0x7252221e,struct.pack('<'+'I'*len(locations),*locations))
    data=block(0x7252221b,b''.join(struct.pack('<6I',*r) for r in elements))+block(0x7252221c,b''.join(struct.pack('<2I',*r) for r in attributes))
    return struct.pack('<4I',0x7252221a,8+len(text)+len(data),0x72522217,len(text))+text+data


def decode(data):
    # Independent reader used to compare complete tree semantics.
    poollen=struct.unpack_from('<I',data,20)[0];pool=data[24:24+poollen]
    pos=24+poollen;count=struct.unpack_from('<I',data,pos+4)[0]//4
    offsets=struct.unpack_from('<'+'I'*count,data,pos+8)
    strings=[pool[o:pool.index(b'\0',o)].decode() for o in offsets]
    pos+=8+4*count;size=struct.unpack_from('<I',data,pos+4)[0]
    nodes=list(struct.iter_unpack('<6I',data[pos+8:pos+8+size]));pos+=8+size
    size=struct.unpack_from('<I',data,pos+4)[0];attrs=list(struct.iter_unpack('<2I',data[pos+8:pos+8+size]))
    def node(index):
        n,_,ac,a,cc,c=nodes[index]
        result=ET.Element(strings[n],{strings[k]:strings[v] for k,v in attrs[a:a+ac]})
        result.extend(node(i) for i in range(c,c+cc));return result
    return node(0)


class AlignmentTests(unittest.TestCase):
    def setUp(self):
        self.tree=ET.fromstring('''<VehicleViewDefinition><ViewManager name="s20">
        <View ident="bonnet" type="Mounted"><Parameter name="offset" type="vector3" x="0.35" y="1.05" z="-0.5"/><Parameter name="fov" value="1.05"/></View>
        <View ident="chase_close" type="Chase"><Parameter name="offset" type="vector3" x="0.35" y="1.05" z="-0.5"/></View>
        <View ident="bonnet_reverse" type="Mounted"><Parameter name="offset" type="vector3" x="0.35" y="1.05" z="-0.5"/></View>
        </ViewManager></VehicleViewDefinition>''')
        self.source=encode(self.tree)

    def test_zero_is_byte_identical(self):
        self.assertEqual(adjust(self.source,'s20'),self.source)

    def test_only_two_offsets_change_even_with_shared_strings(self):
        actual=decode(adjust(self.source,'s20',height=-10,forward=12,right=8))
        expected=copy.deepcopy(decode(self.source))
        for name in ('bonnet','bonnet_reverse'):
            expected.find(f".//View[@ident='{name}']/Parameter[@name='offset']").attrib.update(x='0.2700',y='0.9500',z='-0.3800')
        self.assertEqual(ET.tostring(actual),ET.tostring(expected))

    def test_bounds_wrong_car_and_malformed_data(self):
        for value in [51,-51,1.5,True,None]:
            with self.assertRaises(ValueError): adjust(self.source,'s20',height=value)
        with self.assertRaises(ValueError): adjust(self.source,'pag',height=1)
        for data in [b'',self.source[:-1],b'bad!'+self.source[4:],self.source+b'x']:
            with self.assertRaises(ValueError): adjust(data,'s20',height=1)
        for height in [-50,50]:
            self.assertEqual(float(decode(adjust(self.source,'s20',height=height)).find('.//Parameter').get('y')),1.05+height/100)

    def test_missing_duplicate_nonfinite_offset_refused(self):
        self.tree[0][0][0].set('y','nan')
        with self.assertRaises(ValueError): adjust(encode(self.tree),'s20',height=1)
        self.tree[0][0][0].set('y','1.05');self.tree[0].append(copy.deepcopy(self.tree[0][0]))
        with self.assertRaises(ValueError): adjust(encode(self.tree),'s20',height=1)


if __name__=='__main__': unittest.main()
