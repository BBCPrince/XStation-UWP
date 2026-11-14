// SPDX-License-Identifier: GPL-2.0-or-later
// TLOU (2026-11-09): saved pipelines loaded per level area (BB_PIPELINE_AREAS=1). The preload
// loaded every saved pipeline at start, so memory and start time grew with the cache through a
// playthrough. Now the shaders still load at start, but a graphics pipeline whose shaders belong
// only to level areas (areas.txt from tools/psarc_area_index.py, made from the game's own archives)
// waits until the game reads one of those areas (runtime_file.c note_area_read), and is then set up
// on the GPU thread and built by the background pipeline workers, as a pipeline new to a draw is.
#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

namespace Vulkan {

namespace PipelineAreas {

/// The level area the game reads: the world-*.psarc with 4 MiB read since the area last changed.
struct Tracker {
    std::mutex mutex;
    std::unordered_map<std::string, u64> bytes;
    std::string area;
    std::atomic<u32> epoch{0};
};

inline Tracker& Get() {
    static Tracker tracker;
    return tracker;
}

/// Game thread: `bytes` read from a level archive.
inline void Read(std::string_view archive, u64 bytes) {
    auto& t = Get();
    std::string name{archive};
    if (const auto cut = name.find_last_of("/\\"); cut != std::string::npos) {
        name.erase(0, cut + 1);
    }
    if (const auto dot = name.rfind(".psarc"); dot != std::string::npos) {
        name.erase(dot);
    }
    std::scoped_lock lock{t.mutex};
    auto& total = t.bytes[name];
    total += bytes;
    if (total >= (4ull << 20) && t.area != name) {
        t.area = name;
        for (auto& [other, count] : t.bytes) {
            if (other != name) {
                count = 0;
            }
        }
        t.epoch.fetch_add(1, std::memory_order_release);
    }
}

inline std::string Current() {
    auto& t = Get();
    std::scoped_lock lock{t.mutex};
    return t.area;
}

} // namespace PipelineAreas

struct PipelineAreaState {
    std::vector<std::string> names;                         ///< area id -> name
    std::unordered_map<u64, std::vector<u16>> shader_areas; ///< program hash -> its areas

    /// A saved graphics pipeline not built at start: what the preload had read for it.
    struct Deferred {
        GraphicsPipelineKey key{};
        std::array<const Shader::Info*, MaxShaderStages> infos{};
        std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
        GraphicsPipeline::SerializationSupport sdata{};
        bool queued = false;
    };
    std::vector<Deferred> deferred;
    std::vector<std::vector<u32>> by_area; ///< area id -> indices into deferred

    // GPU thread, while the game runs
    u32 seen_epoch = 0;
    u32 seen_builds = 0;
    std::vector<const GraphicsPipeline*> building;
    std::vector<GraphicsPipelineKey> building_keys;
    u64 queued = 0, built = 0, failed = 0;

    bool Load(const std::filesystem::path& file) {
        std::ifstream in{file};
        if (!in) {
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            const auto space = line.find(' ');
            if (line.empty() || space == std::string::npos) {
                continue;
            }
            const u16 id = u16(names.size());
            names.push_back(line.substr(0, space));
            size_t at = space + 1;
            while (at < line.size()) {
                size_t end = line.find(' ', at);
                if (end == std::string::npos) {
                    end = line.size();
                }
                if (end > at) {
                    const u64 hash = std::strtoull(line.c_str() + at, nullptr, 16);
                    shader_areas[hash].push_back(id);
                }
                at = end + 1;
            }
        }
        by_area.resize(names.size());
        return !names.empty();
    }

    int AreaId(const std::string& name) const {
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) {
                return int(i);
            }
        }
        return -1;
    }

    /// The areas a pipeline is used in: those all its area-only shaders share (any of them when
    /// they share none). Empty: no area-only shader, the pipeline is shared.
    std::vector<u16> AreasOf(const std::array<const Shader::Info*, MaxShaderStages>& infos) const {
        std::vector<u16> result;
        std::vector<u16> any;
        bool first = true;
        for (const auto* info : infos) {
            if (!info) {
                continue;
            }
            const auto it = shader_areas.find(info->pgm_hash);
            if (it == shader_areas.end()) {
                continue;
            }
            const auto& areas = it->second;
            for (const u16 a : areas) {
                if (std::find(any.begin(), any.end(), a) == any.end()) {
                    any.push_back(a);
                }
            }
            if (first) {
                result = areas;
                first = false;
            } else {
                std::erase_if(result, [&](u16 a) {
                    return std::find(areas.begin(), areas.end(), a) == areas.end();
                });
            }
        }
        return result.empty() ? any : result;
    }
};

} // namespace Vulkan
