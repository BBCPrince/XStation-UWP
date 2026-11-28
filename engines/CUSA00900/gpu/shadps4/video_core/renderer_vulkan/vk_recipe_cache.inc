
namespace {

std::vector<u8> RecipeReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    std::vector<u8> data;
    if (file) {
        data.resize(size_t(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    }
    return data;
}

bool RecipeWriteFile(const std::filesystem::path& path, const void* data, size_t size) {
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file.write(static_cast<const char*>(data), std::streamsize(size));
        if (!file) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    return !error;
}

struct RecipeFile {
    std::string name;
    std::vector<u8> data;
};

bool RecipeParse(const std::vector<u8>& raw, std::vector<std::string>& sources, std::vector<RecipeFile>& files) {
    if (raw.size() < 12 || std::memcmp(raw.data(), "BBRCP1\0\0", 8) != 0) {
        return false;
    }
    u32 body_size;
    std::memcpy(&body_size, raw.data() + 8, 4);
    std::vector<u8> body(body_size);
    mz_ulong out = body_size;
    if (mz_uncompress(body.data(), &out, raw.data() + 12, mz_ulong(raw.size() - 12)) != MZ_OK || out != body_size) {
        return false;
    }
    size_t at = 0;
    const auto take = [&](void* dst, size_t n) {
        if (at + n > body.size()) {
            return false;
        }
        std::memcpy(dst, body.data() + at, n);
        at += n;
        return true;
    };
    u32 count{};
    if (!take(&count, 4)) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        u16 len{};
        if (!take(&len, 2) || at + len > body.size()) {
            return false;
        }
        sources.emplace_back(reinterpret_cast<const char*>(body.data() + at), len);
        at += len;
    }
    if (!take(&count, 4)) {
        return false;
    }
    files.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u16 len{};
        u32 size{};
        if (!take(&len, 2) || at + len > body.size()) {
            return false;
        }
        RecipeFile file;
        file.name.assign(reinterpret_cast<const char*>(body.data() + at), len);
        at += len;
        if (!take(&size, 4) || at + size > body.size()) {
            return false;
        }
        file.data.assign(body.begin() + at, body.begin() + at + size);
        at += size;
        files.push_back(std::move(file));
    }
    return true;
}

/// A program's code, and what follows it up to its footer: constants the code reads relative to its own
/// address sit there (a copy of the code alone made some shaders read past it).
/// Bytes of the game file kept after a program's footer (BB_SHADER_RECIPE_TAIL, a test switch).
size_t RecipeTail() {
    static const size_t tail = [] {
        const char* env = std::getenv("BB_SHADER_RECIPE_TAIL");
        return env && *env ? size_t(std::strtoull(env, nullptr, 10)) : size_t(0);
    }();
    return tail;
}

struct RecipeProgram {
    std::vector<u32> words;
    size_t code_words{};
};

