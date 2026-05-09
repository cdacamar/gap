#include "diff-dir-panel.h"

#include <cassert>

#include "basic-button.h"
#include "basic-window.h"
#include "diff-dir-list.h"
#include "gap-core.h"
#include "os.h"
#include "timers.h"
#include "tooltips.h"

namespace Diff
{
    namespace
    {
        struct DiffDirPanelUIData
        {
            float wheel_offset_amount;
        };

        struct PartitionDirPanel
        {
            static constexpr float padding = 2.f;

            PartitionDirPanel* sib_next;
            PartitionDirPanel* sib_prev;
            CmdBuffer::DrawList* draw_lst;
            Arena::Arena* text_file_arena;
            UI::Widgets::ID id;
            DiffDirListView* view;
            float pct_of_parent;
            float ease_offset;
        };

        read_only PartitionDirPanel null_dir_panel_inst = {
            .sib_next = &null_dir_panel_inst,
            .sib_prev = &null_dir_panel_inst,
        };

        PartitionDirPanel* null_dir_panel()
        {
            return &null_dir_panel_inst;
        }

        bool null_dir_panel(PartitionDirPanel* panel)
        {
            return panel == &null_dir_panel_inst;
        }

        CmdBuffer::ClipRect clip_from_parent(CmdBuffer::ClipRect parent_clip, PartitionDirPanel* first, PartitionDirPanel* target)
        {
            Vec4f clip = UI::clip_as_vec(parent_clip);
            Vec2f parent_size{ rep(parent_clip.width) + 0.f, rep(parent_clip.height) + 0.f };
            // Make the width the same as the start offset (so we can sum widths based on %).
            clip.p1[0] = clip.p0[0];
            // Note: We only layout on one axis so this loop is simplified.
            for (;not null_dir_panel(first); first = first->sib_next)
            {
                clip.p1[0] += parent_size.xy[0] * first->pct_of_parent;
                if (first == target)
                    break;
                clip.p0[0] = clip.p1[0];
            }
            return UI::vec_as_clip(clip);
        }

        void init_dir_panel(PartitionDirPanel* panel, UI::Widgets::ID seed_id, uint32_t seed_idx, Glyph::Atlas* atlas)
        {
            panel->id = UI::Widgets::make_id_seed_idx(seed_id, seed_idx);
            panel->draw_lst = CmdBuffer::alloc_draw_list();
            panel->text_file_arena = Arena::alloc(Arena::default_params);
            panel->ease_offset = 1.f;
            panel->pct_of_parent = .5f;
            panel->sib_next = panel->sib_prev = null_dir_panel();
            // Allocate the diff dir view.
            panel->view = make_diff_dir_list_view(atlas, UI::Widgets::make_id_seed(panel->id, "dir_view"));
        }

        Render::RenderViewport initial_window_viewport(const ScreenDimensions& screen)
        {
            auto viewport = Render::RenderViewport::basic(screen);
            Vec2f center = UI::center_clip(UI::convert(viewport));
            viewport.width = Width{ static_cast<int>(rep(viewport.width) * 0.75) };
            viewport.height = Height{ static_cast<int>(rep(viewport.height) * 0.75) };
            viewport.offset_x = Render::ViewportOffsetX{ static_cast<int>(center.x - rep(viewport.width) / 2) };
            viewport.offset_y = Render::ViewportOffsetY{ static_cast<int>(center.y - rep(viewport.height) / 2) };
            return viewport;
        }

        void insert_flat_dir_entry_sorted(Arena::Arena* arena, FlatDirEntryList* lst, OS::DirIterResult item)
        {
            FlatDirEntry* e = Arena::push_array<FlatDirEntry>(arena, 1);
            e->item = item;
            e->item.path = str8_copy(arena, e->item.path);
            // Find the insert point.
            FlatDirEntry* prev = nullptr;
            uint64_t node_depth = depth_of_file(e->item.path);
            uint64_t n_depth = 0;
            bool inserted = false;
            for EachNode(n, lst->first)
            {
                n_depth = depth_of_file(n->item.path);
                if (node_depth < n_depth
                    or (node_depth == n_depth and str8_compare(e->item.path, n->item.path) < 0))
                {
                    DLLInsert(lst->first, lst->last, prev, e);
                    inserted = true;
                    break;
                }
                prev = n;
            }

            if (not inserted)
            {
                DLLPushBack(lst->first, lst->last, e);
            }
            ++lst->count;
        }

