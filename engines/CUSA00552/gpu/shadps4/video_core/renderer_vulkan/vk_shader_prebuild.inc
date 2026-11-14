// TLOU (BB_SHADER_PREBUILD=<folder of packs from tools/psarc_make_packs.py>, 2026-11-09). Builds shaders ahead of
// time from the game files: the settings stored with each shader give its stage registers, and what is not stored
// (descriptors, render targets) is guessed from what a saved cache showed. A guess is only used when the draw's real
// settings match it (PrebuiltMatches); otherwise the shader is translated at the draw, as without this. What a draw
// used is saved to the shader cache as usual, under the draw's real settings.
//
// Only the common archives and the area the game is reading stay in memory.

#include "vk_shader_prebuild_tables.inc"

namespace {

constexpr u64 IntegerFetchShaderA = 0xff56b297ULL; // the two shaders whose code reads the mip and sampler state
constexpr u64 IntegerFetchShaderB = 0xf95941f6ULL;

/// As StageSpecialization::operator==, leaving out what does not change the SPIR-V of these shaders (tested on every
/// variant in a saved cache, BB_SHADER_REPLAY_FLIP): sRGB and the mip count, and the sampler's point/clamp state.
const char* PrebuiltMismatch(const Shader::StageSpecialization& real, const Shader::StageSpecialization& guess, u64 hash) {
    const bool exact = hash == IntegerFetchShaderA || hash == IntegerFetchShaderB;
    if (real.bitset != guess.bitset) return "null descriptors";
    if (!(real.runtime_info == guess.runtime_info)) {
        if (real.runtime_info.hw_stage == Shader::HwStage::Fragment) {
            const auto& a = real.runtime_info.hw.fs;
            const auto& b = guess.runtime_info.hw.fs;
            if (!std::ranges::equal(a.color_buffers, b.color_buffers)) return "render targets";
            if (a.num_inputs != b.num_inputs || !std::ranges::equal(a.inputs.begin(), a.inputs.begin() + a.num_inputs,
                                                                     b.inputs.begin(), b.inputs.begin() + b.num_inputs))
                return "inputs";
        }
        return "registers";
    }
    if (real.vs_attribs != guess.vs_attribs || real.fetch_shader_data != guess.fetch_shader_data) return "vertex input";
    if (real.fmasks != guess.fmasks) return "fmask";
    if (real.buffers.size() != guess.buffers.size() || real.images.size() != guess.images.size() ||
        real.samplers.size() != guess.samplers.size())
        return "resource count";
    if (real.bitset.none() && real.buffers.empty() && real.images.empty() && real.samplers.empty()) return nullptr;
    if (real.start != guess.start) return "binding start";
    u32 binding{};
    for (size_t i = 0; i < real.buffers.size(); ++i) {
        if (real.bitset[binding++] && real.buffers[i] != guess.buffers[i]) return "buffer";
    }
    for (size_t i = 0; i < real.images.size(); ++i) {
        if (!real.bitset[binding++]) continue;
        auto a = real.images[i];
        auto b = guess.images[i];
        if (!exact) {
            a.is_srgb = b.is_srgb = false;
            a.integer_fetch_single_mip = b.integer_fetch_single_mip = false;
        }
        if (a != b) return "image";
    }
    for (size_t i = 0; i < real.samplers.size(); ++i) {
        auto a = real.samplers[i];
        auto b = guess.samplers[i];
        if (!exact) {
            a.integer_fetch_point_clamp = b.integer_fetch_point_clamp = 0;
        }
        if (a != b) return "sampler";
    }
    return nullptr;
}

/// Between finding a shader's resources and using them: the guessed descriptors.
void GuessSharps(Shader::Info& info) {
    auto& flat = info.flattened_ud_buf;
    const auto write = [&flat](const auto& fetch, const u32* sharp, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            if (((fetch.load_mask >> i) & 1) && fetch.offsets[i] != Shader::UNKNOWN_LOCATION && fetch.offsets[i] < flat.size()) {
                flat[fetch.offsets[i]] = sharp[i];
            }
        }
    };
    for (const auto& image : info.images) {
        const u8 key[7]{image.is_depth,  image.is_atomic,    image.is_array,          image.is_written,
                        image.is_r128,   image.needs_native, u8(image.mip_fallback_mode)};
        // Unseen kinds take the plain sampled image.
        const u32* sharp = kGuessImages[0].sharp;
        bool found = false;
        for (const auto& row : kGuessImages) {
            if (std::memcmp(row.key, key, sizeof(key)) == 0) {
                sharp = row.sharp;
                found = true;
                break;
            }
            if (!found && !row.key[0] && !row.key[1] && !row.key[2] && !row.key[3] && !row.key[4] && !row.key[5]) {
                sharp = row.sharp;
            }
        }
        write(image.sharp_fetch, sharp, 8);
    }
    const u32 hw = u32(info.hw_stage);
    for (const auto& buffer : info.buffers) {
        if (buffer.IsSpecial()) {
            continue;
        }
        static constexpr u32 plain[4]{0x10000u, 0x00000000u, 0x10000u, 0x20000000u};
        const u32* sharp = plain;
        for (const auto& row : kGuessBuffers) {
            if (row.hw == hw && row.written == u32(buffer.is_written) && row.formatted == u32(buffer.is_formatted) &&
                row.used == u32(buffer.used_types)) {
                sharp = row.sharp;
                break;
            }
        }
        write(buffer.sharp_fetch, sharp, 4);
    }
    for (const auto& sampler : info.samplers) {
        const u32* sharp = kGuessSamplers[0].sharp;
        for (const auto& row : kGuessSamplers) {
            if (row.is_depth == u32(sampler.is_depth) && row.post_op == u32(sampler.post_op)) {
                sharp = row.sharp;
                break;
            }
        }
        write(sampler.sharp_fetch, sharp, 4);
    }
}

