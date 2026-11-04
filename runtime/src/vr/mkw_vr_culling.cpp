// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native replacements for the two Mario Kart functions that hide objects the
// game camera cannot see, with the VR switch that turns that hiding off. See
// vr/mkw_vr_culling.h for the design. Both are faithful reimplementations of
// the PAL RMCP01 code: with culling on (the default) they compute exactly what
// the translated originals did.

#include "vr/mkw_vr_culling.h"

#include "guest_function_imports.h"
#include "hle_stubs.h"
#include "isa/ppc_isa_context.h"
#include "memory.h"
#include "memory_access.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <atomic>
#include <cmath>
#include <cstdint>

#if defined(__clang__)
// The reimplementations below must round like the PowerPC originals (discrete
// single-precision operations, explicit fused ones only where the game fuses)
// and keep NaN compares unordered. cmake/PublicProducts.cmake builds this file
// on its own with the translated code's -fno-fast-math -ffp-contract=off; the
// pragmas say the same in the source.
#pragma float_control(push)
#pragma float_control(precise, on)
#endif

namespace mkw::vr {

namespace {

std::atomic<bool> g_object_culling{true};
std::atomic<bool> g_vr_enabled{false};
std::atomic<bool> g_logged{false};

} // namespace

void MkwVRSetObjectCulling(bool enabled) noexcept {
    const bool previous = g_object_culling.exchange(enabled, std::memory_order_relaxed);
    if (previous != enabled || !g_logged.exchange(true, std::memory_order_relaxed)) {
        RT_LOG(RT_TAG_RUNTIME) << "[vr] object culling "
                               << (enabled ? "on (the game's own)" : "off (objects outside the game camera are drawn)")
                               << std::endl;
    }
}

bool MkwVRObjectCullingEnabled() noexcept {
    return g_object_culling.load(std::memory_order_relaxed);
}

void MkwVRObjectCullingApplyConfiguredSettings(bool vr_enabled) noexcept {
    g_vr_enabled.store(vr_enabled, std::memory_order_relaxed);
    // The setting lives under [vr]: a desktop session keeps the game's culling,
    // and so does the Flat screen race view, which shows the game camera's own
    // view, where everything culled is off screen anyway.
    MkwVRSetObjectCulling(!vr_enabled || RuntimeConfigFile::VrObjectCulling() ||
                          RuntimeConfigFile::VrFlatScreen());
}

void MkwVRObjectCullingApplyConfiguredSettings() noexcept {
    MkwVRObjectCullingApplyConfiguredSettings(g_vr_enabled.load(std::memory_order_relaxed));
}

} // namespace mkw::vr

