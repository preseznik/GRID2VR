#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace grid2vr {
constexpr uint64_t CrowdModelShader=0xac1fbbd477a609bdull;
struct ModelDrawIdentity {
    uint64_t shader{}, pixelShader{}, kind{};
    uint32_t drawable{},transform{},vertex{},index{},stride{},vertexOffset{},indexFormat{},indexOffset{},argCount{};
    std::array<uint32_t,5> args{};
    bool operator==(const ModelDrawIdentity&) const = default;
};
struct ModelDrawSnapshot { ModelDrawIdentity identity; std::array<uint8_t,128> constants; };
// This is an opt-in causal experiment for one identified shader's model field.
// Preserve every other constant, including any view-dependent data.
inline bool PrepareModelSnapshot(const ModelDrawSnapshot& original,const ModelDrawSnapshot& current,
    const std::array<uint8_t,64>& nativeModel,std::array<uint8_t,128>& output) {
    if (original.identity.shader!=CrowdModelShader || original.identity!=current.identity ||
        !current.identity.drawable || !current.identity.transform || !current.identity.argCount ||
        current.identity.argCount>5 || memcmp(current.constants.data(),nativeModel.data(),64)) return false;
    output=current.constants;
    memcpy(output.data(),original.constants.data(),64);
    return true;
}
}
