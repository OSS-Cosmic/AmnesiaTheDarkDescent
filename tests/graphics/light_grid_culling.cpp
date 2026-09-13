#include "../../amnesia/slang/LightGridCulling.h"
#include "utest.h"

#include <cmath>
#include <initializer_list>

namespace {
using hpl::float3;
using hpl::float4;

float3 Add(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
float3 Scale(float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Projection {
  float3 apex, right, up, forward;
  float halfWidth, halfHeight;
  float4 rowX, rowY, rowW;

  // Construct points from independent geometric FOV/aspect bounds, rather
  // than using the culling planes as the oracle for acceptance.
  float3 Point(float u, float v, float depth) const {
    return Add(apex, Add(Scale(forward, depth),
        Add(Scale(right, u * halfWidth * depth), Scale(up, v * halfHeight * depth))));
  }
  bool Intersects(float3 center, float halfSize) const {
    return hpl::lightGridProjectionIntersectsBox(rowX, rowY, rowW, center, halfSize);
  }
};

Projection MakeProjection(float fov, float aspect, float yaw, float pitch,
                          float3 apex) {
  Projection p{};
  p.apex = apex;
  p.right = {std::cos(yaw), 0, -std::sin(yaw)};
  p.up = {std::sin(yaw) * std::sin(pitch), std::cos(pitch),
          std::cos(yaw) * std::sin(pitch)};
  p.forward = {std::sin(yaw) * std::cos(pitch), -std::sin(pitch),
               std::cos(yaw) * std::cos(pitch)};
  p.halfHeight = std::tan(fov * 0.5f);
  p.halfWidth = aspect * p.halfHeight;
  auto row = [apex](float3 n) { return float4(n.x, n.y, n.z, -Dot(n, apex)); };
  p.rowX = row(Add(Scale(p.right, 0.5f / p.halfWidth), Scale(p.forward, 0.5f)));
  p.rowY = row(Add(Scale(p.up, -0.5f / p.halfHeight), Scale(p.forward, 0.5f)));
  p.rowW = row(p.forward);
  return p;
}

void CheckOldArchives(int *utest_result) {
  constexpr float unit = 62.5f / 32.0f;
  constexpr float fov = 0.872665f;
  const auto p = MakeProjection(fov, 2.0f, 0, 0, float3(0));
  const float3 point(8, 0, 10);
  const float oldConeDistance = std::cos(fov * 0.5f) * 8 - std::sin(fov * 0.5f) * 10;
  ASSERT_TRUE_MSG(point.x < 10 * p.halfWidth &&
                      std::sqrt(Dot(point, point)) < 17.068f,
                  "Old Archives witness lies inside gobo and reach");
  ASSERT_TRUE_MSG(oldConeDistance > unit * std::sqrt(3.0f) * 0.5f,
                  "old circular cull incorrectly rejects witness cell");
  ASSERT_TRUE_MSG(p.Intersects(point, unit * 0.5f),
                  "rectangular cull retains witness cell");
}

void CheckBoundaries(int *utest_result) {
  for (float scale : {0.0001f, 1.0f, 10000.0f}) {
    const float4 plane(scale, 0, 0, -1001 * scale);
    ASSERT_TRUE_MSG(hpl::lightGridPlaneIntersectsBox(
                        plane, float3(1000, 0, 0), 1),
                    "translated touching box retained at every plane scale");
    ASSERT_FALSE_MSG(hpl::lightGridPlaneIntersectsBox(
                         plane, float3(998, 0, 0), 1),
                     "fully outside box rejected at every plane scale");
  }
  const auto p = MakeProjection(0.872665f, 2, 0, 0, float3(0));
  ASSERT_FALSE_MSG(p.Intersects(float3(0, 0, -4), 0.5f), "box behind apex rejected");
  ASSERT_TRUE_MSG(p.Intersects(float3(0), 0.1f), "box crossing apex retained");
  ASSERT_FALSE_MSG(p.Intersects(p.Point(4, 0, 10), 0.1f), "box outside horizontal side rejected");
  ASSERT_FALSE_MSG(p.Intersects(p.Point(0, -4, 10), 0.1f), "box outside vertical side rejected");
  ASSERT_TRUE_MSG(p.Intersects(p.Point(0, 0, 0.001f), 0), "no unintended near clip");
  ASSERT_TRUE_MSG(p.Intersects(p.Point(0, 0, 500), 0), "no unintended far clip");
}

void CheckCameraSweeps(int *utest_result) {
  constexpr float unit = 62.5f / 32.0f;
  const float edges[] = {-1, -0.999f, -0.5f, 0, 0.5f, 0.999f, 1};
  unsigned checked = 0;
  for (float aspect : {0.5f, 1.0f, 2.0f, 4.0f})
  for (float fov : {0.872665f, 1.65806f})
  for (float yaw : {0.0f, 0.7f, 1.13446f})
  for (float3 apex : {float3(8.5f, 1.25f, 10.5f), float3(1000, -500, 900)}) {
    const auto p = MakeProjection(fov, aspect, yaw, yaw * 0.6f, apex);
    for (float depth : {0.05f, 1.0f, 5.0f, 10.0f, 16.0f})
    for (float u : edges)
    for (float v : edges) {
      const float3 point = p.Point(u, v, depth);
      const float3 relative = Add(point, Scale(apex, -1));
      if (Dot(relative, relative) >= 17.068f * 17.068f) continue;
      // Move the camera-centered grid through one whole cell on each axis.
      // Stagger Y and Z so edges/corners see different rounding combinations.
      for (int step = 0; step <= 32; ++step) {
        const float3 camera = Add(apex, float3(unit * step / 32,
            unit * ((step * 7) % 33) / 32, unit * ((step * 13) % 33) / 32));
        const float3 cell(std::nearbyint((point.x - camera.x) / unit),
                          std::nearbyint((point.y - camera.y) / unit),
                          std::nearbyint((point.z - camera.z) / unit));
        const float3 center = Add(camera, Scale(cell, unit));
        ASSERT_TRUE_MSG(std::fabs(cell.x) < 16 && std::fabs(cell.y) < 16 &&
                            std::fabs(cell.z) < 16,
                        "sweep stays inside grid coverage");
        ASSERT_TRUE_MSG(p.Intersects(center, unit * 0.5f),
                        "illuminated point's cell retained across camera movement");
        ++checked;
      }
    }
  }
  ASSERT_GT_MSG(checked, 100000u,
                "camera sweep covers projection interiors, edges and corners");
}
} // namespace

UTEST(LightGridCulling, OldArchives) { CheckOldArchives(utest_result); }
UTEST(LightGridCulling, Boundaries) { CheckBoundaries(utest_result); }
UTEST(LightGridCulling, CameraSweeps) { CheckCameraSweeps(utest_result); }
