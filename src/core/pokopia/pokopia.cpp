// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/pokopia/pokopia.h"

#include <algorithm>
#include <array>
#include <cstdlib>

#include "common/logging.h"
#include "common/settings.h"
#include "core/pokopia/hooks.h"
#include "core/pokopia/menu_mouse.h"
#include "core/pokopia/mouse_camera.h"

namespace Core::Pokopia {

namespace {

constexpr u64 ProgramId = 0x0400A3A01EE4C000ULL;
// Main module build ID of version 2.0.1 (the same build as Recompia's and the FPS mods').
constexpr std::array<u8, 20> BuildId{0x76, 0xA4, 0x89, 0xF3, 0xBD, 0x36, 0x42, 0xFA, 0x5E, 0x18,
                                     0xBD, 0x83, 0xD3, 0x2C, 0x98, 0xBA, 0xCC, 0x5F, 0x7D, 0x0E};

bool Off(const char* variable) {
    const char* value = std::getenv(variable);
    return value != nullptr && value[0] == '0';
}

} // namespace

void Install(Core::System& system, std::span<u8> image, u64 load_base, u64 program_id,
             std::span<const u8> build_id, std::size_t text_offset, std::size_t text_size,
             std::size_t text_mapped) {
    if (program_id != ProgramId || build_id.size() < BuildId.size() ||
        !std::equal(BuildId.begin(), BuildId.end(), build_id.begin())) {
        return;
    }
    if (Settings::IsNceEnabled()) {
        // NCE runs the game's code natively, where a stub's svc would not reach the hooks.
        LOG_INFO(Loader, "Pokopia mouse features: not available with the NCE CPU backend");
        return;
    }
    HookInstaller hooks{image, load_base, text_offset, text_size, text_mapped};
    if (Off("EDEN_POKOPIA_MOUSE_CAMERA")) {
        LOG_INFO(Loader, "Pokopia mouse camera: off (EDEN_POKOPIA_MOUSE_CAMERA=0)");
    } else {
        InstallMouseCamera(system, hooks, load_base);
    }
    if (Off("EDEN_POKOPIA_MENU_MOUSE")) {
        LOG_INFO(Loader, "Pokopia menu mouse: off (EDEN_POKOPIA_MENU_MOUSE=0)");
    } else {
        InstallMenuMouse(system, hooks, load_base);
    }
}

} // namespace Core::Pokopia
