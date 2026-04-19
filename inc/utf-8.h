#pragma once

#include <stdint.h>

#include <string_view>

#include "types.h"

namespace UTF8
{
    using Codepoint = uint32_t;
    using Offset = Diff::CharOffset;

    struct EncodeInput
    {
        unsigned char buf[4];
    };

    using EncodeOutput = std::string_view;

    constexpr Codepoint invalid_codepoint = static_cast<Codepoint>(-1);

    struct CodepointResult
    {
        Codepoint codepoint;
        Offset first;
        Offset last;

        bool operator==(const CodepointResult&) const = default;
    };

    constexpr CodepointResult invalid { .codepoint = invalid_codepoint, .first = {}, .last = {} };

    CodepointResult next_codepoint(std::string_view input, Offset start = Offset{ 0 });
    bool non_ascii_codepoint(unsigned char c);
    // Is this byte something in the middle of a valid UTF-8 sequence?
    bool trailing_codepoint_byte(unsigned char c);
    bool ascii_codepoint(Codepoint cp);
    size_t codepoint_count(std::string_view input, Offset start = Offset{ 0 });

    // Encoding.
    // Note: This returns a view over the input, so be sure that the output lives as long as the input.
    EncodeOutput utf8_encode(EncodeInput* in, Codepoint cp);

    class CodepointWalker
    {
    public:
        explicit CodepointWalker(std::string_view text, Offset start = Offset{ 0 });

        Codepoint next();
        CodepointResult next_result();
        bool exhausted() const;
    private:
        std::string_view text;
        UTF8::Offset current;
    };
} // namespace UTF8