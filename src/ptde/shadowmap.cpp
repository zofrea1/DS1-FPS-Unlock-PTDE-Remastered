#include "shadowmap.h"

#include "log.h"

#include <cstdint>

// The side of the shadow map is one number in the game's data (0x12DFF34, 2048). The game reads it when it makes the
// shadow map (0x100A32B: a packed-depth colour target and its depth), for the viewport of each slice (0xEB4856: with
// four slices each has a quarter, half the side) and for the half-texel step of the matrix that looks the shadow up
// (0xF477DB). Everything else works in the map's own 0..1 coordinates, so a larger number gives the same shadows
// from more texels.
//
// A depth bias (ShadowBias, 2.3.0 to 2.5.0) is gone. It took a constant off the light's clip depth for the shadow
// lookup the game uses when a character or object lies inside one slice of the map (vertex constants c137..c140).
// The slices are perspective ones (map position and depth divided by the distance ahead of the camera): in Quelaag's
// Domain the depth row was 0.90 w - 0.138 y, so 0.006 off the depth was 0.0435 * w of height, about 22 cm at 5 m,
// and real shadows went: an egg carrier's top no longer shadowed its lower sacs. The game's other lookup (the slice
// picked per pixel, for whatever crosses a slice's edge; pixel constants c140..c160) had no bias, so a character lit
// up or darkened as it crossed those edges (2026-11-18). A larger map is what keeps the texel weave fine.
namespace {
constexpr uintptr_t kShadowMapSize = 0x012DFF34;
}  // namespace

bool shadowmap_set_resolution(int size) {
    if (size != 4096 && size != 8192) return size == 2048;
    auto* p = reinterpret_cast<int32_t*>(kShadowMapSize);
    if (*p != 2048) {
        LOG_ERROR("ShadowMapResolution: the shadow map size at %08X is %d, not 2048 (another build or mod?). Not "
                  "changing it.", static_cast<unsigned>(kShadowMapSize), *p);
        return false;
    }
    *p = size;
    LOG_INFO("ShadowMapResolution: the shadow map is %dx%d (the game's own: 2048x2048)", size, size);
    return true;
}
