#pragma once

#include "arena.h"
#include "gap-strings.h"
#include "types.h"

namespace Diff
{
    struct LineStarts
    {
        Editor::CharOffset* array;
        uint64_t size;
    };

    struct TextFile
    {
        String8 content;
        LineStarts line_starts;
    };

    using OffT = int32_t;

    constexpr OffT diff_idx_sentinel = -1;

    enum class EditType : uint8_t
    {
        Del,
        Ins,
        Eq,
        Invalid,
        Count
    };

    struct Edit
    {
        OffT idx_a;
        OffT idx_b;
        EditType type;
    };

    struct EditNode
    {
        EditNode* next;
        Edit edit;
    };

    struct EditList
    {
        EditNode* first;
        EditNode* last;
        uint64_t count;
    };

    struct LineRange
    {
        Editor::CharOffset first;
        Editor::CharOffset last;
    };

    // This describes a block of text to diff.
    // underlying_off -- the offset into the buffer.  Indices into the array act as the window itself.
    struct DiffBlock
    {
        Editor::CharOffset* underlying_off;
        uint64_t size;
    };

    struct DiffBlockInput
    {
        DiffBlock block;
        const TextFile* file;
    };

    // Text files.
    TextFile text_file_read(Arena::Arena* arena, String8 path);
    String8 text_file_line_text(const TextFile& file, Editor::CursorLine line);
    LineRange text_file_line_range(const TextFile& file, Editor::CursorLine line);
    Editor::CursorLine text_file_line_for_offset(const TextFile& file, Editor::CharOffset off);
    TextFile text_file_copy_to(Arena::Arena* arena, const TextFile& file);

    // Diffing.
    // Note: This is the linear space variant of the Myers diff algorithm.
    EditList diff_file_lines(Arena::Arena* arena, const TextFile& a, const TextFile& b);
    EditList diff_file_block(Arena::Arena* arena, const DiffBlockInput& a, const DiffBlockInput& b);
} // namespace Diff