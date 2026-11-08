// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Core::Pokopia {

class HookInstaller;

/// Pokémon Pokopia 2.0.1's menus with the mouse, after featureJosh/Recompia
/// (games/pkp/pkp_menu.cpp): the game's menu layouts are read from its memory to find the entry
/// under the pointer and the game's own selection. A left click on an entry moves the selection
/// there with D-pad steps, checking where it went after each one, then presses A; a left click
/// anywhere else is A, a right click is B.
bool InstallMenuMouse(Core::System& system, HookInstaller& hooks, u64 main_base);

} // namespace Core::Pokopia