/// The code of every wanted program, from the game's files (PS4 shader binaries end in an "OrbShdr" footer:
/// code length and stage in its third word, the program hash after it; the code ends with s_endpgm).
std::unordered_map<u64, RecipeProgram> RecipeCode(const std::filesystem::path& app0,
                                                     const std::vector<std::string>& sources,
                                                     const std::unordered_set<u64>& wanted) {
    std::unordered_map<u64, RecipeProgram> code;
    for (const auto& source : sources) {
        const auto space = source.find(' ');
        if (space == std::string::npos) {
            continue;
        }
        const std::string kind = source.substr(0, space);
        auto data = RecipeReadFile(app0 / source.substr(space + 1));
        if (data.empty()) {
            std::printf("Shader recipe: %s not found in the game folder\n", source.c_str() + space + 1);
            continue;
        }
        if (kind == "dcx") {
            if (data.size() < 0x4c || std::memcmp(data.data(), "DCX\0", 4) != 0 ||
                std::memcmp(data.data() + 0x28, "DFLT", 4) != 0) {
                continue;
            }
            const auto be32 = [&](size_t o) {
                return u32(data[o]) << 24 | u32(data[o + 1]) << 16 | u32(data[o + 2]) << 8 | u32(data[o + 3]);
            };
            const u32 usize = be32(0x1c), csize = be32(0x20);
            std::vector<u8> unpacked(usize);
            mz_ulong out = usize;
            if (0x4c + size_t(csize) > data.size() ||
                mz_uncompress(unpacked.data(), &out, data.data() + 0x4c, csize) != MZ_OK || out != usize) {
                continue;
            }
            data = std::move(unpacked);
        }
        static constexpr u8 sig[7]{'O', 'r', 'b', 'S', 'h', 'd', 'r'};
        static constexpr u8 endpgm[4]{0x00, 0x00, 0x81, 0xbf};
        for (auto it = std::search(data.begin(), data.end(), std::begin(sig), std::end(sig)); it != data.end();
             it = std::search(it + 7, data.end(), std::begin(sig), std::end(sig))) {
            const size_t pos = size_t(it - data.begin());
            if (pos + 32 > data.size()) {
                break;
            }
            u32 bits;
            u64 hash;
            std::memcpy(&bits, data.data() + pos + 8, 4);
            std::memcpy(&hash, data.data() + pos + 16, 8);
            const u32 length = bits >> 8;
            if (!wanted.contains(hash) || code.contains(hash) || length % 4 || length > pos) {
                continue;
            }
            const size_t low = pos > 0x8000 ? pos - 0x8000 : 0;
            size_t end = std::string::npos;
            for (size_t k = pos; k-- > low;) {
                if (k + 4 <= pos && std::memcmp(data.data() + k, endpgm, 4) == 0) {
                    end = k;
                    break;
                }
            }
            if (end == std::string::npos || end + 4 < length) {
                continue;
            }
            const size_t start = end + 4 - length;
            RecipeProgram program;
            program.code_words = length / 4;
            const size_t stop = std::min(data.size(), pos + 32 + RecipeTail());
            program.words.assign((stop - start + 3) / 4 + 64, 0);
            std::memcpy(program.words.data(), data.data() + start, stop - start);
            code.emplace(hash, std::move(program));
        }
    }
    return code;
}

/// The stage's V# table for a fetch shader (TLOU's ReplayFetchTable): the recorded table pointer is guest
/// memory of another session.
void RecipeFetchTable(const Shader::StageSpecialization& spec, std::array<u32, 16>& user_data,
                      std::vector<u32>& table) {
    if (!spec.fetch_shader_data) {
        return;
    }
    table.assign(4096, 0);
    const auto& attributes = spec.fetch_shader_data->attributes;
    for (size_t i = 0; i < attributes.size() && i < spec.vs_attribs.size(); ++i) {
        const auto& attrib = attributes[i];
        const auto& wanted = spec.vs_attribs[i];
        AmdGpu::Buffer sharp{};
        sharp.base_address = 0x1000;
        sharp.stride = 16;
        sharp.num_records = 1;
        sharp.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
        sharp.num_format = wanted.num_class == AmdGpu::NumberClass::Sint   ? u32(AmdGpu::NumberFormat::Sint)
                           : wanted.num_class == AmdGpu::NumberClass::Uint ? u32(AmdGpu::NumberFormat::Uint)
                                                                           : u32(AmdGpu::NumberFormat::Float);
        sharp.dst_sel_x = u32(wanted.dst_select.r);
        sharp.dst_sel_y = u32(wanted.dst_select.g);
        sharp.dst_sel_z = u32(wanted.dst_select.b);
        sharp.dst_sel_w = u32(wanted.dst_select.a);
        std::memcpy(&table[attrib.dword_offset], &sharp, sizeof(sharp));
        const u64 pointer = reinterpret_cast<u64>(table.data());
        std::memcpy(&user_data[attrib.sgpr_base], &pointer, sizeof(pointer));
    }
}