        void populate_flattend_dir_entries(Arena::Arena* arena, FlatDirEntryList* lst, String8 path, Feed::MessageFeed* feed)
        {
            if (not OS::directory_exists(path))
            {
                String8 msg = str8_fmt(arena, "Dropped path '%S' is not a directory.", path);
                feed->queue_error(msg);
                return;
            }
            auto scratch = Arena::scratch_begin({ &arena, 1 });
            // First, we need to canonicalize the path.
            String8 canon_path = str8_empty;
            // Just allocate to output arena for list.
            OS::Error err = OS::canonical_file_path(arena, &canon_path, path);
            bool good = true;
            if (err != OS::Error::None)
            {
                String8 msg = str8_fmt(scratch.arena, "Unable to resolve path '%S': %S", path, OS::error_text(err));
                feed->queue_error(msg);
                good = false;
            }

            if (good)
            {
                lst->base_dir = canon_path;
                OS::DirIterResult item;
                // Create a queue for recursive directories.
                String8Node* head = Arena::push_array<String8Node>(arena, 1);
                String8Node* free_lst = nullptr;
                head->string = canon_path;
                do
                {
                    String8 dir = head->string;
                    {
                        String8Node* n = head;
                        SLLStackPop(head);
                        SLLStackPush(free_lst, n);
                    }
                    OS::DirIter os_itr = OS::open_dir_iter(dir, OS::DirIterFlags::None);
                    // Do we have a good iterator?
                    if (os_itr != OS::DirIter::Sentinel)
                    {
                        do
                        {
                            if (not OS::dir_iter_next(scratch.arena, &item, os_itr))
                                break;
                            if (implies(item.props.props, OS::FileProperty::Directory))
                            {
                                String8Node* next_dir = free_lst;
                                if (next_dir != nullptr)
                                {
                                    SLLStackPop(free_lst);
                                    zero_bytes(next_dir);
                                }
                                else
                                {
                                    next_dir = Arena::push_array<String8Node>(scratch.arena, 1);
                                }
                                // Note: This string lives in the scratch arena with our stack nodes.
                                next_dir->string = OS::combine_paths(scratch.arena, dir, item.path);
                                SLLStackPush(head, next_dir);
                            }
                            // Otherwise, regular file.
                            else
                            {
                                // Fake the relative path by providing the dir but chopping the base_dir.
                                String8 rel_dir = str8_chop_prefix(dir, canon_path);
                                item.path = OS::combine_paths(scratch.arena, rel_dir, item.path);
                                insert_flat_dir_entry_sorted(arena, lst, item);
                            }
                        } while (true);
                        OS::close_dir_iter(os_itr);
                    }
                } while (head != nullptr);
            }
            Arena::scratch_end(scratch);
        }
    } // namespace [anon]

    struct DiffDirPanel
    {
        Arena::Arena* arena;
        Glyph::Atlas* atlas;
        CmdBuffer::DrawList* frame_lst;
        UI::Widgets::ID id;
        UI::Widgets::BasicWindow* window;
        PartitionDirPanel A;
        PartitionDirPanel B;
        DiffDirPanelUIData ui_data;
    };

    // Creation.
    DiffDirPanel* make_diff_dir_panel(Glyph::Atlas* atlas)
    {
        Arena::Arena* arena = Arena::alloc(Arena::default_params);
        DiffDirPanel* panel = Arena::push_array<DiffDirPanel>(arena, 1);
        panel->arena = arena;
        panel->atlas = atlas;
        panel->frame_lst = CmdBuffer::alloc_draw_list();
        panel->id = UI::Widgets::ID::DiffDirPanel;
        {
            uint8_t* blob = Arena::push_array_no_zero_aligned<uint8_t>(arena,
                                                                        sizeof(UI::Widgets::BasicWindow),
                                                                        Arena::Alignment{ alignof(UI::Widgets::BasicWindow) });
            panel->window = new(blob) UI::Widgets::BasicWindow{ panel->id };
            panel->window->title("Diff Directories");
        }
        init_dir_panel(&panel->A, panel->id, 0, atlas);
        init_dir_panel(&panel->B, panel->id, 1, atlas);
        // Connect A and B.
        panel->A.sib_next = &panel->B;
        panel->B.sib_prev = &panel->A;
        return panel;
    }

