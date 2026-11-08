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
//
// The hook: the instruction at the site becomes a branch to a stub in the text segment's padding,
// which runs svc 0x7d (the kernel calls OnCameraTurn, svc.cpp), then the replaced instruction, then
// branches back.

#include "core/pokopia/mouse_camera.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#include "common/alignment.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "hid_core/hid_core.h"

namespace Core::Pokopia {

namespace {

constexpr u64 ProgramId = 0x0400A3A01EE4C000ULL;
// Main module build ID of version 2.0.1 (the same build as Recompia's and the FPS mods').
constexpr std::array<u8, 20> BuildId{0x76, 0xA4, 0x89, 0xF3, 0xBD, 0x36, 0x42, 0xFA, 0x5E, 0x18,
                                     0xBD, 0x83, 0xD3, 0x2C, 0x98, 0xBA, 0xCC, 0x5F, 0x7D, 0x0E};

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

constexpr u32 SvcHook = 0xD4000001 | (0x7d << 5);

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

s64 SignExtend(u64 value, int bits) {
    const u64 sign = u64{1} << (bits - 1);
    return static_cast<s64>((value ^ sign) - sign);
}

std::optional<u32> Branch(u64 from, u64 to, bool link = false) {
    const s64 offset = static_cast<s64>(to - from);
    if ((offset & 3) != 0 || offset < -(s64{1} << 27) || offset >= (s64{1} << 27)) {
        return std::nullopt;
    }
    return (link ? 0x94000000U : 0x14000000U) | static_cast<u32>((offset >> 2) & 0x3FFFFFF);
}

// The replaced instruction, rewritten to run at `to` instead of `from`: as it is unless it reads
// the PC. Literal loads are not handled.
std::optional<std::vector<u32>> Relocate(u32 insn, u64 from, u64 to) {
    if ((insn & 0x7C000000) == 0x14000000) { // b, bl
        const u64 target = from + SignExtend(insn & 0x3FFFFFF, 26) * 4;
        const auto branch = Branch(to, target, (insn & 0x80000000) != 0);
        if (!branch) {
            return std::nullopt;
        }
        return std::vector<u32>{*branch};
    }
    const bool cond_branch = (insn & 0xFF000010) == 0x54000000;  // b.cond
    const bool compare_branch = (insn & 0x7E000000) == 0x34000000; // cbz, cbnz
    const bool test_branch = (insn & 0x7E000000) == 0x36000000;    // tbz, tbnz
    if (cond_branch || compare_branch || test_branch) {
        // The same test, jumping 8 bytes ahead to a far branch to the old target; otherwise
        // falling through to a branch over it.
        u64 target;
        u32 near;
        if (test_branch) {
            target = from + SignExtend((insn >> 5) & 0x3FFF, 14) * 4;
            near = (insn & ~(0x3FFFU << 5)) | (2U << 5);
        } else {
            target = from + SignExtend((insn >> 5) & 0x7FFFF, 19) * 4;
            near = (insn & ~(0x7FFFFU << 5)) | (2U << 5);
        }
        const auto far = Branch(to + 8, target);
        if (!far) {
            return std::nullopt;
        }
        return std::vector<u32>{near, 0x14000002U, *far}; // b.cond +8; b +8; b target
    }
    if ((insn & 0x1F000000) == 0x10000000) { // adr, adrp
        const u32 rd = insn & 31;
        const u64 imm = ((insn >> 5) & 0x7FFFF) << 2 | ((insn >> 29) & 3);
        const u64 value = (insn & 0x80000000) != 0
                              ? (from & ~u64{0xFFF}) + (SignExtend(imm, 21) << 12)
                              : from + SignExtend(imm, 21);
        std::vector<u32> out;
        for (u32 hw = 0; hw < 4; ++hw) {
            const u32 part = static_cast<u32>((value >> (hw * 16)) & 0xFFFF);
            out.push_back((hw == 0 ? 0xD2800000U : 0xF2800000U) | (hw << 21) | (part << 5) | rd);
        }
        return out;
    }
    if ((insn & 0x3B000000) == 0x18000000) { // ldr (literal), prfm (literal)
        return std::nullopt;
    }
    return std::vector<u32>{insn};
}

u32 ReadWord(std::span<const u8> image, std::size_t offset) {
    u32 word;
    std::memcpy(&word, image.data() + offset, sizeof(word));
    return word;
}

void WriteWord(std::span<u8> image, std::size_t offset, u32 word) {
    std::memcpy(image.data() + offset, &word, sizeof(word));
}

} // namespace

bool InstallMouseCamera(Core::System& system, std::span<u8> image, u64 load_base, u64 program_id,
                        std::span<const u8> build_id, std::size_t text_offset,
                        std::size_t text_size, std::size_t text_mapped) {
    if (program_id != ProgramId || build_id.size() < BuildId.size() ||
        !std::equal(BuildId.begin(), BuildId.end(), build_id.begin())) {
        return false;
    }
    if (const char* off = std::getenv("EDEN_POKOPIA_MOUSE_CAMERA"); off && off[0] == '0') {
        LOG_INFO(Loader, "Pokopia mouse camera: off (EDEN_POKOPIA_MOUSE_CAMERA=0)");
        return false;
    }
    if (Settings::IsNceEnabled()) {
        // NCE runs the game's code natively, where the stub's svc would not reach this hook.
        LOG_INFO(Loader, "Pokopia mouse camera: not available with the NCE CPU backend");
        return false;
    }
    const std::size_t site = text_offset + TurnSite;
    if (site + 4 > text_offset + text_size) {
        return false;
    }
    const u64 site_address = load_base + TurnSite;
    const std::size_t stub = Common::AlignUp(text_offset + text_size, 4);
    const u64 stub_address = load_base + (stub - text_offset);

    const u32 original = ReadWord(image, site);
    const auto moved = Relocate(original, site_address, stub_address + 4);
    if (!moved) {
        LOG_ERROR(Loader, "Pokopia mouse camera: cannot move instruction {:08x} at main+{:#x}",
                  original, TurnSite);
        return false;
    }
    const std::size_t stub_words = 1 + moved->size() + 1;
    const std::size_t stub_end = stub + stub_words * 4;
    if (stub_end > text_offset + text_mapped ||
        !std::all_of(image.begin() + stub, image.begin() + stub_end,
                     [](u8 byte) { return byte == 0; })) {
        LOG_ERROR(Loader, "Pokopia mouse camera: no free space after the text segment");
        return false;
    }
    const auto back = Branch(stub_address + (stub_words - 1) * 4, site_address + 4);
    const auto to_stub = Branch(site_address, stub_address);
    if (!back || !to_stub) {
        return false;
    }

    std::size_t offset = stub;
    WriteWord(image, offset, SvcHook);
    offset += 4;
    for (const u32 word : *moved) {
        WriteWord(image, offset, word);
        offset += 4;
    }
    WriteWord(image, offset, *back);
    WriteWord(image, site, *to_stub);

    g_main_base = load_base;
    g_last_turn_ns = 0;
    Kernel::Svc::g_guest_hooks[stub_address] = &OnCameraTurn;
    system.HIDCore().SetMouseCameraHooked(true);
    LOG_INFO(Loader,
             "Pokopia mouse camera: hooked main+{:#x} ({:08x}) through a stub at main+{:#x}",
             TurnSite, original, stub - text_offset);
    return true;
}

} // namespace Core::Pokopia
