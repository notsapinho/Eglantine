// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/texture_cache/texture_mods.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/surface.h"

namespace VideoCommon::TextureMods {

namespace {

namespace fs = std::filesystem;
using VideoCore::Surface::PixelFormat;

const char* FormatName(PixelFormat format) {
    switch (format) {
#define PIXEL_FORMAT_ELEM(name, ...)                                                               \
    case PixelFormat::name:                                                                        \
        return #name;
        PIXEL_FORMAT_LIST
#undef PIXEL_FORMAT_ELEM
    default:
        return "unknown";
    }
}

struct Replacement {
    fs::path path;
    std::size_t size = 0;
    std::vector<u8> data; // read on first use
};

struct State {
    std::mutex mutex;
    bool loaded = false;
    fs::path dump_dir;
    std::set<u64> dumped;
    // "<hash>_<w>x<h>" -> the replacement; the sizes to hash at all
    std::unordered_map<std::string, Replacement> replacements;
    std::set<std::pair<u32, u32>> sizes;
};

State& St() {
    static State state;
    return state;
}

std::string Key(u64 hash, u32 width, u32 height) {
    return fmt::format("{:016x}_{}x{}", hash, width, height);
}

void Load(State& s) {
    if (s.loaded) {
        return;
    }
    s.loaded = true;
    if (const char* dir = std::getenv("EDEN_TEXTURE_DUMP"); dir != nullptr && dir[0] != 0) {
        s.dump_dir = dir;
        std::error_code ec;
        fs::create_directories(s.dump_dir, ec);
        LOG_INFO(HW_GPU, "Texture dump: writing new tiled textures to {}", s.dump_dir.string());
    }
    // <load>/<title>/<mod>/textures/<hash>_<w>x<h>.bin
    const fs::path load = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir);
    std::error_code ec;
    if (!fs::is_directory(load, ec)) {
        return;
    }
    for (const auto& title : fs::directory_iterator(load, ec)) {
        if (!title.is_directory(ec)) {
            continue;
        }
        for (const auto& mod : fs::directory_iterator(title.path(), ec)) {
            const fs::path textures = mod.path() / "textures";
            if (!fs::is_directory(textures, ec)) {
                continue;
            }
            for (const auto& file : fs::directory_iterator(textures, ec)) {
                if (file.path().extension() != ".bin") {
                    continue;
                }
                const std::string stem = file.path().stem().string();
                unsigned long long hash = 0;
                unsigned width = 0, height = 0;
                if (std::sscanf(stem.c_str(), "%16llx_%ux%u", &hash, &width, &height) != 3) {
                    continue;
                }
                s.replacements[Key(hash, width, height)] =
                    Replacement{file.path(), static_cast<std::size_t>(file.file_size(ec)), {}};
                s.sizes.emplace(width, height);
                LOG_INFO(HW_GPU, "Texture replacement: {} ({})", stem, mod.path().filename().string());
            }
        }
    }
}

void Dump(State& s, const ImageInfo& info, u64 hash, std::span<const u8> host,
          std::span<const BufferImageCopy> copies) {
    if (!s.dumped.insert(hash).second) {
        return;
    }
    const std::string base = Key(hash, info.size.width, info.size.height);
    std::ofstream bin(s.dump_dir / (base + ".bin"), std::ios::binary);
    bin.write(reinterpret_cast<const char*>(host.data()), static_cast<std::streamsize>(host.size()));
    std::string levels;
    for (const BufferImageCopy& copy : copies) {
        levels += fmt::format("{}{{\"offset\": {}, \"size\": {}, \"width\": {}, \"height\": {}}}",
                              levels.empty() ? "" : ", ", copy.buffer_offset, copy.buffer_size,
                              copy.image_extent.width, copy.image_extent.height);
    }
    std::ofstream json(s.dump_dir / (base + ".json"));
    json << fmt::format("{{\"format\": \"{}\", \"width\": {}, \"height\": {}, \"depth\": {}, "
                        "\"layers\": {}, \"levels\": [{}]}}\n",
                        FormatName(info.format), info.size.width, info.size.height,
                        info.size.depth, info.resources.layers, levels);
}

} // namespace

bool Active() {
    // Mods are read once, at the first upload: new ones need a restart.
    static const bool active = [] {
        State& s = St();
        std::scoped_lock lock{s.mutex};
        Load(s);
        return !s.dump_dir.empty() || !s.replacements.empty();
    }();
    return active;
}

void OnUpload(const ImageInfo& info, std::span<const u8> guest, std::span<u8> host,
              std::span<const BufferImageCopy> copies) {
    State& s = St();
    std::scoped_lock lock{s.mutex};
    Load(s);
    const bool dump = !s.dump_dir.empty();
    if (!dump && !s.sizes.contains({info.size.width, info.size.height})) {
        return;
    }
    std::size_t host_size = 0;
    for (const BufferImageCopy& copy : copies) {
        host_size = std::max(host_size, copy.buffer_offset + copy.buffer_size);
    }
    host_size = std::min(host_size, host.size());
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(guest.data()), guest.size());
    if (dump) {
        Dump(s, info, hash, host.first(host_size), copies);
    }
    const auto it = s.replacements.find(Key(hash, info.size.width, info.size.height));
    if (it == s.replacements.end()) {
        return;
    }
    Replacement& r = it->second;
    if (r.size != host_size) {
        LOG_ERROR(HW_GPU, "Texture replacement {}: {} bytes, the texture has {}",
                  r.path.filename().string(), r.size, host_size);
        return;
    }
    if (r.data.empty()) {
        r.data.resize(r.size);
        std::ifstream in(r.path, std::ios::binary);
        in.read(reinterpret_cast<char*>(r.data.data()), static_cast<std::streamsize>(r.size));
        if (!in) {
            LOG_ERROR(HW_GPU, "Texture replacement {}: cannot read it", r.path.string());
            r.data.clear();
            return;
        }
        LOG_INFO(HW_GPU, "Texture replacement {}: in use", r.path.filename().string());
    }
    std::copy(r.data.begin(), r.data.end(), host.begin());
}

} // namespace VideoCommon::TextureMods
