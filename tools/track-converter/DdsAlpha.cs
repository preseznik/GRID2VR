// Adapted from DiRT2VR tools/nordschleife-converter/DdsAlpha.cs (a617f7a), same author.
using System.Buffers.Binary;
using EgoEngineLibrary.Graphics.Dds;

namespace GRID2VR.Tracks;

internal static class DdsAlpha
{
    internal static void Prepare(DdsFile file,bool opaque)
    {
        if(opaque)Opaque(file);else ExpandLuminance(file,false);
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
    internal static void Opaque(DdsFile file)
    {
        uint format=file.header.ddspf.fourCC;
        // EGO needs explicit RGB channels for AC's luminance formats.
        if(ExpandLuminance(file,true))return;
        if(format is 0x33545844 or 0x35545844) // DXT3 / DXT5
        {
            if(file.bdata.Length%16!=0)throw new InvalidDataException("Incomplete opaque DDS block.");
            for(int i=0;i<file.bdata.Length;i+=16)
            {
                var alpha=file.bdata.AsSpan(i,8);
                if(format==0x33545844)alpha.Fill(255);
                else {alpha.Clear();alpha[0]=255;alpha[1]=255;}
            }
        }
        else if(format==0&&file.header.ddspf.rGBBitCount==32)
        {
            uint mask=file.header.ddspf.aBitMask;
            if(file.bdata.Length%4!=0)throw new InvalidDataException("Incomplete opaque DDS pixel.");
            for(int i=0;i<file.bdata.Length;i+=4)
            {
                var pixel=file.bdata.AsSpan(i,4);
                BinaryPrimitives.WriteUInt32LittleEndian(pixel,BinaryPrimitives.ReadUInt32LittleEndian(pixel)|mask);
            }
        }
        else if(format!=0x31545844)throw new InvalidDataException("Unsupported opaque DDS format.");
    }
}
