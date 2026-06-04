#pragma once

#include "diff-core.h"
#include "diff-text.h"
#include "glyph-cache.h"
#include "ui-common.h"

namespace Diff
{
    struct DiffFileForViewResult
    {
        MergedLineList lst_A;
        MergedLineList lst_B;
        MergedTextList merged_txt_A;
        MergedTextList merged_txt_B;
    };

    struct DiffFileForViewInput
    {
        const TextFile* A;
        const TextFile* B;
        bool word_based_diff;
    };

    struct DiffPanelResponse
    {
        bool updated_cfg;
        bool switch_diff;
        Diff::NextDiffOrder order;
    };

    struct DiffPanel;

    // Creation.
    DiffPanel* make_diff_panel(Glyph::Atlas* atlas);

    // Cleanup.
    void release_diff_panel(DiffPanel* panel);

    // Interaction.
    void diff_panel_file_A(DiffPanel* panel, const TextFile& file);
    void diff_panel_file_B(DiffPanel* panel, const TextFile& file);
    void diff_panel_apply_diff(DiffPanel* panel, Feed::MessageFeed* feed);
    void diff_panel_sync_config(DiffPanel* panel, Feed::MessageFeed* feed);
    void diff_panel_try_file_drop(DiffPanel* panel, String8 path, UI::UIState* state, Feed::MessageFeed* feed);
    void diff_panel_sink_cached_diffs(DiffPanel* panel, const DiffDirDiffResults& diffs);

    // Helpers.
    DiffFileForViewResult diff_panel_diff_files_for_view(Arena::Arena* arena, DiffFileForViewInput in);
    DiffFileForViewResult diff_panel_diff_files_for_view_lines_only(Arena::Arena* arena, DiffFileForViewInput in);

    // Building.
    DiffPanelResponse build_diff_panel(DiffPanel* panel,
                                        CmdBuffer::CmdList* cmd_lst,
                                        CmdBuffer::DrawList* core_lst,
                                        UI::UIState* state,
                                        Feed::MessageFeed* feed);
} // namespace Diff