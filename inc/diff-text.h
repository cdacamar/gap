#pragma once

#include "diff-core.h"
#include "gap-strings.h"
#include "glyph-cache.h"
#include "ui-common.h"

namespace Diff
{
    struct MergedLine
    {
        CharOffset first; // First == sentinel implies gap line.
        CharOffset last;
        uint64_t v_line; // The visual line into the merged buffer.
        CursorLine line; // Line for actual text.
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
        CharOffset first;
        CharOffset last;
        uint64_t v_line; // The visual line into the merged buffer.
        CursorLine line; // Line for actual text.
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

    // Data for populating cached results.
    struct DiffDirDiffEntry
    {
        const TextFile* file;
        MergedDiffView file_line_diffs;
        MergedTextBlocks file_text_block_diffs;
    };

    struct DiffDirDiffResults
    {
        DiffDirDiffEntry A;
        DiffDirDiffEntry B;
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
    void diff_text_view_sync_config(DiffTextView* widget);
    void diff_text_view_populate_text(DiffTextView* widget, const TextFile& text);
    void diff_text_view_populate_line_diff(DiffTextView* widget, MergedLineList lst);
    void diff_text_view_populate_text_blocks_diff(DiffTextView* widget, MergedTextList lst);
    void diff_text_view_share_scroll_pos(DiffTextView* widget, const DiffTextView* share_from);
    void diff_text_view_reset_scroll_pos(DiffTextView* widget);
    void diff_text_view_apply_context_window(DiffTextView* widget);
    void diff_text_view_sink_cached_diffs(DiffTextView* widget, const TextFile& text, MergedDiffView diff_lines, MergedTextBlocks diff_text_blocks);

    // Helpers.
    MergedTextNode* diff_text_view_push_merged_text(Arena::Arena* arena, MergedTextList* lst, MergedText merged);
    MergedLineNode* diff_text_view_push_merge_line(Arena::Arena* arena, MergedLineList* lst, MergedLine line);
    MergedDiffView diff_text_view_join_merged_line_list(Arena::Arena* arena, MergedLineList lst);
    MergedTextBlocks diff_text_view_join_merged_text_blocks_list(Arena::Arena* arena, MergedTextList lst, const TextFile& file);

    // Queries.
    TextFile* diff_text_view_text_file(DiffTextView* widget);

    // Building.
    DiffTextViewResponse build_diff_text_view(DiffTextView* widget,
                                                CmdBuffer::DrawList* lst,
                                                UI::UIState* state);
} // namespace Diff