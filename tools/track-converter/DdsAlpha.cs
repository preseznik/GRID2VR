// Adapted from DiRT2VR tools/nordschleife-converter/DdsAlpha.cs (a617f7a), same author.
using System.Buffers.Binary;
using EgoEngineLibrary.Graphics.Dds;

namespace GRID2VR.Tracks;

internal static class DdsAlpha
{
    // Opaque textures: alpha becomes a constant `spec`. GRID 2's terrain shaders read diffuse
    // alpha as the specular mask (TDiffuseSpecMap1.a); stock Brands Hatch grass is 0-8, tarmac
    // 10-35, painted tarmac ~122. A solid 255 made all converted ground shiny.
    internal static void Prepare(DdsFile file,bool opaque,byte spec=255)
    {
        if(opaque)Opaque(file,spec);else if(!ExpandLuminance(file,false))ExpandRgb24(file);
    }
    // Uncompressed 24-bit RGB (AC layered-ground masks and detail normals) has no EGO texel
    // format: widen to A8R8G8B8 with alpha 255, as DiRT2VR's NordMaterials does.
    static void ExpandRgb24(DdsFile file)
    {
        var f=file.header.ddspf;
        if(f.fourCC!=0||f.rGBBitCount!=24)return;
        if(f.rBitMask!=0xff0000||f.gBitMask!=0xff00||f.bBitMask!=0xff||file.bdata.Length%3!=0)throw new InvalidDataException("Unsupported 24-bit DDS layout.");
        var pixels=new byte[checked(file.bdata.Length/3*4)];
        for(int i=0,o=0;i<file.bdata.Length;i+=3,o+=4){pixels[o]=file.bdata[i];pixels[o+1]=file.bdata[i+1];pixels[o+2]=file.bdata[i+2];pixels[o+3]=255;}
        file.bdata=pixels;file.header.ddspf.flags=DdsPixelFormat.Flags.DDPF_RGB|DdsPixelFormat.Flags.DDPF_ALPHAPIXELS;
        file.header.ddspf.rGBBitCount=32;file.header.ddspf.aBitMask=0xff000000;file.header.pitchOrLinearSize=checked(file.header.width*4);
    }
    // GRID 2 is a 32-bit process; AC ships many 2048-4096 px maps. Drop whole top mip
    // levels (no resampling) until the texture fits. Returns the bytes removed.
    internal static long Shrink(DdsFile file,uint maximum)
    {
        long removed=0;
        uint format=file.header.ddspf.fourCC;
        if(format==0x30315844)return 0; // DX10 header: left as authored
        int blockBytes=format switch{0x31545844=>8,0x33545844 or 0x35545844=>16,0=>0,_=>-1};
        if(blockBytes<0)return 0;
        while((file.header.width>maximum||file.header.height>maximum)&&file.header.mipMapCount>1)
        {
            uint w=file.header.width,h=file.header.height;
            long level=blockBytes>0?(long)Math.Max(1,(w+3)/4)*Math.Max(1,(h+3)/4)*blockBytes:(long)w*h*(file.header.ddspf.rGBBitCount/8);
            if(level<=0||level>=file.bdata.Length)throw new InvalidDataException("Unexpected DDS mip chain.");
            file.bdata=file.bdata[(int)level..];removed+=level;
            file.header.width=Math.Max(1,w/2);file.header.height=Math.Max(1,h/2);file.header.mipMapCount--;
            file.header.pitchOrLinearSize=blockBytes>0?(uint)(Math.Max(1,(file.header.width+3)/4)*Math.Max(1,(file.header.height+3)/4)*blockBytes):file.header.width*(file.header.ddspf.rGBBitCount/8);
        }
        return removed;
    }
    static bool ExpandLuminance(DdsFile file,bool opaque)
    {
        if(file.header.ddspf.fourCC!=0 || (file.header.ddspf.flags&DdsPixelFormat.Flags.DDPF_LUMINANCE)==0 || file.header.ddspf.rGBBitCount is not (8 or 16))return false;
        int stride=checked((int)file.header.ddspf.rGBBitCount/8);
        if(file.header.ddspf.rBitMask!=255 || file.header.ddspf.gBitMask!=0 || file.header.ddspf.bBitMask!=0 || file.header.ddspf.aBitMask is not (0 or 65280) || file.bdata.Length%stride!=0)throw new InvalidDataException("Unsupported luminance DDS layout.");
        var pixels=new byte[checked(file.bdata.Length/stride*4)];
        for(int i=0;i<file.bdata.Length/stride;i++)
        {
            byte l=file.bdata[i*stride];pixels[i*4]=l;pixels[i*4+1]=l;pixels[i*4+2]=l;
            pixels[i*4+3]=!opaque&&stride==2&&file.header.ddspf.aBitMask!=0?file.bdata[i*stride+1]:(byte)255;
        }
        file.bdata=pixels;file.header.ddspf.flags=DdsPixelFormat.Flags.DDPF_RGB|DdsPixelFormat.Flags.DDPF_ALPHAPIXELS;
        file.header.ddspf.rGBBitCount=32;file.header.ddspf.rBitMask=0x00ff0000;file.header.ddspf.gBitMask=0x0000ff00;file.header.ddspf.bBitMask=0x000000ff;file.header.ddspf.aBitMask=0xff000000;
        file.header.pitchOrLinearSize=checked(file.header.width*4);
        return true;
    }
    // AC opaque materials can store blend/specular masks in diffuse alpha. EGO's
    // object shader uses that channel as coverage; preserve compressed RGB, make coverage solid.
    internal static void Opaque(DdsFile file,byte spec=255)
    {
        uint format=file.header.ddspf.fourCC;
        // EGO needs explicit RGB channels for AC's luminance formats.
        if(ExpandLuminance(file,true)){format=0;}
        if(format is 0x33545844 or 0x35545844) // DXT3 / DXT5
        {
            if(file.bdata.Length%16!=0)throw new InvalidDataException("Incomplete opaque DDS block.");
            byte nibble=(byte)((spec+8)/17);
            for(int i=0;i<file.bdata.Length;i+=16)
            {
                var alpha=file.bdata.AsSpan(i,8);
                if(format==0x33545844)alpha.Fill((byte)(nibble|nibble<<4));
                else {alpha.Clear();alpha[0]=spec;alpha[1]=spec;}
            }
        }
        else if(format==0&&file.header.ddspf.rGBBitCount==32)
        {
            uint mask=file.header.ddspf.aBitMask;int shift=System.Numerics.BitOperations.TrailingZeroCount(mask);
            if(mask==0)throw new InvalidDataException("32-bit opaque DDS without an alpha mask.");
            if(file.bdata.Length%4!=0)throw new InvalidDataException("Incomplete opaque DDS pixel.");
            for(int i=0;i<file.bdata.Length;i+=4)
            {
                var pixel=file.bdata.AsSpan(i,4);
                BinaryPrimitives.WriteUInt32LittleEndian(pixel,(BinaryPrimitives.ReadUInt32LittleEndian(pixel)&~mask)|((uint)spec<<shift&mask));
            }
        }
        else if(format==0x31545844) // DXT1: no alpha; a mask below 255 needs DXT5
        {
            if(spec!=255)ToDxt5(file,spec);
        }
        else throw new InvalidDataException("Unsupported opaque DDS format.");
    }

