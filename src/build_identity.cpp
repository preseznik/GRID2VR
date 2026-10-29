#include "build_identity.h"

// Only this file receives the per-commit definition, so a new commit rebuilds
// one translation unit instead of the whole renderer.
#ifndef GRID2VR_SOURCE_REVISION
#define GRID2VR_SOURCE_REVISION "unversioned"
#endif

namespace grid2vr {
namespace {
constexpr char prefix[]="grid2vr_source_revision=";
constexpr char marker[]="grid2vr_source_revision=" GRID2VR_SOURCE_REVISION;
}
const char* SourceRevision() noexcept {return marker+sizeof(prefix)-1;}
}