struct PackShader {
    u64 hash;
    u32 type;
    std::span<const u32> regs;
    std::span<const u32> code;
};

std::vector<PackShader> ParsePack(const std::vector<u8>& pack) {
    std::vector<PackShader> shaders;
    if (pack.size() < 12 || std::memcmp(pack.data(), "GCNPACK2", 8) != 0) {
        return shaders;
    }
    u32 count;
    std::memcpy(&count, pack.data() + 8, 4);
    shaders.reserve(count);
    size_t at = 12;
    for (u32 i = 0; i < count && at + 20 <= pack.size(); ++i) {
        PackShader row{};
        u32 code_bytes, reg_words;
        std::memcpy(&row.hash, pack.data() + at, 8);
        std::memcpy(&row.type, pack.data() + at + 8, 4);
        std::memcpy(&code_bytes, pack.data() + at + 12, 4);
        std::memcpy(&reg_words, pack.data() + at + 16, 4);
        at += 20;
        row.regs = std::span{reinterpret_cast<const u32*>(pack.data() + at), reg_words};
        at += reg_words * 4;
        row.code = std::span{reinterpret_cast<const u32*>(pack.data() + at), code_bytes / 4};
        at += code_bytes;
        shaders.push_back(row);
    }
    return shaders;
}

/// The stage's settings from the registers stored with the shader. False: not a stage built ahead.
bool GuessRuntimeInfo(const PackShader& shader, Shader::RuntimeInfo& runtime) {
    const auto regs = shader.regs;
    if (regs.size() < 5) {
        return false;
    }
    const u32 rsrc1 = regs[0], rsrc2 = regs[1];
    const auto common = [&](Shader::HwStage hw, Shader::SwStage sw) {
        runtime.Initialize(hw, sw);
        runtime.props.fp_round_mode32 = AmdGpu::FpRoundMode((rsrc1 >> 12) & 3);
        runtime.props.fp_round_mode16_64 = AmdGpu::FpRoundMode((rsrc1 >> 14) & 3);
        runtime.props.fp_denorm_mode32 = AmdGpu::FpDenormMode((rsrc1 >> 16) & 3);
        runtime.props.fp_denorm_mode16_64 = AmdGpu::FpDenormMode((rsrc1 >> 18) & 3);
        runtime.props.num_user_data = (rsrc2 >> 1) & 31;
    };
    if (shader.type == 4) {
        if (regs.size() < 5) {
            return false;
        }
        common(Shader::HwStage::Compute, Shader::SwStage::Compute);
        runtime.props.num_allocated_vgprs = (rsrc1 & 63) * 4;
        auto& cs = runtime.hw.cs;
        cs.workgroup_size = {regs[2], regs[3], regs[4]};
        cs.tgid_enable = {bool((rsrc2 >> 7) & 1), bool((rsrc2 >> 8) & 1), bool((rsrc2 >> 9) & 1)};
        cs.shared_memory_size = ((rsrc2 >> 15) & 0x1ff) * 512;
        return true;
    }
    if (shader.type != 0 || regs.size() < 11) {
        return false;
    }
    common(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    runtime.props.num_input_vgprs = (rsrc1 >> 24) & 3;
    runtime.props.num_allocated_vgprs = ((rsrc1 & 63) + 1) * 4;
    auto& fs = runtime.hw.fs;
    fs.z_export_format = AmdGpu::ShaderExportFormat(regs[2]);
    std::memcpy(&fs.en_flags, &regs[4], 4);
    std::memcpy(&fs.addr_flags, &regs[5], 4);
    fs.num_inputs = regs[6] & 63;
    fs.front_face_all_bits = (regs[7] >> 24) & 1;
    fs.mrtz_mask = regs[8] & 1;
    // Input semantics: 16 bits each, 0x10 is the first varying.
    const u32 count = std::min<u32>(regs[10], 32);
    auto tail = regs.subspan(11);
    if (!tail.empty() && (tail[0] & 0xff) == 0x0a && (tail[0] >> 24) != 0) {
        tail = tail.subspan(1);
    }
    for (u32 i = 0; i < fs.num_inputs && i < 32; ++i) {
        u32 semantic = i < count && i / 2 < tail.size() ? (tail[i / 2] >> ((i & 1) * 16)) & 0xffff : 0;
        fs.inputs[i].param_index = u8(semantic >= 0x10 && semantic < 0x30 ? semantic - 0x10 : i);
    }
    // Render targets, as the saved cache shows them for these exports.
    const u32 col_format = regs[3], cb_mask = regs[9];
    u32 exports = 0;
    for (u32 cb = 0; cb < Shader::MaxColorBuffers; ++cb) {
        exports += ((col_format >> (cb * 4)) & 15) != 0;
    }
    for (u32 cb = 0; cb < Shader::MaxColorBuffers; ++cb) {
        const u32 format = (col_format >> (cb * 4)) & 15;
        if (!format || !((cb_mask >> (cb * 4)) & 15)) {
            continue;
        }
        auto& target = fs.color_buffers[cb];
        target.export_format = AmdGpu::ShaderExportFormat(format);
        target.num_conversion = AmdGpu::NumberConversion(0);
        target.blend_self_scale = 0;
        const auto swizzle = [](u32 r, u32 g, u32 b, u32 a) {
            return AmdGpu::CompMapping{AmdGpu::CompSwizzle(r), AmdGpu::CompSwizzle(g), AmdGpu::CompSwizzle(b), AmdGpu::CompSwizzle(a)};
        };
        if (format == 5) { // one channel
            target.data_format = AmdGpu::DataFormat(2);
            target.num_format = AmdGpu::NumberFormat(0);
            target.swizzle = swizzle(4, 0, 0, 0);
        } else if (exports == 1) { // the lighting and post targets
            target.data_format = AmdGpu::DataFormat(12);
            target.num_format = AmdGpu::NumberFormat(7);
            target.swizzle = swizzle(4, 5, 6, 7);
        } else if (((cb_mask >> (cb * 4)) & 15) == 3) { // two channels
            target.data_format = AmdGpu::DataFormat(5);
            target.num_format = AmdGpu::NumberFormat(7);
            target.swizzle = swizzle(4, 5, 0, 0);
        } else { // the G-buffer
            target.data_format = AmdGpu::DataFormat(10);
            target.num_format = AmdGpu::NumberFormat(0);
            target.swizzle = swizzle(4, 5, 6, 7);
        }
    }
    return true;
}

struct PrebuiltEntry {
    u32 meta_size{};
    u32 spv_words{};
    std::vector<u8> meta; ///< compressed: the guess's specialization, then its Info with the SRT walker
    std::vector<u8> spv;  ///< compressed
    size_t Bytes() const { return meta.size() + spv.size() + sizeof(PrebuiltEntry) + 64; }
};
using PrebuiltPtr = std::shared_ptr<const PrebuiltEntry>;
using PrebuiltMap = std::unordered_map<u64, PrebuiltPtr>;

std::vector<u8> Squeeze(const void* data, size_t size) {
    mz_ulong out_size = mz_compressBound(mz_ulong(size));
    std::vector<u8> out(out_size);
    if (mz_compress2(out.data(), &out_size, static_cast<const u8*>(data), mz_ulong(size), 1) != MZ_OK) {
        return {};
    }
    out.resize(out_size);
    out.shrink_to_fit();
    return out;
}

/// One shader on this thread. Kept apart from callers: sigsetjmp must not cross objects with destructors.
PrebuiltPtr BuildOneInner(const PackShader& shader, const Shader::RuntimeInfo& guess, const Shader::Profile& profile,
                          Shader::Pools& pools) {
    static const std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    const Shader::ShaderParams params{std::span<const u32, Shader::ShaderParams::NumShaderUserData>(user_data),
                                      shader.code, shader.hash};
    Shader::Info info(guess.hw_stage, guess.sw_stage, params);
    Shader::RuntimeInfo runtime = guess;
    Shader::Backend::Bindings binding{};
    const auto program = Shader::TranslateProgram(shader.code, pools, info, runtime, profile);
    const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding);
    if (spv.empty() || info.translation_failed) {
        return nullptr;
    }
    const Shader::StageSpecialization spec(info, runtime, profile, Shader::Backend::Bindings{});
    Serialization::Archive ar;
    spec.Serialize(ar);
    info.Serialize(ar);
    const auto meta = ar.TakeOff();
    auto entry = std::make_shared<PrebuiltEntry>();
    entry->meta_size = u32(meta.size());
    entry->spv_words = u32(spv.size());
    entry->meta = Squeeze(meta.data(), meta.size());
    entry->spv = Squeeze(spv.data(), spv.size() * 4);
    return entry;
}