    // DXT1 -> DXT5 with constant alpha, same colour blocks. BC3 colour blocks are always
    // 4-colour; DXT1 3-colour blocks (c0 <= c1) are rewritten: endpoints swapped and indices
    // remapped (midpoint -> nearest third; index 3, transparent black, unused in opaque maps).
    static void ToDxt5(DdsFile file,byte spec)
    {
        if(file.bdata.Length%8!=0)throw new InvalidDataException("Incomplete DXT1 block.");
        var output=new byte[file.bdata.Length*2];
        for(int b=0,o=0;b<file.bdata.Length;b+=8,o+=16)
        {
            output[o]=spec;output[o+1]=spec; // alpha endpoints; indices 0
            var colour=output.AsSpan(o+8,8);file.bdata.AsSpan(b,8).CopyTo(colour);
            ushort c0=BinaryPrimitives.ReadUInt16LittleEndian(colour),c1=BinaryPrimitives.ReadUInt16LittleEndian(colour[2..]);
            if(c0>c1)continue;
            uint indices=BinaryPrimitives.ReadUInt32LittleEndian(colour[4..]),remapped=0;
            if(c0==c1){BinaryPrimitives.WriteUInt32LittleEndian(colour[4..],0);continue;}
            BinaryPrimitives.WriteUInt16LittleEndian(colour,c1);BinaryPrimitives.WriteUInt16LittleEndian(colour[2..],c0);
            for(int p=0;p<16;p++)
            {
                uint index=(indices>>(2*p))&3;
                uint mapped=index switch{0=>1u,1=>0u,2=>2u,_=>1u};
                remapped|=mapped<<(2*p);
            }
            BinaryPrimitives.WriteUInt32LittleEndian(colour[4..],remapped);
        }
        file.bdata=output;file.header.ddspf.fourCC=0x35545844;
        file.header.pitchOrLinearSize*=2;
    }
}
