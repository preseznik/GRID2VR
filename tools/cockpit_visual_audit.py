"""Blender background reconstruction for offline cockpit alignment review.

Untextured installed LOD0 geometry. Glass and alternate wiper poses omitted.
Not a game-render, animation, material, or headset acceptance test.
Run Blender --background --factory-startup --python this.py -- --meshes DIR --output NEW_DIR.
"""
import json,base64,math,sys
from pathlib import Path
import bpy
from mathutils import Matrix,Vector
import numpy as np
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--meshes',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args(sys.argv[sys.argv.index('--')+1:])
ROOT=args.meshes.resolve()
OUT=args.output.resolve()
PROJECT=Path(__file__).resolve().parent.parent
if not any(OUT.is_relative_to(PROJECT/p) for p in ('build','docs/evidence')):
 raise ValueError('Evidence must stay under the workspace build or docs/evidence folder')
if OUT.exists():raise ValueError('Choose a new evidence directory')
OUT.mkdir(parents=True)
CAT=json.loads((PROJECT/'tools/data/cockpit-defaults.json').read_text())
def children(n,t):return [x for x in n.get('children',[]) if x['type']==t]
def data(n):return base64.b64decode(n['data'])
def load(car):
 d=json.loads((ROOT/f'{car}_highLOD.json').read_text()); ids={}; world={}; nodes=[]
 expected=next(r for r in CAT['cars'] if r['car']==car)
 if d['sha256']!=expected['meshSha256']:raise ValueError('Unreviewed mesh hash')
 def visit(n,parent):
  local=children(n,'TRANSFORM'); mat=Matrix(np.frombuffer(data(local[0]),dtype='>f4').reshape(4,4).T.tolist()) if local else Matrix.Identity(4)
  mat=parent@mat
  if 'id' in n['attrs']:ids[n['attrs']['id']]=n;world[n['attrs']['id']]=mat
  nodes.append(n)
  for x in n.get('children',[]):visit(x,mat)
 visit(d['root'],Matrix.Identity(4)); meshes=[]
 for n in nodes:
  if n['type']!='MATRIXPALETTEJOINTNODE' or not n['attrs']['id'].startswith('x0_'):continue
  name=n['attrs']['id']; mat=world[name]
  for inst in children(n,'MATRIXPALETTEJOINTRENDERINSTANCE'):
   a=inst['attrs']; rds=ids[a['indices'][1:]]; ri=children(rds,'RENDERINDEXSOURCE')[0]
   assert ri['attrs']['primitive']=='triangles' and ri['attrs']['format']=='ushort'
   ix=np.frombuffer(data(children(ri,'INDEXSOURCEDATA')[0]),dtype='>u2').astype(int)
   ix=ix[a['indexOffset']:a['indexOffset']+a['indicesCountFromOffset']]
   streams=[]
   for s in children(rds,'RENDERSTREAM'):
    b=ids[s['attrs']['dataBlock'][1:]]; desc=children(b,'DATABLOCKSTREAM')[s['attrs']['subStream']]['attrs']
    if desc['renderType']=='Vertex':
     assert desc['dataType']=='float3'; raw=data(children(b,'DATABLOCKDATA')[0])
     vs=np.ndarray((b['attrs']['elementCount'],3),dtype='>f4',buffer=raw,offset=desc['offset'],strides=(desc['stride'],4)).astype(float)
     streams.append(vs)
   assert len(streams)==1
   verts=streams[0];start=a['streamOffset'];count=a['elementCountFromOffset']
   assert min(ix)>=start and max(ix)<start+count,(car,name,min(ix),max(ix),start,count)
   verts=verts[start:start+count];ix=ix-start
   v4=np.column_stack((verts,np.ones(len(verts)))); transformed=(np.array(mat)@v4.T).T[:,:3]
   # Reject incorrect space using authored local joint bounds.
   box=children(n,'BOUNDINGBOX')[0];bounds=np.frombuffer(data(box),dtype='>f4')
   assert np.all(verts.min(0)>=bounds[:3]-.002) and np.all(verts.max(0)<=bounds[3:]+.002),(car,name,'bounds')
   shader=a['shader'][1:];meshes.append((name,shader,transformed,ix.reshape(-1,3)))
 return meshes,d['sha256']
