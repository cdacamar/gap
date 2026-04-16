#pragma once

#include "diff-core.h"
#include "gap-strings.h"
#include "glyph-cache.h"
#include "ui-common.h"

namespace Diff
{
    struct MergedLine
    {
        Editor::CharOffset first; // First == sentinel implies gap line.
        Editor::CharOffset last;
        uint64_t v_line; // The visual line into the merged buffer.
        Editor::CursorLine line; // Line for actual text.
        EditType type;
    };

    struct MergedLineNode
    {
        MergedLineNode* next;
        MergedLine line;
    };

    struct MergedLineList
    {
        MergedLineNode* first;
        MergedLineNode* last;
        uint64_t count;
    };

    struct MergedDiffView
    {
        MergedLine* lines;
        uint64_t size;
    };

    struct MergedText
    {
        Editor::CharOffset first;
        Editor::CharOffset last;
        uint64_t v_line; // The visual line into the merged buffer.
        Editor::CursorLine line; // Line for actual text.
        EditType type;
    };

    struct MergedTextNode
    {
        MergedTextNode* next;
        MergedText merged;
    };

    struct MergedTextList
    {
        MergedTextNode* first;
        MergedTextNode* last;
        uint64_t count;
    };

    struct MergedTextBlocks
    {
        MergedText* blocks;
        uint64_t size;
    };

    struct DiffTextViewResponse
    {
        bool scroll_changed;
    };

    struct DiffTextView;

    // Creation.
    DiffTextView* make_diff_text_view(Glyph::Atlas* atlas, UI::Widgets::ID id);

    // Cleanup.
    void release_diff_text_view(DiffTextView* widget);

    // Interaction.
    void populate_text(DiffTextView* widget, const TextFile& text);
    void populate_line_diff(DiffTextView* widget, MergedLineList lst);
    void populate_text_blocks_diff(DiffTextView* widget, MergedTextList lst);
    void share_scroll_pos(DiffTextView* widget, const DiffTextView* share_from);
    void apply_context_window(DiffTextView* widget);

    // Helpers.
    MergedTextNode* push_merged_text(Arena::Arena* arena, MergedTextList* lst, MergedText merged);
    MergedLineNode* push_merge_line(Arena::Arena* arena, MergedLineList* lst, MergedLine line);

    // Queries.
    TextFile* text_file(DiffTextView* widget);

    // Building.
    DiffTextViewResponse build_diff_text_view(DiffTextView* widget,
                                                CmdBuffer::DrawList* lst,
                                                UI::UIState* state);
} // namespace Diff