    // Cleanup.
    void release_diff_dir_panel(DiffDirPanel* panel)
    {
        using Wind = UI::Widgets::BasicWindow;
        panel->window->~Wind();

        for (PartitionDirPanel* child = &panel->A;
            not null_dir_panel(child);
            child = child->sib_next)
        {
            release_diff_dir_list_view(child->view);
        }
        CmdBuffer::release_draw_list(panel->frame_lst);
        CmdBuffer::release_draw_list(panel->A.draw_lst);
        CmdBuffer::release_draw_list(panel->B.draw_lst);
        Arena::Arena* arena = panel->arena;
        Arena::release(arena);
    }

    // Interaction.
    void diff_dir_panel_start(DiffDirPanel* panel, const ScreenDimensions& screen, UI::UIState* state)
    {
        UI::Widgets::ShowWindowData show_data{
            .initial_viewport = initial_window_viewport(screen),
            .expand_point = { 0.5f, 0.5f }
        };
        panel->window->show(show_data);
        panel->window->background_alpha(0.8f);
        UI::set_focus_window(state, UI::Widgets::ID::ConfigExplorer);
        panel->ui_data = {};
        panel->ui_data.wheel_offset_amount = UI::standard_font_padding(Glyph::FontSize{ Config::diff_state().diff_font_size }) * 2;
    }

    void diff_dir_panel_sync_config(DiffDirPanel* panel)
    {
        panel->window->sync_config(panel->atlas);
    }

