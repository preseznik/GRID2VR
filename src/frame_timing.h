#pragma once
#include <windows.h>
#include <d3d11.h>
#include <filesystem>

namespace grid2vr::frametiming {
// Session performance summary for VR: stereo FPS windows, render-thread time
// blocked in xrWaitFrame and Present, GPU usage, and non-blocking D3D11
// timestamps around the center scene and both eye replays. Timestamp queries
// change no pipeline binding and results are read without waiting. Every
// call is ignored unless made on the thread that started the summary.
enum Point : unsigned {FrameBegin,CenterBegin,CenterEnd,RightBegin,RightEnd,FrameEnd,PointCount};
#ifdef GRID2VR_ENABLE_XR_SESSION
bool Start(ID3D11Device* device,const std::filesystem::path& output,unsigned headsetWidth,unsigned headsetHeight,unsigned scale);
LONGLONG Now();
void Mark(Point point);
void Wait(LONGLONG started,LONGLONG periodNanoseconds);
void Submitted(bool stereo);
// After the native Present returns; started is Now() taken just before it.
void Present(LONGLONG started);
void Stop();
#else
inline LONGLONG Now() {return 0;}
inline void Mark(Point) {}
inline void Present(LONGLONG) {}
#endif
}
