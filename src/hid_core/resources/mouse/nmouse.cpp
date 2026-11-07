// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "hid_core/frontend/emulated_devices.h"
#include "hid_core/hid_core.h"
#include "hid_core/resources/mouse/nmouse.h"

namespace Service::HID {

Nmouse::Nmouse(Core::HID::HIDCore& hid_core_) : ControllerBase{hid_core_} {
    emulated_devices = hid_core.GetEmulatedDevices();
}

Nmouse::~Nmouse() {
    hid_core.SetNmouseInUse(false);
}

void Nmouse::OnInit() {}
void Nmouse::OnRelease() {}

void Nmouse::Initialize(Common::ProcessAddress t_mem, u64 size, Kernel::KProcess* process,
                        s32 width, s32 height) {
    std::scoped_lock lock{mutex};
    if (size < MinimumSize) {
        LOG_ERROR(Service_HID, "Nmouse transfer memory too small: {:#x}", size);
        return;
    }
    transfer_memory = t_mem;
    owner = process;
    range_width = width > 0 ? width : 1920;
    range_height = height > 0 ? height : 1080;
    has_last_position = false;
    has_moved = false;
    lifos = {};
    WriteLayout();
    hid_core.SetNmouseInUse(true);
}

void Nmouse::Finalize() {
    std::scoped_lock lock{mutex};
    transfer_memory = {};
    owner = nullptr;
    wheel_pending = 0;
    wheel_tap_pressed = false;
    hid_core.SetNmouseInUse(false);
}

void Nmouse::InitializeJcms(Common::ProcessAddress t_mem, u64 size, Kernel::KProcess* process) {
    std::scoped_lock lock{mutex};
    if (size < JcmsMinimumSize) {
        LOG_ERROR(Service_HID, "Jcms transfer memory too small: {:#x}", size);
        return;
    }
    jcms_memory = t_mem;
    jcms_owner = process;
    jcms_lifos = {};
    // Every ring starts empty with its buffer count set, as the server leaves it.
    auto& memory = jcms_owner->GetMemory();
    const JcmsLifo empty{};
    for (std::size_t i = 0; i < JcmsRings; ++i) {
        memory.WriteBlock(jcms_memory + i * sizeof(JcmsLifo), &empty, sizeof(empty));
    }
}

void Nmouse::FinalizeJcms() {
    std::scoped_lock lock{mutex};
    jcms_memory = {};
    jcms_owner = nullptr;
}

void Nmouse::SetPointerRange(s32 width, s32 height) {
    std::scoped_lock lock{mutex};
    if (width <= 0 || height <= 0) {
        return;
    }
    range_width = width;
    range_height = height;
    // The next position is in the new range: not a move.
    has_last_position = false;
}

std::pair<s32, s32> Nmouse::GetPointerRange() {
    std::scoped_lock lock{mutex};
    return {range_width, range_height};
}

void Nmouse::WriteLayout() {
    auto& memory = owner->GetMemory();
    for (std::size_t i = 0; i < Slots.size(); ++i) {
        const HandleEntry entry{.handle = 0x4e4d0000 + i, .is_valid = 1};
        memory.WriteBlock(transfer_memory + i * sizeof(HandleEntry), &entry, sizeof(entry));

        NmouseInfo info{};
        info.type_set = Slots[i].type_set;
        info.is_type_set_valid = 1;
        info.npad_id = 0; // Player 1
        info.is_npad_id_valid = Slots[i].has_npad ? 1 : 0;
        info.is_pointer_enabled = 1;
        memory.WriteBlock(transfer_memory + InfoOffset + i * InfoStride, &info, sizeof(info));

        memory.WriteBlock(transfer_memory + LifoOffset + i * LifoStride, &lifos[i],
                          sizeof(lifos[i]));
    }
    memory.Write8(transfer_memory + ShowCursorOffset, 1);
}

void Nmouse::OnUpdate(const Core::Timing::CoreTiming& core_timing) {
    std::scoped_lock lock{mutex};
    const s64 now_ns = core_timing.GetGlobalTimeNs().count();
    if (owner != nullptr) {
        UpdateNmouse(now_ns);
    }
    if (jcms_owner != nullptr) {
        UpdateJcms(now_ns);
    }
}

void Nmouse::UpdateNmouse(s64 now_ns) {
    // The host mouse over the game's window, in the pointer range the application asked for.
    const auto position = emulated_devices->GetMousePosition();
    const s32 x = static_cast<s32>(std::lround(position.x * static_cast<f32>(range_width)));
    const s32 y = static_cast<s32>(std::lround(position.y * static_cast<f32>(range_height)));
    const bool moved = has_last_position && (x != last_x || y != last_y);
    last_x = x;
    last_y = y;
    has_last_position = true;

    // With mouse panning on, the mouse is the camera: the Joy-Con stays lifted, and moves made
    // meanwhile do not count, so turning panning off leaves it lifted until the mouse moves.
    const bool panning = Settings::values.mouse_panning.GetValue();
    if (panning) {
        has_moved = false;
    } else if (moved) {
        last_moved_ns = now_ns;
        has_moved = true;
    }
    const bool on_table = has_moved && now_ns - last_moved_ns < LiftAfterNs;
    hid_core.SetNmouseActive(on_table);

    // Wheel notches since the previous sample. Turns while the Joy-Con is lifted are dropped, so
    // they do not arrive in one burst when it goes back on the table.
    const auto wheel = emulated_devices->GetMouseWheel();
    const s32 wheel_x = has_last_wheel ? wheel.x - last_wheel_x : 0;
    const s32 wheel_y = has_last_wheel ? wheel.y - last_wheel_y : 0;
    last_wheel_x = wheel.x;
    last_wheel_y = wheel.y;
    has_last_wheel = true;
    UpdateWheelDpad(wheel_y, now_ns);

    const u32 buttons = on_table ? emulated_devices->GetMouseButtons().raw : 0;

    auto& memory = owner->GetMemory();
    for (std::size_t i = 0; i < Slots.size(); ++i) {
        NmouseDevState state{};
        state.sampling_number = lifos[i].ReadCurrentEntry().state.sampling_number + 1;
        state.range_width = range_width;
        state.range_height = range_height;
        if (Slots[i].moves) {
            state.x = x;
            state.y = y;
            if (on_table) {
                state.wheel_x = wheel_x;
                state.wheel_y = wheel_y;
                state.buttons = buttons;
            }
            state.attribute = AttributeConnected | (on_table ? AttributeActive : 0);
        }
        lifos[i].timestamp = now_ns;
        lifos[i].WriteNextEntry(state);
        memory.WriteBlock(transfer_memory + LifoOffset + i * LifoStride, &lifos[i],
                          sizeof(lifos[i]));
    }
}

void Nmouse::UpdateWheelDpad(s32 wheel_y, s64 now_ns) {
    // Small steps (touchpads) add up to a notch; at most eight notches wait their turn.
    wheel_pending = std::clamp(wheel_pending + wheel_y, -8 * WheelNotch, 8 * WheelNotch);
    if (now_ns < wheel_tap_until_ns) {
        return;
    }
    if (wheel_tap_pressed) {
        wheel_tap_pressed = false;
        hid_core.SetNmouseWheelDpad(0);
        wheel_tap_until_ns = now_ns + WheelTapGapNs;
        return;
    }
    if (std::abs(wheel_pending) >= WheelNotch) {
        const s8 direction = wheel_pending > 0 ? 1 : -1;
        wheel_pending -= direction * WheelNotch;
        wheel_tap_pressed = true;
        hid_core.SetNmouseWheelDpad(direction);
        wheel_tap_until_ns = now_ns + WheelTapPressNs;
    }
}

void Nmouse::UpdateJcms(s64 now_ns) {
    // Player 1's rings (npad 0: index 0 left, 1 right) keep counting with empty states, as
    // Recompia does: nothing in them is known to matter beyond being current.
    auto& memory = jcms_owner->GetMemory();
    for (std::size_t i = 0; i < jcms_lifos.size(); ++i) {
        JcmsState state{};
        state.sampling_number = jcms_lifos[i].ReadCurrentEntry().state.sampling_number + 1;
        jcms_lifos[i].timestamp = now_ns;
        jcms_lifos[i].WriteNextEntry(state);
        memory.WriteBlock(jcms_memory + i * sizeof(JcmsLifo), &jcms_lifos[i],
                          sizeof(jcms_lifos[i]));
    }
}

} // namespace Service::HID