PrebuiltPtr BuildOne(const PackShader& shader, const Shader::Profile& profile, Shader::Pools& pools) {
    Shader::RuntimeInfo guess{};
    if (!GuessRuntimeInfo(shader, guess)) {
        return nullptr;
    }
    PrebuiltPtr entry;
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = nullptr;
        return nullptr;
    }
    runtime_fault_recover = &recover;
    try {
        entry = BuildOneInner(shader, guess, profile, pools);
    } catch (const std::exception&) {
        entry = nullptr;
    }
    runtime_fault_recover = nullptr;
    return entry;
}

bool Unsqueeze(const std::vector<u8>& data, void* out, size_t out_size) {
    mz_ulong size = mz_ulong(out_size);
    return mz_uncompress(static_cast<u8*>(out), &size, data.data(), mz_ulong(data.size())) == MZ_OK && size == out_size;
}

std::vector<u8> PrebuildReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    std::vector<u8> data;
    if (file) {
        data.resize(size_t(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    }
    return data;
}

class PrebuiltStore {
public:
    static PrebuiltStore& Get() {
        static PrebuiltStore store;
        return store;
    }

    bool Enabled() const { return enabled.load(std::memory_order_relaxed); }

    void Start(std::filesystem::path dir_, const Shader::Profile& profile_) {
        dir = std::move(dir_);
        profile = profile_;
        threads = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 3u);
        if (const char* env = std::getenv("BB_SHADER_PREBUILD_THREADS"); env && *env) {
            threads = std::max<u32>(u32(std::strtoul(env, nullptr, 10)), 1u);
        }
        limit_bytes = 300ull << 20;
        if (const char* env = std::getenv("BB_SHADER_PREBUILD_MB"); env && *env) {
            limit_bytes = std::strtoull(env, nullptr, 10) << 20;
        }
        enabled.store(true, std::memory_order_relaxed);
        manager = std::jthread{[this](std::stop_token token) { Run(token); }};
    }

    /// Game thread: `bytes` were read from a level archive.
    void AreaRead(std::string_view archive, u64 bytes) {
        if (!Enabled()) {
            return;
        }
        std::scoped_lock lock{request_mutex};
        std::string name{archive};
        if (const auto cut = name.find_last_of("/\\"); cut != std::string::npos) {
            name.erase(0, cut + 1);
        }
        if (const auto dot = name.rfind(".psarc"); dot != std::string::npos) {
            name.erase(dot);
        }
        // Opening an archive reads its table; playing in an area reads much more.
        auto& total = area_bytes[name];
        total += bytes;
        if (total >= (4ull << 20) && wanted_area != name) {
            std::printf("Shader prebuild: the game is reading %s\n", name.c_str());
            std::fflush(stdout);
            wanted_area = name;
            for (auto& [other, count] : area_bytes) {
                if (other != name) {
                    count = 0;
                }
            }
            request_cv.notify_all();
        }
    }

    PrebuiltPtr Find(u64 hash) {
        std::shared_lock lock{maps_mutex};
        if (const auto it = common.find(hash); it != common.end()) {
            return it->second;
        }
        if (const auto it = area.find(hash); it != area.end()) {
            return it->second;
        }
        return nullptr;
    }

    // Lookups at draws that had no shader yet (GPU thread).
    std::array<u64, 7> counts{}; // pixel: hit, wrong guess, not built; compute: the same; last: others

    void Report() {
        const u64 total = counts[0] + counts[1] + counts[2] + counts[3] + counts[4] + counts[5] + counts[6];
        if ((total & 63) == 0 && total) {
            std::printf("Shader prebuild: %llu shaders new to draws: pixel %llu built ahead, %llu wrong guess, %llu not "
                        "built; compute %llu, %llu, %llu; other stages %llu\n",
                        static_cast<unsigned long long>(total), static_cast<unsigned long long>(counts[0]),
                        static_cast<unsigned long long>(counts[1]), static_cast<unsigned long long>(counts[2]),
                        static_cast<unsigned long long>(counts[3]), static_cast<unsigned long long>(counts[4]),
                        static_cast<unsigned long long>(counts[5]), static_cast<unsigned long long>(counts[6]));
            for (const auto& [why, n] : wrong) {
                std::printf("Shader prebuild:   wrong guess, %s: %llu\n", why.c_str(), static_cast<unsigned long long>(n));
            }
            std::fflush(stdout);
        }
    }
    std::unordered_map<std::string, u64> wrong;

private:
    void Run(std::stop_token token) {
        Common::SetCurrentThreadName("bbport:ShaderPrebuild");
        PrebuiltMap built;
        for (const char* name : {"bin", "common", "sp-common"}) {
            BuildPack(name, built, nullptr, token);
        }
        {
            std::unique_lock lock{maps_mutex};
            common = std::move(built);
        }
        common_bytes = 0;
        {
            std::shared_lock lock{maps_mutex};
            for (const auto& [_, entry] : common) {
                common_bytes += entry->Bytes();
            }
        }
        std::string have;
        while (!token.stop_requested()) {
            std::string want;
            {
                std::unique_lock lock{request_mutex};
                Common::CondvarWait(request_cv, lock, token, [&] { return wanted_area != have; });
                if (token.stop_requested()) {
                    return;
                }
                want = wanted_area;
            }
            PrebuiltMap next;
            BuildPack(want.c_str(), next, &common, token);
            {
                std::scoped_lock lock{request_mutex};
                if (wanted_area != want) {
                    continue; // another area meanwhile: this one is thrown away
                }
            }
            std::unique_lock lock{maps_mutex};
            area = std::move(next);
            have = want;
        }
    }

    void BuildPack(const char* name, PrebuiltMap& out, const PrebuiltMap* skip, std::stop_token token) {
        const auto start = std::chrono::steady_clock::now();
        const auto pack = PrebuildReadFile(dir / (std::string{name} + ".pack"));
        const auto shaders = ParsePack(pack);
        if (shaders.empty()) {
            std::printf("Shader prebuild: no shaders for %s\n", name);
            std::fflush(stdout);
            return;
        }
        const std::string area_name = skip ? name : "";
        std::atomic<size_t> next{0};
        std::atomic<u64> bytes{0}, failed{0};
        std::atomic<bool> stop{false};
        std::mutex out_mutex;
        u32 rest_ms = 2;
        if (const char* env = std::getenv("BB_SHADER_PREBUILD_REST"); env && *env) {
            rest_ms = u32(std::strtoul(env, nullptr, 10));
        }
        const auto worker = [&] {
            Common::SetCurrentThreadName("bbport:ShaderPrebuild");
            // BB_SHADER_PREBUILD_LOW=1: below the game's threads.
            if (const char* low = std::getenv("BB_SHADER_PREBUILD_LOW"); low && low[0] == '1') {
                Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
            }
            g_shader_prebuild_test = true;
            g_shader_fake_sharps = &GuessSharps;
            static thread_local Shader::Pools pools;
            while (!stop.load(std::memory_order_relaxed) && !token.stop_requested()) {
                const size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= shaders.size()) {
                    break;
                }
                const auto& shader = shaders[index];
                if (shader.type != 0 && shader.type != 4) {
                    continue;
                }
                if (skip && skip->contains(shader.hash)) {
                    continue;
                }
                if ((index & 15) == 0 && skip) {
                    std::scoped_lock lock{request_mutex};
                    if (wanted_area != area_name) {
                        stop.store(true);
                        break;
                    }
                }
                auto entry = BuildOne(shader, profile, pools);
                if (!entry) {
                    failed.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                const u64 size = entry->Bytes();
                if (common_bytes + bytes.fetch_add(size) + size > limit_bytes) {
                    stop.store(true);
                    break;
                }
                {
                    std::scoped_lock lock{out_mutex};
                    out.emplace(shader.hash, std::move(entry));
                }
                // The GPU thread must get its turn while this runs (a build on the Xbox froze the game for 6 s).
                if (rest_ms) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(rest_ms));
                }
            }
            g_shader_fake_sharps = nullptr;
            g_shader_prebuild_test = false;
        };
        {
            std::vector<std::jthread> pool;
            for (u32 i = 0; i < threads; ++i) {
                pool.emplace_back(worker);
            }
        }
        std::printf("Shader prebuild: %s: %zu of %zu shaders in %.1f s on %u threads, %.1f MiB, %llu failed%s\n", name,
                    out.size(), shaders.size(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), threads,
                    double(bytes.load()) / 1048576.0, static_cast<unsigned long long>(failed.load()),
                    stop.load() ? " (stopped early)" : "");
        std::fflush(stdout);
    }

    std::atomic<bool> enabled{false};
    std::filesystem::path dir;
    Shader::Profile profile{};
    u32 threads{1};
    u64 limit_bytes{};
    u64 common_bytes{};
    std::shared_mutex maps_mutex;
    PrebuiltMap common, area;
    std::mutex request_mutex;
    std::condition_variable_any request_cv;
    std::unordered_map<std::string, u64> area_bytes;
    std::string wanted_area;
    std::jthread manager;
};

} // namespace

