#include "flicker_pixels.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>

namespace grid2vr::flicker {
namespace {
using Microsoft::WRL::ComPtr;
unsigned PixelBytes(DXGI_FORMAT f) {
    switch(f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R16G16B16A16_UNORM:return 8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return 4;
    default:return 0;
    }
}
bool SameRgb(const unsigned char* a,const unsigned char* b,DXGI_FORMAT format) {
    if(format==DXGI_FORMAT_R10G10B10A2_UNORM) {
        uint32_t x{},y{};memcpy(&x,a,4);memcpy(&y,b,4);return (x&0x3fffffffu)==(y&0x3fffffffu);
    }
    const auto bytes=PixelBytes(format);
    return !memcmp(a,b,bytes==8?6:format==DXGI_FORMAT_R11G11B10_FLOAT?4:3);
}
bool Copy(ID3D11DeviceContext* c,ID3D11Texture2D* from,ComPtr<ID3D11Texture2D>& to) {
    D3D11_TEXTURE2D_DESC desc{};from->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Device> device;from->GetDevice(&device);
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&to)))return false;
    c->CopyResource(to.Get(),from);return true;
}
}
void PixelWitness::Reset() {
    target_.Reset();before_.Reset();after_.Reset();desc_={};status_="not_captured";
}
void PixelWitness::Before(ID3D11DeviceContext* c) {
    Reset();
    if(!c || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) {status_="context";return;}
    ComPtr<ID3D11RenderTargetView> view;c->OMGetRenderTargets(1,&view,nullptr);
    if(!view) {status_="unbound";return;}
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    if(FAILED(resource.As(&target_))) {status_="resource";return;}
    target_->GetDesc(&desc_);const auto bytes=PixelBytes(desc_.Format);
    if(!bytes || !desc_.Width || !desc_.Height || desc_.ArraySize!=1 || desc_.MipLevels!=1 ||
       desc_.SampleDesc.Count!=1 || uint64_t(desc_.Width)*desc_.Height*bytes>8ull*1024*1024) {status_="unsupported";return;}
    status_=Copy(c,target_.Get(),before_)?"before":"allocation";
}
void PixelWitness::After(ID3D11DeviceContext* c) {
    if(strcmp(status_,"before"))return;
    status_=Copy(c,target_.Get(),after_)?"after":"allocation";
}
std::string PixelWitness::Finish(ID3D11DeviceContext* c,ID3D11Texture2D* finalSource) {
    std::ostringstream s;
    s << "\"target\":" << uintptr_t(target_.Get()) << ",\"width\":" << desc_.Width << ",\"height\":" << desc_.Height
      << ",\"format\":" << desc_.Format << ",\"finalSourceSame\":" << (finalSource && finalSource==target_.Get()?"true":"false");
    D3D11_TEXTURE2D_DESC finalDesc{};if(finalSource)finalSource->GetDesc(&finalDesc);
    s << ",\"finalSource\":[" << uintptr_t(finalSource) << ',' << finalDesc.Width << ',' << finalDesc.Height << ',' << finalDesc.Format << ']';
    ComPtr<ID3D11Texture2D> finalTarget;
    if(!strcmp(status_,"after")) {
        if(!c || !Copy(c,target_.Get(),finalTarget))status_="allocation";
        else {
            const std::array<ID3D11Texture2D*,3> textures{before_.Get(),after_.Get(),finalTarget.Get()};
            std::array<D3D11_MAPPED_SUBRESOURCE,3> mapped{};unsigned count{};
            const auto bytes=PixelBytes(desc_.Format);
            for(auto* texture:textures) {
                if(FAILED(c->Map(texture,0,D3D11_MAP_READ,0,&mapped[count])))break;
                ++count;
            }
            bool valid=count==3;
            for(unsigned i=0;i<count;++i)valid&=mapped[i].pData && mapped[i].RowPitch>=desc_.Width*bytes;
            status_=valid?"ok":"map";
            if(valid) {
                unsigned changed{},retained{},reverted{},other{},runs{};
                unsigned minX=desc_.Width,minY=desc_.Height,maxX{},maxY{};
                bool truncated=false;std::ostringstream mask;
                for(unsigned y=0;y<desc_.Height;++y) {
                    std::array<const unsigned char*,3> row{};
                    for(unsigned i=0;i<3;++i)row[i]=static_cast<const unsigned char*>(mapped[i].pData)+size_t(y)*mapped[i].RowPitch;
                    unsigned start=desc_.Width;
                    for(unsigned x=0;x<=desc_.Width;++x) {
                        const auto at=size_t(x)*bytes;
                        const bool written=x<desc_.Width && !SameRgb(row[0]+at,row[1]+at,desc_.Format);
                        if(written) {
                            ++changed;minX=(std::min)(minX,x);minY=(std::min)(minY,y);maxX=(std::max)(maxX,x+1);maxY=(std::max)(maxY,y+1);
                            if(SameRgb(row[1]+at,row[2]+at,desc_.Format))++retained;
                            else if(SameRgb(row[0]+at,row[2]+at,desc_.Format))++reverted;
                            else ++other;
                            if(start==desc_.Width)start=x;
                        } else if(start!=desc_.Width) {
                            if(runs<4096) {if(runs)mask<<',';mask << '[' << y << ',' << start << ',' << x-start << ']';}
                            else truncated=true;
                            ++runs;start=desc_.Width;
                        }
                    }
                }
                if(!changed)minX=minY=0;
                s << ",\"changedPixels\":" << changed << ",\"retainedPixels\":" << retained << ",\"revertedPixels\":" << reverted << ",\"otherPixels\":" << other
                  << ",\"bounds\":[" << minX << ',' << minY << ',' << maxX << ',' << maxY << "]"
                  << ",\"maskTruncated\":" << (truncated?"true":"false") << ",\"maskRuns\":[" << mask.str() << ']';
            }
            for(unsigned i=0;i<count;++i)c->Unmap(textures[i],0);
        }
    }
    s << ",\"status\":\"" << status_ << '"';Reset();return s.str();
}
}
