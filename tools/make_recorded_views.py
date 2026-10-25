"""Generate a bounded desktop-only moving stereo input (native units)."""
import argparse
import math
from pathlib import Path
import numpy as np
from analyze_eye_offset import pose_rotation

def generate(count):
    if not 2<=count<=900:raise ValueError('Use 2..900 frames')
    rows=[]
    for index in range(count):
        t=index/(count-1)*2*math.pi
        axis=np.array([.4,1,.25]);axis/=np.linalg.norm(axis)
        angle=.12*math.sin(t)
        q=np.r_[axis*math.sin(angle/2),math.cos(angle/2)]
        center=np.array([.025*math.sin(t),.015*math.sin(2*t),.02*(math.cos(t)-1)])
        right=pose_rotation(np.r_[q,center])[0];pair=[]
        for eye,sign in enumerate((-1,1)):
            position=center+sign*.032*right
            fov=np.array([(-1,-.8)[eye],(.8,1)[eye],-.7,.6])
            fov*=1+.05*math.sin(t+.3)
            pair.extend(np.r_[q,position,fov])
        rows.append(' '.join(format(float(v),'.9g') for v in np.array(pair,dtype='<f4')))
    return '\n'.join(rows)+'\n'

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path);parser.add_argument('--count',type=int,default=120)
    args=parser.parse_args();args.output.write_text(generate(args.count))
