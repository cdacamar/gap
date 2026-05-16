#pragma once

#include "diff-core.h"
#include "diff-text.h"
#include "feed.h"
#include "glyph-cache.h"
#include "types.h"
#include "ui-common.h"

namespace Diff
{
    struct DiffDirPanelResponse
    {
        uint64_t diff_idx;
        bool close;
        bool pop_to_diff;
    };

    enum class NextDiffOrder
    {
        Next,
        Previous,
    };

    struct DiffDirPanel;

    // Creation.
    DiffDirPanel* make_diff_dir_panel(Glyph::Atlas* atlas);

    // Clenaup.
    void release_diff_dir_panel(DiffDirPanel* panel);

    // Interaction.
    void diff_dir_panel_start(DiffDirPanel* panel, const ScreenDimensions& screen, UI::UIState* state);
    void diff_dir_panel_sync_config(DiffDirPanel* panel);
    void diff_dir_panel_dir_A(DiffDirPanel* panel, String8 path, Feed::MessageFeed* feed);
    void diff_dir_panel_dir_B(DiffDirPanel* panel, String8 path, Feed::MessageFeed* feed);
    void diff_dir_panel_apply_diff(DiffDirPanel* panel, Feed::MessageFeed* feed);
    void diff_dir_panel_try_dir_drop(DiffDirPanel* panel, String8 path, UI::UIState* state, Feed::MessageFeed* feed);
    bool diff_dir_panel_nav_diff(DiffDirPanel* panel, NextDiffOrder order, Feed::MessageFeed* feed);
    void diff_dir_panel_sync_thread_data(DiffDirPanel* panel, Feed::MessageFeed* feed);

    // Queries.
    DiffDirDiffResults diff_dir_panel_cached_diffs(DiffDirPanel* panel, uint64_t diff_idx);
    uint64_t diff_dir_panel_selected_diff(DiffDirPanel* panel);

    // Building.
    DiffDirPanelResponse build_diff_dir_panel(DiffDirPanel* panel,
                                                CmdBuffer::CmdList* cmd_lst,
                                                CmdBuffer::DrawList* core_lst,
                                                UI::UIState* state,
                                                Feed::MessageFeed* feed);
} // namespace Diff