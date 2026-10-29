#pragma once

namespace grid2vr {
// Source commit the renderer was built from ("<40 hex>", "<40 hex>-dirty" or
// "unversioned"). Release packaging verifies the embedded marker.
const char* SourceRevision() noexcept;
}
