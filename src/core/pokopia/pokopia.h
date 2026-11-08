// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Core::Pokopia {

/// Pokémon Pokopia 2.0.1's PC features (the mouse camera, the mouse in menus), after
/// featureJosh/Recompia. Called by the NSO loader for every main module before it is mapped:
/// `image` is the module from its base, the text segment is `text_size` bytes at `text_offset`
/// and maps `text_mapped` bytes. Other games and versions are left alone.
void Install(Core::System& system, std::span<u8> image, u64 load_base, u64 program_id,
             std::span<const u8> build_id, std::size_t text_offset, std::size_t text_size,
             std::size_t text_mapped);

} // namespace Core::Pokopia
