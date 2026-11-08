// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>

#include "common/common_types.h"
#include "core/hle/kernel/svc.h"

namespace Core::Pokopia {

/// Puts game hooks into a main module while the loader builds it (before it is mapped). A hook at
/// a site runs host code before the instruction there: the instruction becomes a branch to a stub
/// in the text segment's padding, which runs svc 0x7d (svc.cpp calls the hook), then the replaced
/// instruction (relocated when it reads the PC), then branches back.
class HookInstaller {
public:
    HookInstaller(std::span<u8> image, u64 load_base, std::size_t text_offset,
                  std::size_t text_size, std::size_t text_mapped);

    /// `site` is an offset in the module. False (logged) when the instruction cannot be moved or
    /// the padding is full; the module is then unchanged at that site.
    bool Add(std::size_t site, Kernel::Svc::GuestHook hook, const char* what);

private:
    std::span<u8> image;
    u64 load_base;
    std::size_t text_offset;
    std::size_t text_end;
    std::size_t text_mapped_end;
    std::size_t next_stub;
};

} // namespace Core::Pokopia
