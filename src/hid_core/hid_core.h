// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <utility>
#include <mutex>
#include <memory>

#include "common/common_funcs.h"
#include "hid_core/hid_types.h"

namespace Kernel {
class KernelCore;
}

namespace Core::HID {
class EmulatedConsole;
class EmulatedController;
class EmulatedDevices;
} // namespace Core::HID

namespace Core::HID {

class HIDCore {
public:
    explicit HIDCore(Kernel::KernelCore& kernel);
    ~HIDCore();

    YUZU_NON_COPYABLE(HIDCore);
    YUZU_NON_MOVEABLE(HIDCore);

    EmulatedController* GetEmulatedController(NpadIdType npad_id_type);
    const EmulatedController* GetEmulatedController(NpadIdType npad_id_type) const;

    EmulatedController* GetEmulatedControllerByIndex(std::size_t index);
    const EmulatedController* GetEmulatedControllerByIndex(std::size_t index) const;

    EmulatedConsole* GetEmulatedConsole();
    const EmulatedConsole* GetEmulatedConsole() const;

    EmulatedDevices* GetEmulatedDevices();
    const EmulatedDevices* GetEmulatedDevices() const;

    void SetSupportedStyleTag(NpadStyleTag style_tag);
    NpadStyleTag GetSupportedStyleTag() const;

    /// Counts the connected players from P1-P8
    s8 GetPlayerCount() const;

    /// Returns the first connected npad id
    NpadIdType GetFirstNpadId() const;

    /// Returns the first disconnected npad id
    NpadIdType GetFirstDisconnectedNpadId() const;

    /// Sets the npad id of the last active controller
    void SetLastActiveController(NpadIdType npad_id);

    /// Returns the npad id of the last controller that pushed a button
    NpadIdType GetLastActiveController() const;

    /// An application reads the Switch 2 mouse (nn::hid::InitializeNmouse): the host mouse is
    /// player 1's right Joy-Con mouse, it draws its own pointer, and the host mouse buttons are
    /// that Joy-Con's ZR and R.
    void SetNmouseInUse(bool in_use);
    /// Also needs player 1 on Dual or right Joy-Cons, the controllers that are a mouse.
    bool IsNmouseInUse() const;

    /// The Joy-Con 2 mouse is on its surface: only then are the host mouse buttons its ZR and R.
    void SetNmouseActive(bool active);
    bool IsNmouseActive() const;

    /// The D-pad direction the mouse wheel is tapping on player 1: 1 up, -1 down, 0 none.
    void SetNmouseWheelDpad(s8 direction);
    s8 GetNmouseWheelDpad() const;

    /// A game hook turns the camera with the mouse (core/pokopia): with mouse panning on, the
    /// window hands its mouse movement here instead of to the emulated right stick.
    void SetMouseCameraHooked(bool hooked);
    bool IsMouseCameraHooked() const;
    void AddMouseLook(s32 dx, s32 dy);
    /// The movement since the last call, in window pixels (y down).
    std::pair<s32, s32> TakeMouseLook();

    /// Sets all emulated controllers into configuring mode.
    void EnableAllControllerConfiguration();

    /// Sets all emulated controllers into normal mode.
    void DisableAllControllerConfiguration();

    /// Reloads all input devices from settings
    void ReloadInputDevices();

    /// Removes all callbacks from input common
    void UnloadInputDevices();

    /// Number of emulated controllers
    static constexpr std::size_t available_controllers{10};

    std::unique_ptr<EmulatedController> player_1;
    std::unique_ptr<EmulatedController> player_2;
    std::unique_ptr<EmulatedController> player_3;
    std::unique_ptr<EmulatedController> player_4;
    std::unique_ptr<EmulatedController> player_5;
    std::unique_ptr<EmulatedController> player_6;
    std::unique_ptr<EmulatedController> player_7;
    std::unique_ptr<EmulatedController> player_8;
    std::unique_ptr<EmulatedController> other;
    std::unique_ptr<EmulatedController> handheld;
    std::unique_ptr<EmulatedConsole> console;
    std::unique_ptr<EmulatedDevices> devices;
    Kernel::KernelCore& kernel;
    NpadStyleTag supported_style_tag{NpadStyleSet::All};
    NpadIdType last_active_controller{NpadIdType::Handheld};
    std::atomic<bool> nmouse_in_use{false};
    std::atomic<bool> nmouse_active{false};
    std::atomic<s8> nmouse_wheel_dpad{0};
    std::atomic<bool> mouse_camera_hooked{false};
    std::mutex mouse_look_mutex;
    s32 mouse_look_x{};
    s32 mouse_look_y{};
};

} // namespace Core::HID
