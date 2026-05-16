#pragma once

#include "constants.h"
#include "glyph-cache.h"
#include "ui-common.h"

namespace Diff
{
    struct FlatDirEntry
    {
        FlatDirEntry* next;
        FlatDirEntry* prev;
        OS::DirIterResult item;
    };

    struct FlatDirEntryList
    {
        FlatDirEntry* first;
        FlatDirEntry* last;
        uint64_t count;
        String8 base_dir;
    };

    struct DirFileArray
    {
        OS::DirIterResult* array;
        uint64_t size;
        String8 base_dir;
    };

    struct MergedFile
    {
        TextFile file;
        String8 rel_path;
        EditType type;
    };

    struct MergedFileNode
    {
        MergedFileNode* next;
        MergedFile merged;
    };

    struct MergedFileList
    {
        MergedFileNode* first;
        MergedFileNode* last;
        uint64_t count;
    };

    struct MergedFileArray
    {
        MergedFile* array;
        uint64_t size;
    };

    struct DiffCount
    {
        uint64_t ins; // Sentinel value indicates it is not computed.
        uint64_t del; // Sentinel value indicates it is not computed.

        bool operator==(const DiffCount&) const = default;
    };

    inline constexpr DiffCount diff_count_sentinel = {
        .ins = Constants::max_U64,
        .del = Constants::max_U64,
    };

    struct DiffCountArray
    {
        DiffCount* array;
        uint64_t size;
        uint64_t largest_ins;
        uint64_t largest_del;
    };

    struct DiffDirListViewResponse
    {
        uint64_t file_idx;
        bool scroll_changed;
        bool pop_to_diff;
    };

    enum class SelectedDiffFile : uint64_t
    {
        Sentinel = sentinel_for<SelectedDiffFile>
    };

    struct DiffDirListView;

    // Creation.
    DiffDirListView* make_diff_dir_list_view(Glyph::Atlas* atlas, UI::Widgets::ID id);

    // Cleanup.
    void release_diff_dir_list_view(DiffDirListView* widget);

    // Interaction.
    void diff_dir_list_view_sync_config(DiffDirListView* widget);
    void diff_dir_list_view_populate_files(DiffDirListView* widget, FlatDirEntryList lst);
    void diff_dir_list_view_populate_merged_files(DiffDirListView* widget, MergedFileList lst);
    void diff_dir_list_view_share_scroll_pos(DiffDirListView* widget, const DiffDirListView* share_from);
    void diff_dir_list_view_apply_diff_count_sidebar(DiffDirListView* widget, DiffCountArray counts);
    void diff_dir_list_view_update_diff_count(DiffDirListView* widget, DiffCount count, uint64_t idx);

    // Queries.
    String8 diff_dir_list_view_base_dir(DiffDirListView* widget);
    DirFileArray diff_dir_list_view_file_array(DiffDirListView* widget);
    MergedFileArray diff_dir_list_view_merged_file_array(DiffDirListView* widget);
    bool diff_dir_list_valid_diff(DiffDirListView* widget, uint64_t file_idx);

    // Building.
    DiffDirListViewResponse build_diff_dir_list_view(DiffDirListView* widget,
                                                        CmdBuffer::DrawList* lst,
                                                        UI::UIState* state,
                                                        SelectedDiffFile sel);
} // namespace Diff