    void diff_dir_panel_dir_A(DiffDirPanel* panel, String8 path, Feed::MessageFeed* feed)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        FlatDirEntryList dir_entries = {};
        populate_flattend_dir_entries(scratch.arena, &dir_entries, path, feed);
        diff_dir_list_view_populate_files(panel->A.view, dir_entries);
        Arena::scratch_end(scratch);
    }

    void diff_dir_panel_dir_B(DiffDirPanel* panel, String8 path, Feed::MessageFeed* feed)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        FlatDirEntryList dir_entries = {};
        populate_flattend_dir_entries(scratch.arena, &dir_entries, path, feed);
        diff_dir_list_view_populate_files(panel->B.view, dir_entries);
        Arena::scratch_end(scratch);
    }

    void diff_dir_panel_apply_diff(DiffDirPanel* panel, Feed::MessageFeed* feed)
    {
        DirFileArray files_A = diff_dir_list_view_file_array(panel->A.view);
        DirFileArray files_B = diff_dir_list_view_file_array(panel->B.view);
        if (files_A.size == 0)
        {
            feed->queue_info("Please profide a directory for the left side comparison.");
            return;
        }

        if (files_B.size == 0)
        {
            feed->queue_info("Please profide a directory for the right side comparison.");
            return;
        }
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Clear panel text file arenas.
        Arena::clear(panel->A.text_file_arena);
        Arena::clear(panel->B.text_file_arena);
        uint64_t idx_A = 0;
        uint64_t idx_B = 0;
        // The lists to place our files.
        MergedFileList dirs_A = {};
        MergedFileList dirs_B = {};
        while (idx_A < files_A.size and idx_B < files_B.size)
        {
            OS::DirIterResult* file_A = &files_A.array[idx_A];
            OS::DirIterResult* file_B = &files_B.array[idx_B];
            // We use the same comparison function as the one in 'insert_flat_dir_entry_sorted'.
            uint64_t depth_A = depth_of_file(file_A->path);
            uint64_t depth_B = depth_of_file(file_B->path);
            int same = int(depth_A) - int(depth_B);
            if (same == 0)
            {
                same = str8_compare(file_A->path, file_B->path);
            }

            // Just insert 1-1.
            if (same == 0)
            {
                MergedFileNode* entries = Arena::push_array<MergedFileNode>(scratch.arena, 2);
                entries[0].merged.file.path = file_A->path;
                entries[1].merged.file.path = file_B->path;
                entries[0].merged.type = EditType::Eq;
                entries[1].merged.type = EditType::Eq;
                SLLQueuePush(dirs_A.first, dirs_A.last, &entries[0]);
                SLLQueuePush(dirs_B.first, dirs_B.last, &entries[1]);
                ++dirs_A.count;
                ++dirs_B.count;
                // Move both iterators forward.
                ++idx_A;
                ++idx_B;
            }
            // The file entry for A has no entry in B.  We need to create a deletion
            // in A and continue to create deletions until our comparison is no longer
            // less-than.
            else if (same < 0)
            {
                do
                {
                    MergedFileNode* entries = Arena::push_array<MergedFileNode>(scratch.arena, 2);
                    entries[0].merged.file.path = file_A->path;
                    entries[1].merged.file.path = str8_empty;
                    entries[0].merged.type = EditType::Del;
                    entries[1].merged.type = EditType::Invalid;
                    SLLQueuePush(dirs_A.first, dirs_A.last, &entries[0]);
                    SLLQueuePush(dirs_B.first, dirs_B.last, &entries[1]);
                    ++dirs_A.count;
                    ++dirs_B.count;

                    // Setup for the next file.
                    ++idx_A;
                    if (idx_A < files_A.size)
                    {
                        file_A = &files_A.array[idx_A];
                        depth_A = depth_of_file(file_A->path);
                        same = int(depth_A) - int(depth_B);
                        if (same == 0)
                        {
                            same = str8_compare(file_A->path, file_B->path);
                        }
                    }
                } while (same < 0 and idx_A < files_A.size);
            }
            // The file entry for B has no entry in A.  We need to create an insertion
            // in B and continue to create insertions until our comparison is no longer
            // greater-than.
            else
            {
                do
                {
                    MergedFileNode* entries = Arena::push_array<MergedFileNode>(scratch.arena, 2);
                    entries[0].merged.file.path = str8_empty;
                    entries[1].merged.file.path = file_B->path;
                    entries[0].merged.type = EditType::Invalid;
                    entries[1].merged.type = EditType::Ins;
                    SLLQueuePush(dirs_A.first, dirs_A.last, &entries[0]);
                    SLLQueuePush(dirs_B.first, dirs_B.last, &entries[1]);
                    ++dirs_A.count;
                    ++dirs_B.count;

                    // Setup for the next file.
                    ++idx_B;
                    if (idx_B < files_B.size)
                    {
                        file_B = &files_B.array[idx_B];
                        depth_B = depth_of_file(file_B->path);
                        same = int(depth_A) - int(depth_B);
                        if (same == 0)
                        {
                            same = str8_compare(file_A->path, file_B->path);
                        }
                    }
                } while (same > 0 and idx_B < files_B.size);
            }
        }
        // Cleanup remaining files from each bucket.
        // Deletions.
        for (;idx_A < files_A.size; ++idx_A)
        {
            OS::DirIterResult* file_A = &files_A.array[idx_A];
            MergedFileNode* entries = Arena::push_array<MergedFileNode>(scratch.arena, 2);
            entries[0].merged.file.path = file_A->path;
            entries[1].merged.file.path = str8_empty;
            entries[0].merged.type = EditType::Del;
            entries[1].merged.type = EditType::Invalid;
            SLLQueuePush(dirs_A.first, dirs_A.last, &entries[0]);
            SLLQueuePush(dirs_B.first, dirs_B.last, &entries[1]);
            ++dirs_A.count;
            ++dirs_B.count;
        }
        // Insertions.
        for (;idx_B < files_B.size; ++idx_B)
        {
            OS::DirIterResult* file_B = &files_B.array[idx_B];
            MergedFileNode* entries = Arena::push_array<MergedFileNode>(scratch.arena, 2);
            entries[0].merged.file.path = str8_empty;
            entries[1].merged.file.path = file_B->path;
            entries[0].merged.type = EditType::Invalid;
            entries[1].merged.type = EditType::Ins;
            SLLQueuePush(dirs_A.first, dirs_A.last, &entries[0]);
            SLLQueuePush(dirs_B.first, dirs_B.last, &entries[1]);
            ++dirs_A.count;
            ++dirs_B.count;
        }
        assert(dirs_A.count == dirs_B.count);
        // Populate.
        diff_dir_list_view_populate_merged_files(panel->A.view, dirs_A);
        diff_dir_list_view_populate_merged_files(panel->B.view, dirs_B);
        Arena::scratch_end(scratch);
        // Phase 2: compute all diffs between corresponding files.
        scratch = Arena::scratch_begin(Arena::no_conflicts);
        Arena::scratch_end(scratch);
    }

    void diff_dir_panel_try_dir_drop(DiffDirPanel* panel, String8 path, UI::UIState* state, Feed::MessageFeed* feed)
    {
        bool dir_evaluated = false;
        CmdBuffer::ClipRect clip = UI::convert(panel->window->content_viewport(panel->window->window_viewport()));
        // Test to see which panel the mouse is over, populate the file and reapply diffs.
        for (PartitionDirPanel* child = &panel->A;
            not null_dir_panel(child);
            child = child->sib_next)
        {
            CmdBuffer::ClipRect child_clip = clip_from_parent(clip, &panel->A, child);
            if (mouse_in_clip(state->mouse.ui_mouse, child_clip))
            {
                dir_evaluated = true;
                auto scratch = Arena::scratch_begin(Arena::no_conflicts);
                FlatDirEntryList dir_entries = {};
                populate_flattend_dir_entries(scratch.arena, &dir_entries, path, feed);
                diff_dir_list_view_populate_files(child->view, dir_entries);
                Arena::scratch_end(scratch);
                // Now try to apply diffs.
                diff_dir_panel_apply_diff(panel, feed);
                break;
            }
        }

        if (not dir_evaluated)
        {
            feed->queue_warning("Please drop file over specific side to apply directory diffs.");
        }
    }

    // Building.
    DiffDirPanelResponse build_diff_dir_panel(DiffDirPanel* panel,
                                                CmdBuffer::CmdList* cmd_lst,
                                                CmdBuffer::DrawList* core_lst,
                                                UI::UIState* state,
                                                Feed::MessageFeed*)
    {
        PROF_SCOPE();

        DiffDirPanelResponse resp = {};

        CmdBuffer::ClipRect clip = CmdBuffer::current_clip(*core_lst);
        const auto& colors = Config::widget_colors();

        // Start the frame for the enclosing editor frame.
        CmdBuffer::new_frame(panel->frame_lst, core_lst->screen, { .dt = core_lst->delta_time, .app_time = core_lst->app_time });
        // Default clip rect for the screen.
        CmdBuffer::push_clip(panel->frame_lst, clip);
        // Default texture (atlas by default).
        CmdBuffer::push_texture(panel->frame_lst, panel->atlas->atlas_texture());
        // Default palette.
        CmdBuffer::push_color_palette(panel->frame_lst, *CmdBuffer::current_palette(*core_lst));

        // Build the window first.
        {
            auto window_resp = panel->window->build(panel->frame_lst, panel->atlas, state);
            resp.close = window_resp.close;
        }
        // Now we can constrain the clip.
        clip = UI::convert(panel->window->content_viewport(panel->window->window_viewport()));
        CmdBuffer::push_clip(panel->frame_lst, clip);
        Glyph::FontSize font_size = Glyph::FontSize{ Config::diff_state().diff_font_size };

        // Build panel decoration UI.
        {
            CmdBuffer::ClipRect header_clip = clip;
            auto font_ctx = panel->atlas->render_font_context(font_size);
            header_clip.height = Height(UI::standard_font_padding(font_size) * 2);
            Vec2f base_pos;
            base_pos.y = static_cast<float>(rep(clip.height));
            // Create titles for each panel and center them.
            for (PartitionDirPanel* child = &panel->A;
                not null_dir_panel(child);
                child = child->sib_next)
            {
                // TODO.
#if 0
                CmdBuffer::ClipRect child_clip = clip_from_parent(clip, &panel->A, child);
                const TextFile* file = text_file(child->view);
                String8 name = file->path;
                CmdBuffer::start_glyph_run(panel->frame_lst, Render::VertShader::OneOneTransform);
                Vec2f pos = base_pos;
                pos.y -= font_ctx.current_font_line_height();
                pos.x = rep(child_clip.offset_x) + (rep(child_clip.width) - font_ctx.measure_text(name).x) / 2.f;
                font_ctx.render_text(panel->frame_lst, name, pos, colors.window_title_font_color);
#endif
            }
            // Replace the clip.
            clip.height = retract(clip.height, rep(header_clip.height));
            CmdBuffer::pop_clip(panel->frame_lst);
            CmdBuffer::push_clip(panel->frame_lst, clip);
        }

        // Build non-leaf UI.
        {
            CmdBuffer::start_shapes(panel->frame_lst, Render::VertShader::OneOneTransform);
            Vec4f region_color = colors.outline_selection;
            const float boundary_width_bias = rep(font_size) / 3.f;
            for (PartitionDirPanel* child = &panel->A;
                // Non-leaf UI does only involves inner-panels (e.g. the fence post problem).
                not null_dir_panel(child) and not null_dir_panel(child->sib_next);
                child = child->sib_next)
            {
                PartitionDirPanel* sib = child->sib_next;
                CmdBuffer::ClipRect child_clip = clip_from_parent(clip, &panel->A, child);
                CmdBuffer::ClipRect sib_clip = clip_from_parent(clip, &panel->A, sib);
                CmdBuffer::ClipRect boundary_clip = {};

                Vec4f panelv_clip = clip_as_vec(clip);
                {
                    Vec4f childv_clip = clip_as_vec(child_clip);
                    Vec4f sibv_clip = clip_as_vec(sib_clip);
                    Vec4f boundaryv_clip{};
                    boundaryv_clip.p0[0] = childv_clip.p1[0] - PartitionDirPanel::padding;
                    boundaryv_clip.p1[0] = sibv_clip.p0[0] + PartitionDirPanel::padding;
                    boundaryv_clip.p0[1] = panelv_clip.p0[1];
                    boundaryv_clip.p1[1] = panelv_clip.p1[1];
                    boundary_clip = vec_as_clip(boundaryv_clip);
                }

                Widgets::ID boundary_id = Widgets::ID::Zero;
                {
                    Widgets::ID ids[] = { panel->id, child->id, sib->id };
                    Widgets::MultiSeed multi_seed_in{
                        .first = ids,
                        .last = ids + std::size(ids)
                    };
                    boundary_id = Widgets::make_multi_seed(multi_seed_in, "bndry");
                }

                if (mouse_in_clip(state->mouse.ui_mouse, pad_clip(boundary_clip, Vec2i(static_cast<int>(-boundary_width_bias)))))
                {
                    try_set_hot_widget(state, boundary_id);
                    if (down(*state, MouseButton::L))
                    {
                        bool first_focus = state->focus_widget != boundary_id;
                        try_set_focus_widget(state, boundary_id);
                        if (state->focus_widget == boundary_id
                            and first_focus)
                        {
                            // Stash some drag data.
                            Vec2f start_pct{ child->pct_of_parent, sib->pct_of_parent };
                            start_drag(state, boundary_id, state->mouse.ui_mouse, start_pct);
                        }
                    }
                }

                // Process movement.
                if (dragging(*state, boundary_id))
                {
                    const Vec2f* drag_data = drag_payload<Vec2f>(state);
                    constexpr float min_pixel_value = 50.f;
                    Vec2i mouse_delta = state->mouse.ui_mouse - state->drag.payload.start_point;
                    float total_size = panelv_clip.p1[0] - panelv_clip.p0[0];
                    float child_pct_before = drag_data->x; // Child %.
                    float child_pixels_before = child_pct_before * total_size;
                    float child_pixels_after = std::max(child_pixels_before + mouse_delta.xy[0], min_pixel_value);
                    float child_pct_after = child_pixels_after / total_size;

                    float pct_delta = child_pct_after - child_pct_before;
                    float sib_pct_before = drag_data->y; // Sib %.
                    float sib_pct_after = sib_pct_before - pct_delta;
                    float sib_pixels_after = sib_pct_after * total_size;
                    if (sib_pixels_after < 50.f)
                    {
                        sib_pixels_after = 50.f;
                        sib_pct_after = sib_pixels_after / total_size;
                        pct_delta = -(sib_pct_after - sib_pct_before);
                        child_pct_after = child_pct_before + pct_delta;
                    }
                    child->pct_of_parent = child_pct_after;
                    sib->pct_of_parent = sib_pct_after;
                }

                if (state->focus_widget == boundary_id
                    and not down(*state, MouseButton::L)
                    and clicked_count(*state, MouseButton::L) == 2)
                {
                    // If the boundary is double-clicked, we'll resize both boundaries to be even.
                    float pct_sum = child->pct_of_parent + sib->pct_of_parent;
                    child->pct_of_parent = 0.5f * pct_sum;
                    sib->pct_of_parent = 0.5f * pct_sum;
                }

                if ((state->hot_widget == boundary_id
                        and self_or_empty_focus_widget(*state, boundary_id))
                    or dragging(*state, boundary_id))
                {
                    auto [pos, size] = pos_size_clip(boundary_clip);
                    // Remove the offsets from the enclosing window clip.
                    pos.x -= rep(clip.offset_x);
                    pos.y -= rep(clip.offset_y);
                    CmdBuffer::solid_rect(panel->frame_lst, Render::FragShader::BasicColor, pos, size, region_color);
                    change_cursor(state, UI::CursorStyle::LeftRightArrow);
                }
            }
        }

        DiffDirListView* scroll_changed[] = {
            nullptr,
            nullptr
        };
        uint64_t scroll_idx = 0;

        // Build leaf-UI.
        for (PartitionDirPanel* child = &panel->A;
            not null_dir_panel(child);
            child = child->sib_next)
        {
            CmdBuffer::ClipRect child_clip = clip_from_parent(clip, &panel->A, child);
            // Setup command buffer for panel.
            CmdBuffer::new_frame(child->draw_lst, core_lst->screen, { .dt = core_lst->delta_time, .app_time = core_lst->app_time });
            // Create the rect.
            CmdBuffer::push_clip(child->draw_lst, child_clip);
            // Default texture (atlas by default).
            CmdBuffer::push_texture(child->draw_lst, panel->atlas->atlas_texture());
            // Default palette.
            CmdBuffer::push_color_palette(child->draw_lst, *CmdBuffer::current_palette(*core_lst));

            // Build core widget.
            auto r = build_diff_dir_list_view(child->view, child->draw_lst, state);
            scroll_changed[scroll_idx++] = r.scroll_changed ? child->view : nullptr;

            CmdBuffer::pop_clip(child->draw_lst);
            CmdBuffer::pop_texture(child->draw_lst);
            CmdBuffer::pop_color_palette(child->draw_lst);

            CmdBuffer::push_draw_list(cmd_lst, CmdBuffer::DrawListLayer::_2, child->draw_lst);
        }

        for EachIndex(i, std::size(scroll_changed))
        {
            DiffDirListView* view = scroll_changed[i];
            if (view != nullptr)
            {
                DiffDirListView* share_to = panel->A.view;
                if (view == panel->A.view)
                {
                    share_to = panel->B.view;
                }
                diff_dir_list_view_share_scroll_pos(share_to, view);
            }
        }

        panel->window->end(state);

        CmdBuffer::pop_clip(panel->frame_lst); // Window.
        CmdBuffer::pop_clip(panel->frame_lst); // Core.
        CmdBuffer::pop_texture(panel->frame_lst);
        CmdBuffer::pop_color_palette(panel->frame_lst);

        CmdBuffer::push_draw_list(cmd_lst, CmdBuffer::DrawListLayer::_1, panel->frame_lst);
        return resp;
    }
} // namespace Diff