namespace {

using mkw::vr::CullingAabb;
using mkw::vr::CullingFrustum;
using mkw::vr::FrustumAabbResult;

void ReadVec3(uint32_t address, float out[3]) noexcept {
    for (uint32_t i = 0; i < 3; ++i) {
        out[i] = MemoryInline::FlatReadFloat32(address + i * 4u);
    }
}

// The translated code loads these singles straight from guest memory, so the
// unchecked flat reads it uses are the right tool here too: this runs once per
// scene object per frame.
void ReadFrustum(uint32_t frustum, CullingFrustum& out) noexcept {
    ReadVec3(frustum + mkw::vr::kFrustumBoxOffset, out.box.min);
    ReadVec3(frustum + mkw::vr::kFrustumBoxOffset + 12u, out.box.max);
    for (uint32_t p = 0; p < mkw::vr::kFrustumPlaneCount; ++p) {
        const uint32_t plane = frustum + mkw::vr::kFrustumPlanesOffset + p * mkw::vr::kFrustumPlaneStride;
        ReadVec3(plane, out.planes[p].normal);
        out.planes[p].distance = MemoryInline::FlatReadFloat32(plane + 12u);
    }
}

// nw4r::math::FRUSTUM::IntersectAABB_Ex (0x80086610). Its one direct caller is
// nw4r::g3d::ScnObjGather::Add, with the object's own bounding box.
int32_t Nw4rFrustumIntersectAabbEx(uint32_t frustum, uint32_t aabb) {
    if (!mkw::vr::MkwVRObjectCullingEnabled()) {
        // Partially inside: gathered and drawn, like a box straddling a plane.
        return static_cast<int32_t>(FrustumAabbResult::Partial);
    }
    CullingFrustum f;
    ReadFrustum(frustum, f);
    CullingAabb box;
    ReadVec3(aabb, box.min);
    ReadVec3(aabb + 12u, box.max);
    return static_cast<int32_t>(mkw::vr::FrustumIntersectAabb(f, box));
}

// Guest functions UpdateScreenInfo calls, unchanged translated code.
const uint32_t kPSMTXInverse = MKW_GADDR(80199FC8);
const uint32_t kNw4rSinCosFIdx = MKW_GADDR(800851E0);
const uint32_t kNw4rVec3TransformNormal = MKW_GADDR(80085AB0);
const uint32_t kClipInfoMgrNormalizeVector = MKW_GADDR(807872C0);
const uint32_t kClipInfoMgrWidenPlane = MKW_GADDR(807DEBCC);
const uint32_t kClipInfoMgrGetArea8And9GroupIDs = MKW_GADDR(80786FC0);
// The four float constants UpdateScreenInfo reads (its r30 table) and the
// reference vector it hands the plane-widening helper (its r31).
// guest_versions.inc knows no NTSC-U counterpart of the constants table (StaticR .data), so
// there it is 0 and the game's own UpdateScreenInfo runs instead (see the registration below).
const uint32_t kClipConstants = [] {
    const GuestRegion::Mapping mapping = GuestRegion::Map(0x808A4808u, GuestRegion::Current());
    return mapping.found ? mapping.address : 0u;
}();
const uint32_t kClipReferenceVector = MKW_GADDR(802A4130);
// The original's stack frame, laid out as it uses it.
constexpr uint32_t kFrameBytes = 0xA0u;
constexpr uint32_t kFrameCos = 0x08u;
constexpr uint32_t kFrameSin = 0x0Cu;
constexpr uint32_t kFrameVecA = 0x10u;
constexpr uint32_t kFrameVecB = 0x1Cu;
constexpr uint32_t kFrameVecC = 0x28u;
constexpr uint32_t kFrameForward = 0x34u;
constexpr uint32_t kFrameInverse = 0x40u;

float SingleMul(float a, float b) noexcept {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    return a * b;
}

void CopyVec3(uint32_t from, uint32_t to) noexcept {
    for (uint32_t i = 0; i < 3; ++i) {
        MemoryInline::FlatWriteRam32(to + i * 4u, MemoryInline::FlatRead32(from + i * 4u));
    }
}

// MTX::PSVECCrossProduct (0x8019ACCC), which the original inlines: paired
// single multiply-subtracts, so each component is one fused operation.
void CrossProduct(uint32_t a_addr, uint32_t b_addr, uint32_t out_addr) noexcept {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    float a[3];
    float b[3];
    ReadVec3(a_addr, a);
    ReadVec3(b_addr, b);
    const float x_sub = SingleMul(b[1], a[2]);
    const float x = std::fmaf(a[1], b[2], -x_sub);
    const float y_sub = SingleMul(b[0], a[2]);
    const float y = -std::fmaf(a[0], b[2], -y_sub);
    const float z_sub = SingleMul(b[1], a[0]);
    const float z = -std::fmaf(a[1], b[0], -z_sub);
    MemoryInline::FlatWriteFloat32(out_addr, x);
    MemoryInline::FlatWriteFloat32(out_addr + 4u, y);
    MemoryInline::FlatWriteFloat32(out_addr + 8u, z);
}

// ClipInfoMgr::UpdateScreenInfo (0x8078707C): fills one screen's
// ClipScreenInfo from its camera. The matrix, trigonometry, normalisation and
// plane-widening steps run the game's own translated code; the few
// single-precision operations in between mirror the original instruction by
// instruction. With culling off the six plane normals are zeroed afterwards,
// so ClipInfoMgr::Update finds nothing beyond a plane.
void ZeroScreenPlanesUnlessCulling(uint32_t screen) noexcept {
    if (!mkw::vr::MkwVRObjectCullingEnabled()) {
        for (uint32_t offset = 0; offset < mkw::vr::kClipScreenPlanesBytes; offset += 4u) {
            MemoryInline::FlatWriteRam32(screen + mkw::vr::kClipScreenPlanesOffset + offset, 0u);
        }
    }
}

void ClipInfoMgrUpdateScreenInfo(uint32_t screen, uint32_t camera) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    CpuContext* ctx = CurrentCpuContext();
    // The callees save and restore LR themselves; keeping the entry value
    // makes this native leave it exactly as the original's epilogue did.
    const uint32_t caller_lr = ctx->lr;
    const uint32_t caller_sp = ctx->gpr[1];
    const uint32_t sp = caller_sp - kFrameBytes;
    MemoryInline::FlatWriteRam32(sp, caller_sp);
    ctx->gpr[1] = sp;

    const auto call = [ctx](uint32_t target, uint32_t r3, uint32_t r4, uint32_t r5 = 0, uint32_t r6 = 0) {
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
        ctx->gpr[6] = r6;
        InvokeIndirectCpu(target, ctx);
    };
    const auto read = [](uint32_t address) { return MemoryInline::FlatReadFloat32(address); };
    const auto write = [](uint32_t address, float value) { MemoryInline::FlatWriteFloat32(address, value); };

    // The camera's view matrix, inverted into the frame.
    const uint32_t view_matrix = MemoryInline::FlatRead32(camera + 0x6Cu) + 4u;
    const uint32_t inverse = sp + kFrameInverse;
    call(kPSMTXInverse, view_matrix, inverse);
    // Camera position: the inverse's translation column.
    write(screen + 0x00u, read(inverse + 0x0Cu));
    write(screen + 0x04u, read(inverse + 0x1Cu));
    write(screen + 0x08u, read(inverse + 0x2Cu));
    write(screen + 0x0Cu, read(camera + 0x18u));

