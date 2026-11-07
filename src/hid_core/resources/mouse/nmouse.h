// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <utility>

#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/typed_address.h"
#include "hid_core/resources/controller_base.h"
#include "hid_core/resources/ring_lifo.h"

namespace Kernel {
class KProcess;
}

namespace Core::HID {
class EmulatedDevices;
}

namespace Service::HID {

// The Switch 2 mouse (nn::hid::InitializeNmouse, hid 3139): the application hands hid a transfer
// memory and reads the mice from it, Joy-Con 2 mouse sensors included. Fed here from the host
// mouse, as player 1's Joy-Con mice (and the merged mouse).
class Nmouse final : public ControllerBase {
public:
    explicit Nmouse(Core::HID::HIDCore& hid_core_);
    ~Nmouse() override;

    void OnInit() override;
    void OnRelease() override;
    void OnUpdate(const Core::Timing::CoreTiming& core_timing) override;

    void Initialize(Common::ProcessAddress t_mem, u64 size, Kernel::KProcess* owner, s32 width,
                    s32 height);
    void Finalize();

    // hid 3110 / 3111 (nn::hid::InitializeJcms / FinalizeJcms): the second buffer the SDK reads,
    // one lifo per JcmsHandle (npad * 2 + type). Player 1's two rings are kept sampling so
    // GetJcmsStates always has a current entry.
    void InitializeJcms(Common::ProcessAddress t_mem, u64 size, Kernel::KProcess* owner);
    void FinalizeJcms();

    // hid 3132 / 3133 (SetNmousePointerRanges / GetNmousePointerRanges).
    void SetPointerRange(s32 width, s32 height);
    std::pair<s32, s32> GetPointerRange();

    // NmouseType bits as the application asks for them (GetNmouseHandles, GetMergedNmouseHandle).
    static constexpr u32 TypeMerged = 1u << 0;
    static constexpr u32 TypeJoyLeftJcms = 1u << 1;
    static constexpr u32 TypeJoyRightJcms = 1u << 2;

private:
    // The layout the SDK's NmouseTransferMemoryAccessor reads.
    static constexpr std::size_t MaxHandles = 17;
    static constexpr std::size_t LifoOffset = 0x220;
    static constexpr std::size_t LifoStride = 0x818;
    static constexpr std::size_t InfoOffset = 0x8c40;
    static constexpr std::size_t InfoStride = 0x1c;
    static constexpr std::size_t ShowCursorOffset = 0x8e68;
    static constexpr std::size_t MinimumSize = ShowCursorOffset + 1;

    struct HandleEntry {
        u64 handle;
        u8 is_valid;
        INSERT_PADDING_BYTES(7);
    };
    static_assert(sizeof(HandleEntry) == 0x10);

    struct NmouseInfo {
        u32 type_set;
        u8 is_type_set_valid;
        INSERT_PADDING_BYTES(3);
        u32 npad_id;
        u8 is_npad_id_valid;
        INSERT_PADDING_BYTES(3);
        u8 is_pointer_enabled;
        INSERT_PADDING_BYTES(0xb);
    };
    static_assert(sizeof(NmouseInfo) == InfoStride);

    // The SDK copies this into nn::hid::NmouseState (0x30 bytes): +0x00 to +0x24 as they are, then
    // +0x30 to +0x24, +0x28 and +0x2c as they are. Field meanings follow Recompia's reading of the
    // SDK 20.5 client (featureJosh/Recompia, runtime/docs/hid_mouse_sw2.md): its DevNmouseState
    // conversion reads +0x18/+0x1c as the pointer range, and GetWheelDeltaDev returns +0x20 and
    // +0x30, so the state carries no movement delta.
    struct NmouseDevState {
        u64 unknown_00;
        s64 sampling_number; // NmouseState +0x08
        s32 x; // NmouseState +0x10, the pointer, in the range given to InitializeNmouse
        s32 y; // NmouseState +0x14
        s32 range_width;  // NmouseState +0x18, the pointer range (InitializeNmouse or
        s32 range_height; // NmouseState +0x1c  SetNmousePointerRanges)
        s32 wheel_x;      // NmouseState +0x20, first value of GetWheelDeltaDev
        INSERT_PADDING_BYTES(0x4); // development data, not in NmouseState
        u32 buttons;   // NmouseState +0x28, nn::hid::MouseButton bits (no reader seen; Pokopia takes
                       // the Joy-Con's ZR / R from the npad, see npad.cpp)
        u32 attribute; // NmouseState +0x2c, bit 1 must be set for applications to take the mouse as
                       // in use
        s32 wheel_y;   // NmouseState +0x24, second value of GetWheelDeltaDev
        INSERT_PADDING_BYTES(0x3c);
    };
    static_assert(sizeof(NmouseDevState) == 0x70);
    static_assert(offsetof(NmouseDevState, sampling_number) == 0x08);
    static_assert(offsetof(NmouseDevState, x) == 0x10);
    static_assert(offsetof(NmouseDevState, y) == 0x14);
    static_assert(offsetof(NmouseDevState, range_width) == 0x18);
    static_assert(offsetof(NmouseDevState, range_height) == 0x1c);
    static_assert(offsetof(NmouseDevState, wheel_x) == 0x20);
    static_assert(offsetof(NmouseDevState, buttons) == 0x28);
    static_assert(offsetof(NmouseDevState, attribute) == 0x2c);
    static_assert(offsetof(NmouseDevState, wheel_y) == 0x30);

