// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Core::Pokopia {

class HookInstaller;

/// Pokémon Pokopia 2.0.1's mouse camera, after featureJosh/Recompia (games/pkp/pkp_play.cpp):
/// with mouse panning on, the mouse turns the follow camera's yaw and pitch directly, by an exact
/// angle per pixel, instead of through an emulated right stick.
bool InstallMouseCamera(Core::System& system, HookInstaller& hooks, u64 main_base);

} // namespace Core::Pokopia