def render(car,pos,tag):
 bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
 for mesh in list(bpy.data.meshes):
  if mesh.users==0:bpy.data.meshes.remove(mesh)
 meshes,sha=load(car)
 for name,shader,v,ix in meshes:
  if 'glass' in shader.lower():continue
  if any(s in name for s in ['_pos_01','_pos_02']):continue # alternate wiper poses
  verts=[(-x,z,y) for x,y,z in v]
  m=bpy.data.meshes.new(name);m.from_pydata(verts,[],ix.tolist());m.update()
  o=bpy.data.objects.new(name,m);bpy.context.collection.objects.link(o)
  color=(.23,.25,.28,1) if shader=='Cabin' else (.42,.46,.5,1)
  if 'steering' in name:color=(.12,.14,.17,1)
  if 'Matt'==shader:color=(.15,.17,.19,1)
  o.color=color
 bpy.ops.object.camera_add(location=(-pos[0],pos[2],pos[1]))
 cam=bpy.context.object;cam.rotation_euler=(math.pi/2,0,0);cam.data.type='PERSP';cam.data.lens=18;cam.data.sensor_width=36;cam.data.clip_start=.01;cam.data.clip_end=100
 scene=bpy.context.scene;scene.camera=cam;scene.render.engine='BLENDER_WORKBENCH'
 scene.render.resolution_x=768;scene.render.resolution_y=512;scene.render.resolution_percentage=100
 sh=scene.display.shading;sh.light='STUDIO';sh.studio_light='paint.sl';sh.color_type='OBJECT';sh.show_shadows=True;sh.show_cavity=True;sh.cavity_type='BOTH';sh.background_type='WORLD';scene.world.color=(.32,.5,.7)
 scene.view_settings.view_transform='Standard';scene.render.image_settings.file_format='PNG';scene.render.filepath=str(OUT/f'{car}-{tag}.png')
 bpy.ops.render.render(write_still=True)
 return dict(car=car,sourceSha256=sha,position=pos,meshes=len(meshes),triangles=sum(len(x[3]) for x in meshes))

from mathutils.bvhtree import BVHTree

def sightlines(car,pos):
 meshes,_=load(car);verts=[];faces=[]
 for name,shader,v,ix in meshes:
  if 'glass' in shader.lower() or any(s in name for s in ['_pos_01','_pos_02']):continue
  start=len(verts);verts.extend(v.tolist());faces.extend((ix+start).tolist())
 bvh=BVHTree.FromPolygons(verts,faces,all_triangles=True)
 def hit(pitch,yaw=0,dx=0):
  a,b=map(math.radians,[pitch,yaw]);origin=Vector((pos[0]+dx,pos[1],pos[2]))
  direction=Vector((math.sin(b)*math.cos(a),math.sin(a),math.cos(b)*math.cos(a)))
  return bvh.ray_cast(origin,direction,4)[3]
 return dict(upperDegrees=[next((p*.5 for p in range(141) if hit(p*.5,yaw) is not None),None) for yaw in [-10,0,10]],
  lowerDegrees=next((p*.5 for p in range(0,-141,-1) if hit(p*.5) is not None),None),
  roofAboveMeters=hit(90),eyeForwardClear=[hit(0,dx=dx) is None for dx in [-.032,0,.032]])

receipts=[]
for row in CAT['cars']:
 before=list(row['derivedPosition']);after=list(before);after[1]=round(after[1]+row['heightCentimeters']/100,4)
 r=render(row['car'],before,'before');r['name']=row['name'];r['review']=row['review'];r['heightCentimeters']=row['heightCentimeters']
 r['beforeSightlines']=sightlines(row['car'],before)
 render(row['car'],after,'after');r['afterPosition']=after;r['afterSightlines']=sightlines(row['car'],after)
 receipts.append(r)
(OUT/'geometry-review.json').write_text(json.dumps(dict(schema=1,method=__doc__,cars=receipts),indent=2))
