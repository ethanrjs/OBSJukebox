#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace separate_song {
// Whether text is well-formed UTF-8: no stray bytes, overlong forms, surrogates or values above
// U+10FFFF. Paths cross from GD to OBS as UTF-8, and on Windows building a std::filesystem::path
// from anything else throws.
inline bool validUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        auto c = static_cast<unsigned char>(text[i]);
        std::size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (!length || i + length > text.size())
            return false;
        uint32_t value = length == 1 ? c : c & (0x7F >> length);
        for (std::size_t k = 1; k < length; ++k) {
            auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80)
                return false;
            value = value << 6 | (next & 0x3F);
        }
        if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) || (length == 4 && value < 0x10000) ||
            value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
            return false;
        i += length;
    }
    return true;
}
} // namespace separate_song
