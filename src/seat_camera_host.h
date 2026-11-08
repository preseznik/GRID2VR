#pragma once
#include "seat_session.h"
#include <functional>
#include <memory>

namespace grid2vr::seat {
// Uses the DLC camera module's pinned native consumers and local owner graph.
// Opaque identities are compared across frames; old pointers are never followed.
class CameraHost {
public:
    CameraHost();
    ~CameraHost();
    CameraHost(const CameraHost&)=delete;
    CameraHost& operator=(const CameraHost&)=delete;
    bool Initialize(uintptr_t base,const SessionRequest& request);
    // Called at Present, after the initial DLC calibration. The editor is only
    // called for the currently selected, identified local cockpit. It returns
    // the absolute settings it wants; both native views are updated together.
    bool Update(const std::function<CameraSettings(size_t car,bool changed)>& edit);
    void Stop();
    const char* Reason() const;
#ifdef GRID2VR_DLC_COCKPIT_TEST
    // Relocated synthetic image: the production fingerprint is still checked.
    bool TestInitialize(uintptr_t base,const SessionRequest& request);
#endif
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
