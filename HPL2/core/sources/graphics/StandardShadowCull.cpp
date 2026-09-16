#include "graphics/StandardShadowCull.h"

#include <algorithm>
#include <cmath>

namespace hpl {

namespace {

// Mirrors MathLib's MvpToPlanes epsilon so the degenerate cases (infinite far
// plane, zero-length normal) tip at the same threshold on both sides.
constexpr float kPlaneEpsilon = 1e-7f;

struct Plane {
    float x, y, z, w;
};

inline Plane addRows(const float* a, const float* b) {
    Plane p;
    p.x = a[0] + b[0];
    p.y = a[1] + b[1];
    p.z = a[2] + b[2];
    p.w = a[3] + b[3];
    return p;
}

inline Plane subRows(const float* a, const float* b) {
    Plane p;
    p.x = a[0] - b[0];
    p.y = a[1] - b[1];
    p.z = a[2] - b[2];
    p.w = a[3] - b[3];
    return p;
}

inline float normalLength(const Plane& p) {
    return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
}

inline void scalePlane(Plane& p, float s) {
    p.x *= s;
    p.y *= s;
    p.z *= s;
    p.w *= s;
}

// Side planes: MathLib normalizes with rsqrt(dot(xyz,xyz)). Near/far: it
// divides by max(length, eps) instead, which keeps a degenerate far plane
// finite rather than producing infinities.
inline void normalizeSide(Plane& p) {
    const float lengthSquared = p.x * p.x + p.y * p.y + p.z * p.z;
    if (lengthSquared > 0.0f) {
        scalePlane(p, 1.0f / std::sqrt(lengthSquared));
    }
}

inline void normalizeDepth(Plane& p) {
    const float length = normalLength(p);
    scalePlane(p, 1.0f / (length > kPlaneEpsilon ? length : kPlaneEpsilon));
}

inline void storePlane(float* out, const Plane& p) {
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
    out[3] = p.w;
}

} // namespace

bool StandardExtractFrustumPlanes(const float viewProjRowMajor[16], float outPlanes[24]) {
    const float* row0 = viewProjRowMajor + 0;
    const float* row1 = viewProjRowMajor + 4;
    const float* row2 = viewProjRowMajor + 8;
    const float* row3 = viewProjRowMajor + 12;

    // Gribb-Hartmann. The near plane is row2 alone: Vulkan/D3D clip space puts
    // z in [0, 1], so the near plane is z >= 0 rather than z >= -w. An OpenGL
    // style matrix would need row3 + row2 here, which is the STYLE_OGL branch
    // in MathLib that this renderer never takes.
    Plane left = addRows(row3, row0);
    Plane right = subRows(row3, row0);
    Plane bottom = addRows(row3, row1);
    Plane top = subRows(row3, row1);
    Plane far_ = subRows(row3, row2);
    Plane near_ = {row2[0], row2[1], row2[2], row2[3]};

    normalizeSide(left);
    normalizeSide(right);
    normalizeSide(bottom);
    normalizeSide(top);
    normalizeDepth(near_);
    normalizeDepth(far_);

    // A reversed-Z projection swaps which of the two depth planes is nearer.
    const bool reversed = std::fabs(near_.w) > std::fabs(far_.w);
    if (reversed) {
        const Plane swap = near_;
        near_ = far_;
        far_ = swap;
    }

    // An infinite far plane degenerates to a zero normal; MathLib replaces it
    // with the negated near normal so the plane still has a usable direction.
    if (normalLength(far_) < kPlaneEpsilon) {
        far_.x = -near_.x;
        far_.y = -near_.y;
        far_.z = -near_.z;
    }

    storePlane(outPlanes + 0, left);
    storePlane(outPlanes + 4, right);
    storePlane(outPlanes + 8, bottom);
    storePlane(outPlanes + 12, top);
    storePlane(outPlanes + 16, near_);
    storePlane(outPlanes + 20, far_);
    return reversed;
}

void StandardDisableFarPlane(float planes[24]) {
    // Forward direction of the frustum: every side plane normal has a positive
    // component along it, so the four of them sum to it (up to scale). This is
    // independent of the reversed-Z swap, which is the point -- the swap moves
    // the far plane into the near slot for a genuinely reversed projection and
    // into neither for the heuristic's misfire, so the flag cannot pick a slot.
    float forwardX = 0.0f, forwardY = 0.0f, forwardZ = 0.0f;
    for (int side = 0; side < 4; ++side) {
        forwardX += planes[side * 4 + 0];
        forwardY += planes[side * 4 + 1];
        forwardZ += planes[side * 4 + 2];
    }

    // The far plane faces against forward. A degenerate side set leaves the dot
    // at zero, and index 5 is the right answer for every unswapped set.
    const float* near_ = planes + 16;
    const float nearFacing =
        near_[0] * forwardX + near_[1] * forwardY + near_[2] * forwardZ;
    float* far_ = planes + (nearFacing < 0.0f ? 16 : 20);
    far_[0] = 0.0f;
    far_[1] = 0.0f;
    far_[2] = 0.0f;
    far_[3] = 0.0f;
}

bool StandardSphereOverlapsAabb(float centerX, float centerY, float centerZ, float radius,
                                float minX, float minY, float minZ,
                                float maxX, float maxY, float maxZ) {
    // Squared distance from the sphere centre to the closest point on the box.
    float distanceSquared = 0.0f;
    const float centers[3] = {centerX, centerY, centerZ};
    const float mins[3] = {minX, minY, minZ};
    const float maxs[3] = {maxX, maxY, maxZ};
    for (int axis = 0; axis < 3; ++axis) {
        const float c = centers[axis];
        if (c < mins[axis]) {
            const float delta = mins[axis] - c;
            distanceSquared += delta * delta;
        } else if (c > maxs[axis]) {
            const float delta = c - maxs[axis];
            distanceSquared += delta * delta;
        }
    }
    return distanceSquared <= radius * radius;
}

size_t StandardHiZMipOffset(const StandardHiZPyramid& pyramid, uint32_t mip) {
    size_t offset = 0;
    for (uint32_t level = 0; level < mip; ++level) {
        const uint32_t w = pyramid.width >> level;
        const uint32_t h = pyramid.height >> level;
        offset += static_cast<size_t>(w > 0u ? w : 1u) * (h > 0u ? h : 1u);
    }
    return offset;
}

float StandardHiZSampleFarthest(const StandardHiZPyramid& pyramid,
                                const StandardCullScreenRect& rect) {
    if (pyramid.texels == nullptr || pyramid.mipCount == 0u ||
        pyramid.width == 0u || pyramid.height == 0u || rect.testable == 0u) {
        return 1.0f;
    }
    const uint32_t mip = rect.mip < pyramid.mipCount ? rect.mip : pyramid.mipCount - 1u;
    const uint32_t mipWidth = std::max<uint32_t>(1u, pyramid.width >> mip);
    const uint32_t mipHeight = std::max<uint32_t>(1u, pyramid.height >> mip);
    const float* level = pyramid.texels + StandardHiZMipOffset(pyramid, mip);

    // Corner texels of the rect. Anything between them is covered because the
    // chosen mip's texels are at least as large as the rect itself.
    const float us[2] = {rect.minU, rect.maxU};
    const float vs[2] = {rect.minV, rect.maxV};
    float farthest = 0.0f;
    for (int iv = 0; iv < 2; ++iv) {
        for (int iu = 0; iu < 2; ++iu) {
            const float u = us[iu] < 0.0f ? 0.0f : (us[iu] > 1.0f ? 1.0f : us[iu]);
            const float v = vs[iv] < 0.0f ? 0.0f : (vs[iv] > 1.0f ? 1.0f : vs[iv]);
            uint32_t x = static_cast<uint32_t>(u * static_cast<float>(mipWidth));
            uint32_t y = static_cast<uint32_t>(v * static_cast<float>(mipHeight));
            if (x >= mipWidth)
                x = mipWidth - 1u;
            if (y >= mipHeight)
                y = mipHeight - 1u;
            const float texel = level[static_cast<size_t>(y) * mipWidth + x];
            farthest = texel > farthest ? texel : farthest;
        }
    }
    return farthest;
}

bool StandardCullOcclusionReference(const StandardCullCamera& camera,
                                    const StandardHiZPyramid& pyramid,
                                    float minX, float minY, float minZ,
                                    float maxX, float maxY, float maxZ) {
    const StandardCullScreenRect rect =
        standardCullProjectAabb(camera, minX, minY, minZ, maxX, maxY, maxZ);
    return standardCullOccluded(rect, StandardHiZSampleFarthest(pyramid, rect));
}

uint32_t StandardShadowCullReference(const StandardCullCandidate* candidates,
                                     size_t candidateArrayLength,
                                     const StandardCullTile& tile,
                                     uint32_t mode,
                                     StandardDrawIndirect* outIndirect) {
    if (candidates == nullptr || outIndirect == nullptr) {
        return 0u;
    }

    uint32_t written = 0u;
    for (uint32_t local = 0u; local < tile.candidateCount; ++local) {
        const size_t index = static_cast<size_t>(tile.candidateBase) + local;
        if (index >= candidateArrayLength) {
            break;
        }
        const StandardCullCandidate& candidate = candidates[index];
        const bool keep = standardCullKeepCandidate(tile, candidate);

        if (mode == kStandardCullModeInPlace) {
            // Every candidate owns a fixed slot; culled ones become a no-op
            // draw. Keeps the command count CPU-known when the device has no
            // vkCmdDrawIndirectCount.
            StandardDrawIndirect& command = outIndirect[local];
            command.vertexCount = candidate.vertexCount;
            command.instanceCount = keep ? 1u : 0u;
            command.firstVertex = 0u;
            command.firstInstance = candidate.objectSlot;
            written = local + 1u;
            continue;
        }

        if (!keep) {
            continue;
        }
        // Compact: survivors pack to the front. On the GPU this slot comes from
        // an InterlockedAdd, so the order differs run to run -- which is safe
        // here because the shadow pass is depth-only with no blending.
        StandardDrawIndirect& command = outIndirect[written];
        command.vertexCount = candidate.vertexCount;
        command.instanceCount = 1u;
        command.firstVertex = 0u;
        command.firstInstance = candidate.objectSlot;
        ++written;
    }
    return written;
}

uint32_t StandardCullInstanceMaskReference(const StandardCullCandidate* candidates,
                                           size_t candidateArrayLength,
                                           const StandardCullTile& tile,
                                           const StandardCullCamera* camera,
                                           const StandardHiZPyramid* pyramid,
                                           uint32_t* commandWords,
                                           size_t commandWordCount) {
    if (candidates == nullptr || commandWords == nullptr) {
        return 0u;
    }

    uint32_t kept = 0u;
    for (uint32_t local = 0u; local < tile.candidateCount; ++local) {
        const size_t index = static_cast<size_t>(tile.candidateBase) + local;
        if (index >= candidateArrayLength) {
            break;
        }
        const StandardCullCandidate& candidate = candidates[index];

        bool keep = standardCullKeepCandidate(tile, candidate);
        // Same three gates the kernel's cullOccludes applies before it touches
        // the pyramid: a tile with no camera, a candidate marked never-occlude,
        // and an absent pyramid all skip the depth test entirely.
        if (keep && camera != nullptr && pyramid != nullptr &&
            tile.cameraIndex != kStandardCullNoCamera &&
            (candidate.cullFlags & kStandardCullFlagNeverOcclude) == 0u &&
            pyramid->mipCount != 0u) {
            if (StandardCullOcclusionReference(*camera, *pyramid,
                                               candidate.aabbMinX, candidate.aabbMinY,
                                               candidate.aabbMinZ, candidate.aabbMaxX,
                                               candidate.aabbMaxY, candidate.aabbMaxZ)) {
                keep = false;
            }
        }

        // The kernel writes exactly this one word and nothing else.
        if (candidate.commandWordOffset < commandWordCount) {
            commandWords[candidate.commandWordOffset] = keep ? 1u : 0u;
        }
        if (keep) {
            ++kept;
        }
    }
    return kept;
}

} // namespace hpl