    using NmouseLifo = Lifo<NmouseDevState, MaxHandles>;
    static_assert(sizeof(NmouseLifo) == LifoStride);

    // The Jcms block (nn::hid::InitializeJcms, the game's 0x4000 buffer): GetJcmsStates reads the
    // lifo at index * 0x2c8, index = npad * 2 + type (0 left, 1 right), npad < 8.
    struct JcmsState {
        u64 unknown_00;
        s64 sampling_number; // +0x08
        u64 unknown_10; // read by Pokopia's placement code (non-zero turns the player toward the
                        // cursor); left 0
        u64 unknown_18;
    };
    static_assert(sizeof(JcmsState) == 0x20);
    static constexpr std::size_t JcmsLifoEntries = 17;
    static constexpr std::size_t JcmsRings = 16;
    static constexpr std::size_t JcmsSampledRings = 2; // player 1's left and right Joy-Con
    using JcmsLifo = Lifo<JcmsState, JcmsLifoEntries>;
    static_assert(sizeof(JcmsLifo) == 0x2c8);
    static constexpr std::size_t JcmsMinimumSize = JcmsRings * sizeof(JcmsLifo);

    static constexpr u32 AttributeConnected = 1u << 0;
    static constexpr u32 AttributeActive = 1u << 1;

    struct Slot {
        u32 type_set;
        bool has_npad;
        bool moves;
    };
    // 0: player 1's left Joy-Con mouse, 1: its right one, 2: the merged mouse. The host mouse is
    // the right Joy-Con on the table: an application that took the left one as a mouse too would
    // stop reading its buttons (L, ZL).
    static constexpr std::array<Slot, 3> Slots{{
        {TypeJoyLeftJcms, true, false},
        {TypeJoyRightJcms, true, true},
        {TypeMerged, false, true},
    }};

    void WriteLayout();
    void UpdateNmouse(s64 now_ns);
    void UpdateJcms(s64 now_ns);
    void UpdateWheelDpad(s32 wheel_y, s64 now_ns);

    std::mutex mutex;
    Common::ProcessAddress transfer_memory{};
    Kernel::KProcess* owner{};
    s32 range_width{1920};
    s32 range_height{1080};
    bool has_last_position{};
    // A Joy-Con mouse is on the table while it is moved, lifted after a while still (so games leave
    // their mouse mode, as with the console's Joy-Con: Pokopia only lets moves be changed that
    // way).
    static constexpr s64 LiftAfterNs = 2'500'000'000;
    s64 last_moved_ns{};
    bool has_moved{};
    s32 last_x{};
    s32 last_y{};
    std::array<NmouseLifo, Slots.size()> lifos{};
    // Host wheel position at the previous sample (Qt reports 120 per notch, the unit the game's
    // +-600 clamp suggests).
    s32 last_wheel_x{};
    s32 last_wheel_y{};
    bool has_last_wheel{};

    // The wheel also taps player 1's D-pad, one tap per notch (up away from the user), as Recompia
    // binds it: the Joy-Con mouse has no wheel of its own for the game to act on.
    static constexpr s32 WheelNotch = 120;
    static constexpr s64 WheelTapPressNs = 50'000'000;
    static constexpr s64 WheelTapGapNs = 50'000'000;
    s32 wheel_pending{};
    bool wheel_tap_pressed{};
    s64 wheel_tap_until_ns{};

    Common::ProcessAddress jcms_memory{};
    Kernel::KProcess* jcms_owner{};
    std::array<JcmsLifo, JcmsSampledRings> jcms_lifos{};

    Core::HID::EmulatedDevices* emulated_devices{};
};

} // namespace Service::HID