    const float aspect = static_cast<float>(static_cast<double>(read(camera + 0x08u)) /
                                            static_cast<double>(read(camera + 0x0Cu)));
    const float fov = read(camera + 0x10u);
    const float k0 = read(kClipConstants + 0x0u);
    const float k1 = read(kClipConstants + 0x4u);
    const float k2 = read(kClipConstants + 0x8u);
    const float k3 = read(kClipConstants + 0xCu);

    // nw4r::math::SinCosFIdx(&sin, &cos, k1 * fov)
    ctx->fpr[1].d = static_cast<double>(SingleMul(k1, fov));
    call(kNw4rSinCosFIdx, sp + kFrameSin, sp + kFrameCos);

    // Draw-distance scale: (k3 * min(fov, k2))^2. The original keeps fov only
    // on an ordered fov <= k2 (fcmpo, ble), so a NaN fov takes k2.
    const float clamped_fov = mkw::vr::CullingOrderedLessOrEqual(fov, k2) ? fov : k2;
    const float scaled = SingleMul(k3, clamped_fov);
    write(screen + mkw::vr::kClipScreenDrawScaleOffset, SingleMul(scaled, scaled));

    const float sin = read(sp + kFrameSin);
    const float cos = read(sp + kFrameCos);
    const float sin_aspect = SingleMul(sin, aspect);

    // Left plane: (-cos, k0, sin * aspect) turned into world space, normalised.
    const uint32_t plane_left = screen + 0x10u;
    write(plane_left + 0u, -cos);
    write(plane_left + 4u, k0);
    write(plane_left + 8u, sin_aspect);
    call(kNw4rVec3TransformNormal, plane_left, inverse, plane_left);
    CopyVec3(plane_left, sp + kFrameVecC);
    call(kClipInfoMgrNormalizeVector, plane_left, sp + kFrameVecC);

    // Right plane: (cos, k0, sin * aspect).
    const uint32_t plane_right = screen + 0x28u;
    write(plane_right + 0u, cos);
    write(plane_right + 4u, k0);
    write(plane_right + 8u, SingleMul(sin, aspect));
    call(kNw4rVec3TransformNormal, plane_right, inverse, plane_right);
    CopyVec3(plane_right, sp + kFrameVecB);
    call(kClipInfoMgrNormalizeVector, plane_right, sp + kFrameVecB);

    // The view direction from the two side normals, then each side plane
    // widened around it into the second pair (+0x1C and +0x34).
    const uint32_t forward = sp + kFrameForward;
    CrossProduct(plane_left, plane_right, forward);
    CopyVec3(forward, sp + kFrameVecA);
    call(kClipInfoMgrNormalizeVector, forward, sp + kFrameVecA);
    call(kClipInfoMgrWidenPlane, forward, kClipReferenceVector, plane_left, screen + 0x1Cu);
    call(kClipInfoMgrWidenPlane, forward, kClipReferenceVector, plane_right, screen + 0x34u);

    // Top plane (k0, cos, sin) and bottom plane (k0, -cos, sin), turned into
    // world space.
    const uint32_t plane_top = screen + 0x40u;
    write(plane_top + 0u, k0);
    write(plane_top + 4u, cos);
    write(plane_top + 8u, sin);
    call(kNw4rVec3TransformNormal, plane_top, inverse, plane_top);
    const uint32_t plane_bottom = screen + 0x4Cu;
    write(plane_bottom + 0u, k0);
    write(plane_bottom + 4u, -cos);
    write(plane_bottom + 8u, sin);
    call(kNw4rVec3TransformNormal, plane_bottom, inverse, plane_bottom);

    // Area type 8 and 9 group bits for this screen's camera.
    call(kClipInfoMgrGetArea8And9GroupIDs, screen, 9u);
    MemoryInline::FlatWriteRam16(screen + mkw::vr::kClipScreenAreaGroupsOffset,
                                 static_cast<uint16_t>(ctx->gpr[3] & 0xFFFFu));

    ctx->gpr[1] = caller_sp;
    ctx->lr = caller_lr;

    ZeroScreenPlanesUnlessCulling(screen);
}

} // namespace

PPC_NATIVE_OVERRIDE(80086610, Nw4rFrustumIntersectAabbEx, int32_t, (uint32_t frustum, uint32_t aabb), (frustum, aabb));

// A raw registration keeps the base translation, which runs where the running region has no
// known address for the constants table (kClipConstants), followed by the same plane override.
extern "C" void ClipInfoMgr_UpdateScreenInfo_8078707c(CpuContext* ctx) {
    const uint32_t screen = ctx->gpr[3];
    if (kClipConstants == 0) {
        MKW_GUEST_FUNC(8078707C)(ctx);
        ZeroScreenPlanesUnlessCulling(screen);
        return;
    }
    ClipInfoMgrUpdateScreenInfo(screen, ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8078707C, ClipInfoMgr_UpdateScreenInfo_8078707c, "ClipInfoMgr_UpdateScreenInfo_8078707c");

#if defined(__clang__)
#pragma float_control(pop)
#endif
