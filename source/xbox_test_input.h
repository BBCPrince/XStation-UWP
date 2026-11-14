#pragma once
#include <array>
#include <cstdint>
#include <sstream>
#include <string>

// Explicit, bounded developer test input. Absent file means physical input only.
struct XboxTestInput {
    std::uint64_t nonce{};
    std::uint32_t duration{}, buttons{};
    std::array<std::uint8_t, 6> axes{128,128,128,128,0,0};
};
inline bool ParseXboxTestInput(const std::string& text, std::uint64_t previous, XboxTestInput& result) {
    if (text.empty() || text.size() > 255) return false;
    // Only decimal numbers and ASCII whitespace; reject signs/wrapped unsigned values.
    for (unsigned char c : text) if ((c < '0' || c > '9') && c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    std::istringstream input(text);
    std::array<std::uint64_t,9> fields{};
    for (auto& value : fields) if (!(input >> value)) return false;
    std::string extra;
    if (input >> extra) return false;
    if (fields[0] <= previous || fields[1] < 1 || fields[1] > 5000 || fields[2] > 0x3fffff) return false;
    for (unsigned i=3; i<9; ++i) if (fields[i] > 255) return false;
    XboxTestInput next{};
    next.nonce=fields[0]; next.duration=static_cast<std::uint32_t>(fields[1]);
    next.buttons=static_cast<std::uint32_t>(fields[2]);
    for (unsigned i=0;i<6;++i) next.axes[i]=static_cast<std::uint8_t>(fields[i+3]);
    result=next;
    return true;
}