/// The guess's specialization for a shader, read back from its entry.
static bool PrebuiltGuess(const PrebuiltEntry& entry, Shader::Info& info, Shader::StageSpecialization& spec, bool with_info) {
    std::vector<u8> meta(entry.meta_size);
    if (!Unsqueeze(entry.meta, meta.data(), meta.size())) {
        return false;
    }
    Serialization::Archive ar{std::move(meta)};
    spec.info = &info;
    spec.Deserialize(ar);
    if (with_info) {
        info.Deserialize(ar);
    }
    return true;
}

/// BB_SHADER_REPLAY_GUESS: every shader in a saved cache against the guess for it, and where the guess's
/// settings match, its SPIR-V against the saved SPIR-V.
static void ShaderGuessTest(const char* dir, const char* packs, const char* out_path) {
    namespace fs = std::filesystem;
    const auto profile_data = PrebuildReadFile(fs::path{dir} / "profile.bin");
    Shader::Profile profile{};
    if (profile_data.size() != sizeof(profile)) {
        std::printf("Guess test: no usable profile.bin\n");
        std::fflush(stdout);
        return;
    }
    std::memcpy(&profile, profile_data.data(), sizeof(profile));
    std::vector<std::vector<u8>> blobs;
    std::unordered_map<u64, PackShader> shaders;
    for (const auto& entry : fs::directory_iterator{packs}) {
        if (entry.path().extension() == ".pack") {
            blobs.push_back(PrebuildReadFile(entry.path()));
            for (const auto& row : ParsePack(blobs.back())) {
                shaders.try_emplace(row.hash, row);
            }
        }
    }
    std::vector<fs::path> metas;
    for (const auto& entry : fs::directory_iterator{dir}) {
        if (entry.path().extension() == ".meta") {
            metas.push_back(entry.path());
        }
    }
    std::atomic<size_t> next{0};
    std::mutex mutex;
    std::unordered_map<std::string, u64> counts;
    std::unordered_map<u64, u32> shader_hits;
    std::FILE* out = std::fopen(out_path, "wb");
    Shader::Pools* unused = nullptr;
    (void)unused;
    const auto worker = [&] {
        static thread_local Shader::Pools pools;
        while (true) {
            const size_t index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= metas.size()) {
                break;
            }
            auto blob = PrebuildReadFile(metas[index]);
            Serialization::Archive ar{std::move(blob)};
            Shader::Info saved{};
            Shader::StageSpecialization real{};
            real.info = &saved;
            std::optional<Shader::Gcn::FetchShaderData> fetch;
            size_t perm_idx{};
            if (!LoadShaderMeta(ar, saved, fetch, real, perm_idx)) {
                continue;
            }
            const char* stage = saved.hw_stage == Shader::HwStage::Fragment  ? "pixel"
                                : saved.hw_stage == Shader::HwStage::Compute ? "compute"
                                                                             : "vertex";
            std::string result;
            const auto it = shaders.find(saved.pgm_hash);
            if (saved.hw_stage == Shader::HwStage::Vertex) {
                result = "not guessed";
            } else if (it == shaders.end()) {
                result = "no code";
            } else {
                g_shader_prebuild_test = true;
                g_shader_fake_sharps = &GuessSharps;
                auto entry = BuildOne(it->second, profile, pools);
                g_shader_fake_sharps = nullptr;
                g_shader_prebuild_test = false;
                if (!entry) {
                    result = "build failed";
                } else {
                    Shader::Info guess_info{};
                    Shader::StageSpecialization guess{};
                    PrebuiltGuess(*entry, guess_info, guess, false);
                    const char* why = PrebuiltMismatch(real, guess, saved.pgm_hash);
                    if (why) {
                        result = fmt::format("wrong: {}", why);
                    } else {
                        std::vector<u32> spv(entry->spv_words);
                        Unsqueeze(entry->spv, spv.data(), spv.size() * 4);
                        const auto saved_bytes = PrebuildReadFile(
                            fs::path{dir} / fmt::format("{:#018x}_{}_v12.spv", saved.pgm_hash, perm_idx));
                        const bool same = saved_bytes.size() == spv.size() * 4 &&
                                          std::memcmp(saved_bytes.data(), spv.data(), saved_bytes.size()) == 0;
                        result = same ? "hit, SPIR-V same" : "HIT BUT SPIR-V DIFFERS";
                        Shader::Info full{};
                        Shader::StageSpecialization again{};
                        PrebuiltGuess(*entry, full, again, true);
                        bool info_same = full.buffers.size() == saved.buffers.size() &&
                                         full.images.size() == saved.images.size() &&
                                         full.samplers.size() == saved.samplers.size() &&
                                         full.ud_mask.mask == saved.ud_mask.mask && full.mrt_mask == saved.mrt_mask &&
                                         full.srt_info.flattened_bufsize_dw == saved.srt_info.flattened_bufsize_dw &&
                                         full.srt_info.walker_func_size == saved.srt_info.walker_func_size;
                        for (size_t i = 0; info_same && i < saved.buffers.size(); ++i) {
                            info_same = full.buffers[i].sharp_fetch == saved.buffers[i].sharp_fetch &&
                                        full.buffers[i].is_written == saved.buffers[i].is_written;
                        }
                        for (size_t i = 0; info_same && i < saved.images.size(); ++i) {
                            info_same = full.images[i].sharp_fetch == saved.images[i].sharp_fetch &&
                                        full.images[i].is_written == saved.images[i].is_written;
                        }
                        if (info_same && saved.srt_info.walker_func_size) {
                            info_same = std::memcmp(reinterpret_cast<const void*>(full.srt_info.walker_func),
                                                    reinterpret_cast<const void*>(saved.srt_info.walker_func),
                                                    saved.srt_info.walker_func_size) == 0;
                        }
                        if (!info_same) {
                            result += " BUT INFO DIFFERS";
                        }
                    }
                }
            }
            std::scoped_lock lock{mutex};
            ++counts[fmt::format("{}: {}", stage, result)];
            if (result.starts_with("hit")) {
                ++shader_hits[saved.pgm_hash];
            }
            std::fprintf(out, "%s %016llx %zu %s\n", stage, static_cast<unsigned long long>(saved.pgm_hash), perm_idx,
                         result.c_str());
        }
    };
    {
        std::vector<std::jthread> pool;
        for (u32 i = 0; i < std::max(std::thread::hardware_concurrency(), 1u); ++i) {
            pool.emplace_back(worker);
        }
    }
    std::fclose(out);
    for (const auto& [what, n] : counts) {
        std::printf("Guess test: %s: %llu\n", what.c_str(), static_cast<unsigned long long>(n));
    }
    std::printf("Guess test: done, %zu shaders with a hit\n", shader_hits.size());
    std::fflush(stdout);
    std::_Exit(0);
}
