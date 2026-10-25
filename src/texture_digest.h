#pragma once
#include <d3d11.h>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace grid2vr {
struct AlphaCoverageDigest {
    const char* status{"not_requested"};
    uint64_t texels{}, atLeastHalf{}, belowHalf{};
};
struct SubresourceDigest {
    UINT mip{}, slice{}, rowBytes{}, rows{};
    std::string sha256;
    AlphaCoverageDigest alpha;
};
struct TextureDigest {
    const char* status{"unbound"};
    D3D11_TEXTURE2D_DESC desc{};
    uint64_t bytes{};
    std::vector<SubresourceDigest> subresources;
    bool alphaCoverageRequested{};
};
// Whole-mip, unfiltered alpha >= 0.5; not a measurement of object UV coverage or
// visible pixels. BC3 interpolated alpha is compared before eight-bit rounding.
// The mapped storage must contain the described rows.
AlphaCoverageDigest CountTextureAlpha(DXGI_FORMAT format,const D3D11_MAPPED_SUBRESOURCE& mapped,UINT width,UINT height);
// Hash tight texel/block rows, excluding driver padding. No resolve, conversion,
// binding changes or pointer-based cache; repeated calls observe later updates.
TextureDigest DigestTexture(ID3D11DeviceContext* context,ID3D11Resource* resource,uint64_t& budget,bool alphaCoverage=false);
void WriteTextureDigest(std::ostream& out,const TextureDigest& digest);
}
