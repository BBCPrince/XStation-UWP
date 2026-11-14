// TLOU test (BB_SHADER_REPLAY=<cache folder>, BB_SHADER_PACK=<pack>, 2026-11-09). Translates every shader
// variant in a saved cache again, with nothing from a running game: the descriptors come from the saved
// metadata, the code from the game files. If the SPIR-V comes out the same as the saved one, a variant can be
// built ahead of time. Writes one JSON line per variant and per pipeline to BB_SHADER_REPLAY_OUT, then exits.

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx);

static std::vector<u8> ReplayReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    std::vector<u8> data;
    if (file) {
        data.resize(size_t(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    }
    return data;
}

template <typename T>
static std::string ReplayWords(const T& value) {
    std::array<u32, sizeof(T) / sizeof(u32)> words;
    std::memcpy(words.data(), &value, sizeof(words));
    std::string out = "[";
    for (size_t i = 0; i < words.size(); ++i) {
        out += fmt::format("{}{}", i ? "," : "", words[i]);
    }
    return out + "]";
}

static std::string ReplayDescribe(const Shader::Info& info, const Shader::StageSpecialization& spec) {
    const auto& rt = spec.runtime_info;
    std::string s = fmt::format(
        "\"h\":\"{:016x}\",\"hw\":{},\"sw\":{},\"flat\":{},\"walker\":{},\"ud_mask\":{},\"mrt\":{},\"dma\":{},"
        "\"props\":[{},{},{},{},{},{},{}],\"start\":[{},{},{}],\"bits\":\"{}\"",
        info.pgm_hash, u32(info.hw_stage), u32(info.sw_stage), info.flattened_ud_buf.size(),
        info.srt_info.walker_func_size, info.ud_mask.mask, info.mrt_mask, info.uses_dma,
        u32(rt.props.fp_denorm_mode32), u32(rt.props.fp_denorm_mode16_64), u32(rt.props.fp_round_mode32),
        u32(rt.props.fp_round_mode16_64), rt.props.num_user_data, rt.props.num_input_vgprs,
        rt.props.num_allocated_vgprs, spec.start.unified, spec.start.buffer, spec.start.user_data,
        spec.bitset.to_string());
    if (rt.hw_stage == Shader::HwStage::Fragment) {
        const auto& fs = rt.hw.fs;
        s += fmt::format(",\"fs\":{{\"ena\":{},\"addr\":{},\"n\":{},\"zfmt\":{},\"samples\":{},\"mrtz\":{},\"ff\":{},"
                         "\"dual\":{},\"clip\":{},\"motion\":{},\"inputs\":[",
                         ReplayWords(fs.en_flags), ReplayWords(fs.addr_flags), fs.num_inputs, u32(fs.z_export_format), fs.num_samples,
                         fs.mrtz_mask, fs.front_face_all_bits, fs.dual_source_blending, fs.clip_distance_emulation,
                         fs.motion_vectors);
        for (u32 i = 0; i < fs.num_inputs; ++i) {
            s += fmt::format("{}[{},{},{},{}]", i ? "," : "", fs.inputs[i].param_index, fs.inputs[i].is_default,
                             fs.inputs[i].is_flat, fs.inputs[i].default_value);
        }
        s += "],\"cb\":[";
        for (u32 i = 0; i < Shader::MaxColorBuffers; ++i) {
            const auto& cb = fs.color_buffers[i];
            s += fmt::format("{}[{},{},{},{},{},{},{},{},{}]", i ? "," : "", u32(cb.data_format), u32(cb.num_format),
                             u32(cb.num_conversion), u32(cb.export_format), u32(cb.blend_self_scale),
                             u32(cb.swizzle.r), u32(cb.swizzle.g), u32(cb.swizzle.b), u32(cb.swizzle.a));
        }
        s += "]}";
    } else if (rt.hw_stage == Shader::HwStage::Vertex) {
        const auto& vs = rt.hw.vs;
        s += fmt::format(",\"vs\":{{\"n\":{},\"out\":\"", vs.num_outputs);
        for (const auto& map : vs.outputs) {
            for (const auto output : map) {
                s += fmt::format("{:02x}", u32(output));
            }
        }
        s += fmt::format("\",\"depth\":{},\"clipoff\":{},\"ucp\":{},\"motion\":{},\"step\":[{},{}],\"sgpr\":[{},{}],\"tess\":{}}}",
                         vs.emulate_depth_negative_one_to_one, vs.clip_disable, vs.user_clip_plane_mask,
                         vs.motion_vectors, rt.sw.vs.step_rate_0, rt.sw.vs.step_rate_1,
                         rt.sw.vs.vertex_sgpr_offset, rt.sw.vs.instance_sgpr_offset,
                         rt.sw.vs.tess_emulated_primitive);
    } else if (rt.hw_stage == Shader::HwStage::Compute) {
        const auto& cs = rt.hw.cs;
        s += fmt::format(",\"cs\":{{\"shared\":{},\"wg\":[{},{},{}],\"tgid\":[{},{},{}],\"batched\":{}}}",
                         cs.shared_memory_size, cs.workgroup_size[0], cs.workgroup_size[1], cs.workgroup_size[2],
                         cs.tgid_enable[0], cs.tgid_enable[1], cs.tgid_enable[2], cs.batched);
    }
    s += ",\"fetch\":";
    if (spec.fetch_shader_data) {
        s += fmt::format("{{\"vo\":{},\"io\":{},\"a\":[", spec.fetch_shader_data->vertex_offset_sgpr,
                         spec.fetch_shader_data->instance_offset_sgpr);
        for (size_t i = 0; i < spec.fetch_shader_data->attributes.size(); ++i) {
            const auto& a = spec.fetch_shader_data->attributes[i];
            s += fmt::format("{}[{},{},{},{},{},{},{},{},{}]", i ? "," : "", a.semantic, a.dest_vgpr, a.num_elements,
                             a.sgpr_base, a.dword_offset, a.instance_data, a.inst_offset, a.data_format, a.num_format);
        }
        s += "]}";
    } else {
        s += "null";
    }
    s += ",\"va\":[";
    for (size_t i = 0; i < spec.vs_attribs.size(); ++i) {
        const auto& a = spec.vs_attribs[i];
        s += fmt::format("{}[{},{},{},{},{},{}]", i ? "," : "", a.divisor, u32(a.num_class), u32(a.dst_select.r),
                         u32(a.dst_select.g), u32(a.dst_select.b), u32(a.dst_select.a));
    }
    s += "],\"buf\":[";
    for (size_t i = 0; i < info.buffers.size(); ++i) {
        const auto& b = info.buffers[i];
        s += fmt::format("{}[{},{},{},{},{},{}]", i ? "," : "", u32(b.buffer_type), b.is_written, b.is_formatted,
                         u32(b.used_types), u32(b.post_op), ReplayWords(b.GetSharp(info)));
    }
    s += "],\"img\":[";
    for (size_t i = 0; i < info.images.size(); ++i) {
        const auto& m = info.images[i];
        s += fmt::format("{}[{},{},{},{},{},{},{},{}]", i ? "," : "", m.is_depth, m.is_atomic, m.is_array, m.is_written,
                         m.is_r128, m.needs_native, u32(m.mip_fallback_mode), ReplayWords(m.GetSharp(info)));
    }
    s += "],\"smp\":[";
    for (size_t i = 0; i < info.samplers.size(); ++i) {
        const auto& m = info.samplers[i];
        s += fmt::format("{}[{},{},{}]", i ? "," : "", m.is_depth, u32(m.post_op), ReplayWords(m.GetSharp(info)));
    }
    s += fmt::format("],\"fm\":{}", info.fmasks.size());
    return s;
}

/// The stage's V# table for a fetch shader: real table pointers are guest memory of another machine.
static void ReplayFetchTable(const Shader::Info& info, const Shader::StageSpecialization& spec,
                             std::array<u32, 16>& user_data, std::vector<u32>& table) {
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

/// BB_SHADER_REPLAY_FLIP: change one descriptor field to see whether the SPIR-V depends on it at all.
static std::vector<u32> ReplayFlip(const Shader::Info& info, const std::vector<u32>& saved_flat) {
    static const std::string mode = [] {
        const char* env = std::getenv("BB_SHADER_REPLAY_FLIP");
        return std::string{env ? env : ""};
    }();
    std::vector<u32> flat = saved_flat;
    if (mode.empty()) {
        return flat;
    }
    const auto dword = [&flat](const auto& fetch, u32 index) -> u32* {
        if (!((fetch.load_mask >> index) & 1) || fetch.offsets[index] == Shader::UNKNOWN_LOCATION ||
            fetch.offsets[index] >= flat.size()) {
            return nullptr;
        }
        return &flat[fetch.offsets[index]];
    };
    for (const auto& image : info.images) {
        const auto& fetch = image.sharp_fetch;
        if (mode == "srgb") {
            if (u32* d = dword(fetch, 1)) {
                const u32 nf = (*d >> 26) & 15;
                if (nf == 9 || nf == 0) {
                    *d = (*d & ~(15u << 26)) | ((nf == 9 ? 0u : 9u) << 26);
                }
            }
        } else if (mode == "onemip") {
            if (u32* d = dword(fetch, 3)) {
                const u32 base = (*d >> 12) & 15, last = (*d >> 16) & 15;
                *d = (*d & ~(15u << 16)) | ((base == 0 && last == 0 ? 5u : base) << 16);
            }
        } else if (mode == "imgdst") {
            if (u32* d = dword(fetch, 3)) {
                *d = (*d & ~0xfffu) | 0xfac; // bgra against rgba
            }
        } else if (mode == "imgfmt") {
            if (u32* d = dword(fetch, 1)) {
                const u32 data = (*d >> 20) & 63;
                if (data == 35 || data == 10) {
                    *d = (*d & ~(63u << 20)) | ((data == 35 ? 10u : 35u) << 20);
                }
            }
        }
    }
    for (const auto& buffer : info.buffers) {
        const auto& fetch = buffer.sharp_fetch;
        if (buffer.IsSpecial()) {
            continue;
        }
        if (mode == "stride") {
            if (u32* d = dword(fetch, 1)) {
                const u32 stride = (*d >> 16) & 0x3fff;
                if (stride) {
                    *d = (*d & ~(0x3fffu << 16)) | ((stride + 4) << 16);
                }
            }
        } else if (mode == "records") {
            if (u32* d = dword(fetch, 2)) {
                if (*d) {
                    *d += 1;
                }
            }
        }
    }
    return flat;
}

static void ShaderReplayTest(const char* dir, const char* pack_path, const char* out_path) {
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto pack = ReplayReadFile(pack_path);
    std::unordered_map<u64, std::span<const u32>> code_of;
    if (pack.size() < 12 || std::memcmp(pack.data(), "GCNPACK1", 8) != 0) {
        std::printf("Shader replay: %s is not a shader pack\n", pack_path);
        std::fflush(stdout);
        return;
    }
    u32 count;
    std::memcpy(&count, pack.data() + 8, 4);
    size_t at = 12;
    for (u32 i = 0; i < count && at + 16 <= pack.size(); ++i) {
        u64 hash;
        u32 type, size, bits;
        std::memcpy(&hash, pack.data() + at, 8);
        std::memcpy(&type, pack.data() + at + 8, 4);
        std::memcpy(&size, pack.data() + at + 12, 4);
        at += 16;
        std::memcpy(&bits, pack.data() + at + size - 32 + 8, 4);
        code_of[hash] = std::span{reinterpret_cast<const u32*>(pack.data() + at), (bits >> 8) / sizeof(u32)};
        at += size;
    }

    // The saved cache's own settings, not this machine's: what the Xbox translated with.
    Shader::Profile profile{};
    const auto profile_data = ReplayReadFile(fs::path{dir} / "profile.bin");
    if (profile_data.size() != sizeof(profile)) {
        std::printf("Shader replay: profile.bin has %zu bytes, this build expects %zu\n", profile_data.size(),
                    sizeof(profile));
        std::fflush(stdout);
        return;
    }
    std::memcpy(&profile, profile_data.data(), sizeof(profile));

    std::vector<fs::path> metas, keys;
    for (const auto& entry : fs::directory_iterator{dir}) {
        const auto extension = entry.path().extension();
        if (extension == ".meta") {
            metas.push_back(entry.path());
        } else if (extension == ".key") {
            keys.push_back(entry.path());
        }
    }
    std::FILE* out = std::fopen(out_path, "wb");
    if (!out) {
        std::printf("Shader replay: cannot write %s\n", out_path);
        std::fflush(stdout);
        return;
    }
    std::mutex out_mutex;
    std::atomic<size_t> next{0};
    std::atomic<u64> same{0}, differ{0}, failed{0}, skipped{0}, no_code{0};

    // Kept apart from the loop: sigsetjmp must not cross objects with destructors.
    const auto translate = [&profile](const Shader::Info& saved, const Shader::StageSpecialization& spec,
                                      std::span<const u32> code, const std::vector<u32>& flat,
                                      std::array<u32, 16>& user_data, Shader::Pools& pools, std::vector<u32>& spv,
                                      std::string& note) {
        try {
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
                note = fmt::format("resources {}/{} {}/{} {}/{} flat {}/{}", info.buffers.size(), saved.buffers.size(),
                                   info.images.size(), saved.images.size(), info.samplers.size(), saved.samplers.size(),
                                   info.srt_info.flattened_bufsize_dw, saved.srt_info.flattened_bufsize_dw);
            }
        } catch (const std::exception& e) {
            note = fmt::format("assert: {}", e.what());
        }
    };

    const auto worker = [&] {
        Common::SetCurrentThreadName("bbport:ShaderReplay");
        g_shader_prebuild_test = true;
        static thread_local Shader::Pools pools;
        while (true) {
            const size_t index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= metas.size()) {
                break;
            }
            auto blob = ReplayReadFile(metas[index]);
            const size_t blob_size = blob.size();
            Serialization::Archive ar{std::move(blob)};
            Shader::Info saved{};
            Shader::StageSpecialization spec{};
            spec.info = &saved;
            std::optional<Shader::Gcn::FetchShaderData> fetch;
            size_t perm_idx{};
            std::string json = fmt::format("{{\"file\":\"{}\",\"size\":{}", metas[index].stem().string(), blob_size);
            if (!LoadShaderMeta(ar, saved, fetch, spec, perm_idx)) {
                skipped.fetch_add(1, std::memory_order_relaxed);
                json += ",\"result\":\"not loadable\"}\n";
                std::scoped_lock lock{out_mutex};
                std::fwrite(json.data(), 1, json.size(), out);
                continue;
            }
            json += fmt::format(",\"p\":{},", perm_idx) + ReplayDescribe(saved, spec);
            const auto spv_data = ReplayReadFile(fs::path{dir} / fmt::format("{:#018x}_{}_v12.spv", saved.pgm_hash, perm_idx));
            std::vector<u32> saved_spv(spv_data.size() / 4);
            std::memcpy(saved_spv.data(), spv_data.data(), saved_spv.size() * 4);
            const auto it = code_of.find(saved.pgm_hash);
            std::string result, note;
            std::vector<u32> spv;
            if (it == code_of.end()) {
                no_code.fetch_add(1, std::memory_order_relaxed);
                result = "no code";
            } else {
                std::array<u32, 16> user_data{};
                std::copy_n(saved.flattened_ud_buf.begin(), std::min<size_t>(16, saved.flattened_ud_buf.size()),
                            user_data.begin());
                std::vector<u32> table;
                ReplayFetchTable(saved, spec, user_data, table);
                const std::vector<u32> flipped = ReplayFlip(saved, saved.flattened_ud_buf);
                sigjmp_buf recover;
                if (sigsetjmp(recover, 0)) {
                    runtime_fault_recover = nullptr;
                    failed.fetch_add(1, std::memory_order_relaxed);
                    result = "fault";
                } else {
                    runtime_fault_recover = &recover;
                    translate(saved, spec, it->second, flipped, user_data, pools, spv, note);
                    runtime_fault_recover = nullptr;
                    if (spv.empty()) {
                        failed.fetch_add(1, std::memory_order_relaxed);
                        result = "failed";
                    } else if (spv == saved_spv) {
                        same.fetch_add(1, std::memory_order_relaxed);
                        result = "same";
                    } else {
                        differ.fetch_add(1, std::memory_order_relaxed);
                        result = "differ";
                        const auto bad = fs::path{out_path}.parent_path() / "differ";
                        std::error_code ignored;
                        fs::create_directories(bad, ignored);
                        std::ofstream(bad / fmt::format("{:016x}_{}.new.spv", saved.pgm_hash, perm_idx), std::ios::binary)
                            .write(reinterpret_cast<const char*>(spv.data()), std::streamsize(spv.size() * 4));
                    }
                }
                g_shader_fake_flat = nullptr;
                g_shader_fake_fetch = nullptr;
            }
            json += fmt::format(",\"ph\":\"{}\",\"spv\":{},\"new\":{},\"result\":\"{}\",\"note\":\"{}\"}}\n",
                                metas[index].stem().string(), saved_spv.size(), spv.size(), result, note);
            std::scoped_lock lock{out_mutex};
            std::fwrite(json.data(), 1, json.size(), out);
        }
        g_shader_prebuild_test = false;
    };
    {
        u32 threads = std::thread::hardware_concurrency();
        if (const char* env = std::getenv("BB_SHADER_PACK_THREADS"); env && *env) {
            threads = u32(std::strtoul(env, nullptr, 10));
        }
        std::vector<std::jthread> pool;
        for (u32 i = 0; i < std::max(threads, 1u); ++i) {
            pool.emplace_back(worker);
        }
    }
    for (const auto& path : keys) {
        const auto blob = ReplayReadFile(path);
        std::string hex;
        hex.reserve(blob.size() * 2);
        for (const u8 byte : blob) {
            hex += fmt::format("{:02x}", byte);
        }
        std::fprintf(out, "{\"key\":\"%s\",\"hex\":\"%s\"}\n", path.stem().string().c_str(), hex.c_str());
    }
    std::fclose(out);
    std::printf("Shader replay: %zu variants, %zu pipelines in %.1f s: %llu same, %llu differ, %llu failed, "
                "%llu not loadable, %llu not in the game files\n",
                metas.size(), keys.size(), std::chrono::duration<double>(Clock::now() - start).count(),
                static_cast<unsigned long long>(same.load()), static_cast<unsigned long long>(differ.load()),
                static_cast<unsigned long long>(failed.load()), static_cast<unsigned long long>(skipped.load()),
                static_cast<unsigned long long>(no_code.load()));
    std::fflush(stdout);
    std::_Exit(0);
}
