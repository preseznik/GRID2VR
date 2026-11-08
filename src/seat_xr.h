#pragma once
#include "eye_blit.h"
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <memory>

namespace grid2vr::seat {
// Optional independent compositor quad. Never changes the projection views,
// game HUD or eye images. All operations belong to the XR render owner.
class XrPanel {
public:
    XrPanel();
    ~XrPanel();
    bool Initialize(XrSession session,XrSpace local,ID3D11Device* device,NativeEyeDraw draw);
    const XrCompositionLayerQuad* Layer(ID3D11Texture2D* source,const XrPosef& anchor,uint64_t opening);
    bool Enabled() const;
    bool Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
