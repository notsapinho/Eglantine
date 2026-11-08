// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon::TextureMods {

/// Texture dumps and replacements, keyed by a hash of the texture's guest bytes (as the game
/// stores it, before any conversion), so a texture is found again in every run.
///
/// Dump: EDEN_TEXTURE_DUMP=<folder> writes each tiled texture the first time it is uploaded:
/// <hash>_<w>x<h>.bin (the host data of every level and layer, as uploaded: rows of blocks) and
/// <hash>_<w>x<h>.json (format, size, levels, layers, and where each level starts).
///
/// Replace: every <load folder>/<title id>/<mod>/textures/<hash>_<w>x<h>.bin replaces that
/// texture's host data when it is uploaded; its size must match the dump's.
bool Active();

/// After a tiled texture was untiled into `host` (`copies` from UnswizzleImage): dumps it, and
/// overwrites `host` with a replacement when one matches. `guest` is the tiled guest data.
void OnUpload(const ImageInfo& info, std::span<const u8> guest, std::span<u8> host,
              std::span<const BufferImageCopy> copies);

} // namespace VideoCommon::TextureMods
