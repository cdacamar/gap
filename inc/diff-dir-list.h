#pragma once

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

    struct DiffDirListViewResponse
    {
        bool scroll_changed;
    };

    struct DiffDirListView;

    // Creation.
    DiffDirListView* make_diff_dir_list_view(Glyph::Atlas* atlas, UI::Widgets::ID id);

    // Cleanup.
    void release_diff_dir_list_view(DiffDirListView* widget);

    // Interaction.
    void diff_dir_list_view_populate_files(DiffDirListView* widget, FlatDirEntryList lst);
    void diff_dir_list_view_populate_merged_files(DiffDirListView* widget, MergedFileList lst);
    void diff_dir_list_view_share_scroll_pos(DiffDirListView* widget, const DiffDirListView* share_from);

    // Queries.
    DirFileArray diff_dir_list_view_file_array(DiffDirListView* widget);
    MergedFileArray diff_dir_list_view_merged_file_array(DiffDirListView* widget);

    // Building.
    DiffDirListViewResponse build_diff_dir_list_view(DiffDirListView* widget,
                                                        CmdBuffer::DrawList* lst,
                                                        UI::UIState* state);
} // namespace Diff