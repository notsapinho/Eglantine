// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// The camera offsets and the hook site come from featureJosh/Recompia (GPL-3.0-or-later), which
// reverse engineered the same build of the game: games/pkp/pkp_play.cpp, "Mouse camera".
//
// The follow camera's update (main+0x87840) turns the right stick into its yaw (+0xa4, radians,
// 0..2pi, stick right lowers it) and pitch (+0xa0, clamped to [+0xd8, +0xd4]), with twin angles at
// +0x128 / +0x124 when the strafe-style option is on, and marks manual input (+0xff = 1, +0x13c = 0).
// main+0x87d30 is right after it read the stick, inside the branch that only runs while the player
// may turn the camera, with the camera in x19. The hook adds the mouse there the same way.

#include "core/pokopia/mouse_camera.h"
#include "core/pokopia/hooks.h"

#include <algorithm>
#include <bit>
#include <chrono>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "hid_core/hid_core.h"

namespace Core::Pokopia {

namespace {

constexpr std::size_t TurnSite = 0x87d30;

constexpr u64 CamPlayer = 0x8;
constexpr u64 CamPitch = 0xa0;
constexpr u64 CamYaw = 0xa4;
constexpr u64 CamPitchMax = 0xd4;
constexpr u64 CamPitchMin = 0xd8;
constexpr u64 CamManual = 0xff;
constexpr u64 CamTwinPitch = 0x124;
constexpr u64 CamTwinYaw = 0x128;
constexpr u64 CamIdle = 0x13c;
// The save data global; the strafe-style option is read from it (Recompia's twin_angles).
constexpr u64 SaveGlobal = 0x3561930;

constexpr float TwoPi = 6.2831853f;
constexpr float HalfPi = 1.5707964f;
// Recompia's default mouse speed, radians per count; mouse panning's sensitivity (50 = 1x)
// scales it.
constexpr double RadiansPerPixel = 0.000767945;
// Movement older than this when the camera can turn again was made in a menu or a cutscene and is
// dropped, so the view does not jump when control comes back.
constexpr s64 GraceNs = 250'000'000;

u64 g_main_base{};
s64 g_last_turn_ns{};

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

float ReadF32(Core::Memory::Memory& memory, u64 address) {
    return std::bit_cast<float>(memory.Read32(address));
}

void WriteF32(Core::Memory::Memory& memory, u64 address, float value) {
    memory.Write32(address, std::bit_cast<u32>(value));
}

float WrapAngle(float angle) {
    if (angle > TwoPi) {
        angle -= TwoPi;
    }
    if (angle < 0.f) {
        angle += TwoPi;
    }
    return angle;
}

bool TwinAngles(Core::Memory::Memory& memory, u8 player) {
    if (!memory.IsValidVirtualAddressRange(g_main_base + SaveGlobal, 8)) {
        return false;
    }
    const u64 save = memory.Read64(g_main_base + SaveGlobal);
    if (save == 0) {
        return false;
    }
    if (player == 1) {
        return memory.IsValidVirtualAddressRange(save + 24218468, 1) &&
               memory.Read8(save + 24218468) != 0;
    }
    return memory.IsValidVirtualAddressRange(save + 7142872, 1) &&
           (memory.Read8(save + 7142872) & 2) != 0;
}

void OnCameraTurn(Core::System& system, Kernel::Svc::ThreadContext& ctx) {
    auto& hid = system.HIDCore();
    const auto [dx, dy] = hid.TakeMouseLook();
    const s64 now = NowNs();
    const bool fresh = g_last_turn_ns != 0 && now - g_last_turn_ns <= GraceNs;
    g_last_turn_ns = now;
    hid.MarkCameraTurn(now);
    if (!fresh || (dx == 0 && dy == 0) || !Settings::values.mouse_panning.GetValue() ||
        hid.IsNmouseActive()) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    const u64 cam = ctx.r[19];
    if (!memory.IsValidVirtualAddressRange(cam, CamIdle + 4)) {
        return;
    }
    const double x_scale = Settings::values.mouse_panning_x_sensitivity.GetValue() / 50.0;
    const double y_scale = Settings::values.mouse_panning_y_sensitivity.GetValue() / 50.0;
    // Mouse right turns right (the yaw goes down); mouse up looks up (the pitch goes down).
    const float dyaw = static_cast<float>(dx * RadiansPerPixel * x_scale);
    const float dpitch = static_cast<float>(dy * RadiansPerPixel * y_scale);
    const bool twin = TwinAngles(memory, memory.Read8(cam + CamPlayer));
    WriteF32(memory, cam + CamYaw, WrapAngle(ReadF32(memory, cam + CamYaw) - dyaw));
    if (twin) {
        WriteF32(memory, cam + CamTwinYaw, WrapAngle(ReadF32(memory, cam + CamTwinYaw) - dyaw));
    }
    if (dpitch != 0.f) {
        const float pitch = std::clamp(ReadF32(memory, cam + CamPitch) + dpitch,
                                       ReadF32(memory, cam + CamPitchMin),
                                       ReadF32(memory, cam + CamPitchMax));
        WriteF32(memory, cam + CamPitch, pitch);
        if (twin) {
            WriteF32(memory, cam + CamTwinPitch,
                     std::clamp(ReadF32(memory, cam + CamTwinPitch) + dpitch, -HalfPi, HalfPi));
        }
    }
    memory.Write32(cam + CamIdle, 0);
    memory.Write8(cam + CamManual, 1);
}

} // namespace

bool InstallMouseCamera(Core::System& system, HookInstaller& hooks, u64 main_base) {
    g_main_base = main_base;
    g_last_turn_ns = 0;
    if (!hooks.Add(TurnSite, &OnCameraTurn, "mouse camera")) {
        return false;
    }
    system.HIDCore().SetMouseCameraHooked(true);
    return true;
}

} // namespace Core::Pokopia