/// One translation, kept apart from callers: sigsetjmp must not cross objects with destructors.
void RecipeTranslate(const Shader::Info& saved, const Shader::StageSpecialization& spec, std::span<const u32> code,
                     std::array<u32, 16>& user_data, const Shader::Profile& profile, Shader::Pools& pools,
                     std::vector<u32>& spv, const char*& why) {
    try {
        // The recorded settings' buffer strides into the descriptors (2026-11-16): a later draw of the program can
        // have refreshed the saved flat buffer, and a vertex shader's stride is in its code (116 of 1,477 TLOU
        // vertex variants came out with another stride without this).
        std::vector<u32> flat = saved.flattened_ud_buf;
        for (size_t i = 0; i < saved.buffers.size() && i < spec.buffers.size(); ++i) {
            const auto& fetch = saved.buffers[i].sharp_fetch;
            if (!saved.buffers[i].IsSpecial() && ((fetch.load_mask >> 1) & 1) &&
                fetch.offsets[1] != Shader::UNKNOWN_LOCATION && fetch.offsets[1] < flat.size()) {
                u32& word = flat[fetch.offsets[1]];
                word = (word & ~(0x3fffu << 16)) | (u32(spec.buffers[i].stride) << 16);
            }
        }
        const Shader::ShaderParams params{std::span<const u32, Shader::ShaderParams::NumShaderUserData>(user_data),
                                          code, saved.pgm_hash};
        Shader::Info info(saved.hw_stage, saved.sw_stage, params);
        Shader::RuntimeInfo runtime = spec.runtime_info;
        Shader::Backend::Bindings binding = spec.start;
        g_shader_fake_flat = &flat;
        g_shader_fake_fetch = spec.fetch_shader_data ? &*spec.fetch_shader_data : nullptr;
        const auto program = Shader::TranslateProgram(code, pools, info, runtime, profile);
        spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding);
        if (info.buffers.size() != saved.buffers.size() || info.images.size() != saved.images.size() ||
            info.samplers.size() != saved.samplers.size() ||
            info.srt_info.flattened_bufsize_dw != saved.srt_info.flattened_bufsize_dw) {
            spv.clear(); // not the shader the recipe recorded
            why = "other resources";
            return;
        }
        // Only a translation whose settings are the recorded ones is kept: a draw finds it by these settings.
        if (!(Shader::StageSpecialization(info, spec.runtime_info, profile, spec.start) == spec)) {
            spv.clear();
            why = "other settings";
        }
    } catch (const std::exception&) {
        spv.clear();
        why = "assert";
    }
}

struct RecipeResult {
    u64 written{}, keys{}, failed{}, no_code{}, unreadable{};
    std::vector<std::string> failures; ///< "<meta> <why>"
};

/// Rebuilds the cache files of `recipe` into `dir` (translated with `profile`). Returns what it wrote.
RecipeResult RecipeExpand(const std::filesystem::path& recipe_path, const std::filesystem::path& app0,
                          const std::filesystem::path& dir, const Shader::Profile& profile, u32 threads) {
    RecipeResult result;
    std::vector<std::string> sources;
    std::vector<RecipeFile> files;
    if (!RecipeParse(RecipeReadFile(recipe_path), sources, files)) {
        std::printf("Shader recipe: %s is not a cache recipe\n", recipe_path.string().c_str());
        std::fflush(stdout);
        return result;
    }
    std::error_code ignored;
    std::filesystem::create_directories(dir, ignored);
    // The programs the recipe's shaders need.
    std::vector<size_t> metas;
    std::unordered_set<u64> wanted;
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].name.ends_with(".meta") && files[i].data.size() > 24 + sizeof(Shader::InfoPersistent)) {
            metas.push_back(i);
        }
    }
    {
        for (const size_t i : metas) {
            Serialization::Archive ar{std::vector<u8>(files[i].data)};
            Shader::Info saved{};
            Shader::StageSpecialization spec{};
            spec.info = &saved;
            std::optional<Shader::Gcn::FetchShaderData> fetch;
            size_t perm_idx{};
            if (LoadShaderMeta(ar, saved, fetch, spec, perm_idx)) {
                wanted.insert(saved.pgm_hash);
            }
        }
    }
    const auto code = RecipeCode(app0, sources, wanted);
    std::atomic<size_t> next{0};
    std::atomic<u64> written{0}, failed{0}, no_code{0}, unreadable{0};
    std::mutex failures_mutex;
    const auto fail = [&](const std::string& name, const char* why) {
        std::scoped_lock lock{failures_mutex};
        result.failures.push_back(name + " " + why);
    };
    const auto worker = [&] {
        Common::SetCurrentThreadName("bbport:ShaderRecipe");
        g_shader_prebuild_test = true;
        static thread_local Shader::Pools pools;
        while (true) {
            const size_t n = next.fetch_add(1, std::memory_order_relaxed);
            if (n >= metas.size()) {
                break;
            }
            const auto& file = files[metas[n]];
            Serialization::Archive ar{std::vector<u8>(file.data)};
            Shader::Info saved{};
            Shader::StageSpecialization spec{};
            spec.info = &saved;
            std::optional<Shader::Gcn::FetchShaderData> fetch;
            size_t perm_idx{};
            if (!LoadShaderMeta(ar, saved, fetch, spec, perm_idx)) {
                unreadable.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const auto it = code.find(saved.pgm_hash);
            if (it == code.end()) {
                no_code.fetch_add(1, std::memory_order_relaxed);
                fail(file.name, "not in the game files");
                continue;
            }
            std::array<u32, 16> user_data{};
            std::copy_n(saved.flattened_ud_buf.begin(), std::min<size_t>(16, saved.flattened_ud_buf.size()),
                        user_data.begin());
            std::vector<u32> table;
            RecipeFetchTable(spec, user_data, table);
            std::vector<u32> spv;
            const char* why = "translation failed";
            bool faulted = false;
            sigjmp_buf recover;
            if (sigsetjmp(recover, 0)) {
                runtime_fault_recover = nullptr;
                spv.clear();
                faulted = true;
            } else {
                runtime_fault_recover = &recover;
                RecipeTranslate(saved, spec, std::span<const u32>{it->second.words.data(), it->second.code_words},
                                user_data, profile, pools, spv, why);
                runtime_fault_recover = nullptr;
            }
            g_shader_fake_flat = nullptr;
            g_shader_fake_fetch = nullptr;
            if (spv.empty()) {
                failed.fetch_add(1, std::memory_order_relaxed);
                fail(file.name, faulted ? "faulted" : why);
                continue;
            }
            // The SPIR-V first: a meta without its binary would be loaded and then miss it.
            if (RecipeWriteFile(dir / fmt::format("{:#018x}_{}.spv", saved.pgm_hash, perm_idx), spv.data(),
                                spv.size() * 4) &&
                RecipeWriteFile(dir / file.name, file.data.data(), file.data.size())) {
                written.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_shader_prebuild_test = false;
    };
    {
        std::vector<std::jthread> pool;
        for (u32 i = 0; i < std::max(threads, 1u); ++i) {
            pool.emplace_back(worker);
        }
    }
    for (const auto& file : files) {
        if (file.name.ends_with(".key") && RecipeWriteFile(dir / file.name, file.data.data(), file.data.size())) {
            ++result.keys;
        }
    }
    result.written = written.load();
    result.failed = failed.load();
    result.no_code = no_code.load();
    result.unreadable = unreadable.load();
    return result;
}

} // namespace

/// Cold start with a recipe: the cache folder gets the recipe's shaders and pipelines, and this machine's
/// profile and driver id, so the warm-up that follows loads them all.
static void RecipeColdStart(const Instance& instance, const Shader::Profile& profile) {
    if (!EmulatorSettings.IsPipelineCacheEnabled() || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    const char* recipe = std::getenv("BB_SHADER_RECIPE");
    const char* app0 = std::getenv("BB_APP0_DIR");
    if (!recipe || !*recipe || (recipe[0] == '0' && recipe[1] == 0) || !app0 || !*app0) {
        return;
    }
    using namespace Common::FS;
    auto dir = GetUserPath(PathType::CacheDir) / std::string{Common::ElfInfo::Instance().GameSerial()};
    if (const char* name = std::getenv("BB_PIPELINE_CACHE_NAMESPACE"); name && *name) {
        const std::string scope{name};
        const bool valid = scope.size() <= 64 && std::ranges::all_of(scope, [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_';
        });
        if (valid) {
            dir /= scope;
        }
    }
    if (std::filesystem::exists(dir / "profile.bin")) {
        return; // not a first start: the cache is this machine's own
    }
    const auto start = std::chrono::steady_clock::now();
    u32 threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    if (const char* env = std::getenv("BB_SHADER_RECIPE_THREADS"); env && *env) {
        threads = std::max<u32>(u32(std::strtoul(env, nullptr, 10)), 1u);
    }
    std::printf("Shader recipe: first start, rebuilding the pipeline cache from %s\n", recipe);
    std::fflush(stdout);
    const auto r = RecipeExpand(recipe, app0, dir, profile, threads);
    if (r.written) {
        RecipeWriteFile(dir / "profile.bin", &profile, sizeof(profile));
        const auto uuid = instance.GetPipelineCacheUUID();
        RecipeWriteFile(dir / "driver_uuid.bin", uuid.data(), uuid.size());
    }
    std::printf("Shader recipe: %llu shaders and %llu pipeline keys in %.1f s on %u threads; %llu failed, %llu not "
                "in the game files, %llu unreadable\n",
                static_cast<unsigned long long>(r.written), static_cast<unsigned long long>(r.keys),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), threads,
                static_cast<unsigned long long>(r.failed), static_cast<unsigned long long>(r.no_code),
                static_cast<unsigned long long>(r.unreadable));
    std::fflush(stdout);
}

extern "C" void bb_longjmp(void* buffer, int value);
/// PC test: no runtime is running, so a fault while translating is sent back to its recovery point here, as the
/// runtime's handler does in the game (probe.c).
static long __stdcall RecipeTestFault(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && runtime_fault_recover) {
        unsigned long long* buffer = *runtime_fault_recover;
        runtime_fault_recover = nullptr;
        CONTEXT* c = info->ContextRecord;
        c->Rsp = ((c->Rsp - 128) & ~DWORD64(15)) - 40;
        c->Rcx = DWORD64(uintptr_t(buffer));
        c->Rdx = 1;
        c->Rip = DWORD64(uintptr_t(&bb_longjmp));
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/// PC test (tools/bb_recipe_test.py): the rebuild into `out_dir` with a saved cache's profile, no game. Exits.
extern "C" __declspec(dllexport) void bbgpu_recipe_cache_test(const char* recipe, const char* app0,
                                                             const char* out_dir, const char* profile_bin) {
    const auto data = RecipeReadFile(profile_bin);
    Shader::Profile profile{};
    if (data.size() != sizeof(profile)) {
        std::printf("Shader recipe test: profile %s has %zu bytes, this build expects %zu\n", profile_bin,
                    data.size(), sizeof(profile));
        std::fflush(stdout);
        std::_Exit(2);
    }
    std::memcpy(&profile, data.data(), sizeof(profile));
    AddVectoredExceptionHandler(1, RecipeTestFault);
    const auto start = std::chrono::steady_clock::now();
    const auto r = RecipeExpand(recipe, app0, out_dir, profile, std::max(std::thread::hardware_concurrency(), 1u));
    std::printf("Shader recipe test: %llu shaders, %llu keys in %.1f s; %llu failed, %llu not in the game files, "
                "%llu unreadable\n",
                static_cast<unsigned long long>(r.written), static_cast<unsigned long long>(r.keys),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                static_cast<unsigned long long>(r.failed), static_cast<unsigned long long>(r.no_code),
                static_cast<unsigned long long>(r.unreadable));
    for (const auto& line : r.failures) {
        std::printf("Shader recipe test: %s\n", line.c_str());
    }
    std::fflush(stdout);
    std::_Exit(0);
}
