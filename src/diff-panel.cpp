#include "diff-panel.h"

#include <cassert>

#include "basic-button.h"
#include "diff-text.h"
#include "gap-core.h"
#include "timers.h"
#include "tooltips.h"

namespace Diff
{
    namespace
    {
        struct PartitionPanel
        {
            static constexpr float padding = 2.f;

            PartitionPanel* sib_next;
            PartitionPanel* sib_prev;
            CmdBuffer::DrawList* draw_lst;
            UI::Widgets::ID id;
            DiffTextView* view;
            float pct_of_parent;
            float ease_offset;
        };

        read_only PartitionPanel null_panel_inst = {
            .sib_next = &null_panel_inst,
            .sib_prev = &null_panel_inst,
        };

        PartitionPanel* null_panel()
        {
            return &null_panel_inst;
        }

        bool null_panel(PartitionPanel* panel)
        {
            return panel == &null_panel_inst;
        }

        CmdBuffer::ClipRect clip_from_parent(CmdBuffer::ClipRect parent_clip, PartitionPanel* first, PartitionPanel* target)
        {
            Vec4f clip = UI::clip_as_vec(parent_clip);
            Vec2f parent_size{ rep(parent_clip.width) + 0.f, rep(parent_clip.height) + 0.f };
            // Make the width the same as the start offset (so we can sum widths based on %).
            clip.p1[0] = clip.p0[0];
            // Note: We only layout on one axis so this loop is simplified.
            for (;not null_panel(first); first = first->sib_next)
            {
                clip.p1[0] += parent_size.xy[0] * first->pct_of_parent;
                if (first == target)
                    break;
                clip.p0[0] = clip.p1[0];
            }
            return UI::vec_as_clip(clip);
        }

        void init_panel(PartitionPanel* panel, UI::Widgets::ID seed_id, uint32_t seed_idx, Glyph::Atlas* atlas)
        {
            panel->id = UI::Widgets::make_id_seed_idx(seed_id, seed_idx);
            panel->draw_lst = CmdBuffer::alloc_draw_list();
            panel->ease_offset = 1.f;
            panel->pct_of_parent = .5f;
            panel->sib_next = panel->sib_prev = null_panel();
            // Allocate the diff text view.
            panel->view = make_diff_text_view(atlas, UI::Widgets::make_id_seed(panel->id, "txt_view"));
        }

        enum class PanelButtons
        {
            SwapDiffs,
            LowerCtxWindow,
            CtxWindow,
            ExpandCtxWindow,
            Count
        };
    } // namespace [anon]

    struct DiffPanel
    {
        Arena::Arena* arena;
        Glyph::Atlas* atlas;
        CmdBuffer::DrawList* frame_lst;
        UI::Widgets::ID id;
        PartitionPanel A;
        PartitionPanel B;
    };

    // Creation.
    DiffPanel* make_diff_panel(Glyph::Atlas* atlas)
    {
        Arena::Arena* arena = Arena::alloc(Arena::default_params);
        DiffPanel* panel = Arena::push_array<DiffPanel>(arena, 1);
        panel->arena = arena;
        panel->atlas = atlas;
        panel->frame_lst = CmdBuffer::alloc_draw_list();
        panel->id = UI::Widgets::ID::DiffPanel;
        init_panel(&panel->A, panel->id, 0, atlas);
        init_panel(&panel->B, panel->id, 1, atlas);
        // Connect A and B.
        panel->A.sib_next = &panel->B;
        panel->B.sib_prev = &panel->A;
        return panel;
    }

    // Cleanup.
    void release_diff_panel(DiffPanel* panel)
    {
        for (PartitionPanel* child = &panel->A;
            not null_panel(child);
            child = child->sib_next)
        {
            release_diff_text_view(child->view);
        }
        CmdBuffer::release_draw_list(panel->frame_lst);
        CmdBuffer::release_draw_list(panel->A.draw_lst);
        CmdBuffer::release_draw_list(panel->B.draw_lst);
        Arena::Arena* arena = panel->arena;
        Arena::release(arena);
    }

    // Interaction.
    void file_A(DiffPanel* panel, const TextFile& file)
    {
        populate_text(panel->A.view, file);
    }

    void file_B(DiffPanel* panel, const TextFile& file)
    {
        populate_text(panel->B.view, file);
    }

    struct OffsetVisualLine
    {
        Editor::CharOffset first;
        uint64_t v_line;
    };

    struct OffsetVisualLineMap
    {
        OffsetVisualLine* array;
        uint64_t size;
    };

    uint64_t v_line_for_offset(OffsetVisualLineMap* map, Editor::CharOffset off)
    {
        if (map->size == 0)
            return 0;
        uint64_t low = 0;
        uint64_t high = map->size - 1;
        uint64_t mid = 0;
        while (low <= high)
        {
            mid = low + ((high - low) / 2);
            if (mid == high)
                break;
            Editor::CharOffset mid_start = map->array[mid].first;
            Editor::CharOffset mid_stop = map->array[mid + 1].first;
            if (off < mid_start)
            {
                high = mid - 1;
            }
            else if (off >= mid_stop)
            {
                low = mid + 1;
            }
            else
            {
                break;
            }
        }
        return map->array[mid].v_line;
    }

    struct BuildMergedListInput
    {
        Arena::Arena* merge_arena;
        MergedLineList A;
        MergedLineList B;
        const TextFile* file_A;
        const TextFile* file_B;
        MergedTextList* merged_A;
        MergedTextList* merged_B;
    };

    void populate_merged_text_list(Arena::Arena* arena, const BuildMergedListInput& in)
    {
        if (in.A.count == 0)
            return;
        if (in.B.count == 0)
            return;
        // Create a list of edits between these.
        // To do this, we need to know the number of total char offsets we're dealing with.  This number is
        // equivalent to the number of offsets in each line of each list.  Let's compute that here and then
        // Create the arrays we will populate after.
        DiffBlockInput block_a = {};
        DiffBlockInput block_b = {};
        // These are used to map offsets back to the visual lines in the diff.
        OffsetVisualLineMap off_map_a = {};
        OffsetVisualLineMap off_map_b = {};
        // Count A.
        for EachNode(n, in.A.first)
        {
            block_a.block.size += rep(distance(n->line.first, n->line.last));
        }
        // Count B.
        for EachNode(n, in.B.first)
        {
            block_b.block.size += rep(distance(n->line.first, n->line.last));
        }
        // Allocate.
        block_a.block.underlying_off = Arena::push_array_no_zero<Editor::CharOffset>(arena, block_a.block.size);
        block_b.block.underlying_off = Arena::push_array_no_zero<Editor::CharOffset>(arena, block_b.block.size);
        off_map_a.size = in.A.count;
        off_map_b.size = in.B.count;
        off_map_a.array = Arena::push_array_no_zero<OffsetVisualLine>(arena, off_map_a.size);
        off_map_b.array = Arena::push_array_no_zero<OffsetVisualLine>(arena, off_map_b.size);
        // Populate.
        uint64_t idx = 0;
        uint64_t idx_map = 0;
        for EachNode(n, in.A.first)
        {
            for (Editor::CharOffset off = n->line.first; off < n->line.last; off = extend(off))
            {
                block_a.block.underlying_off[idx++] = off;
            }
            off_map_a.array[idx_map].first = n->line.first;
            off_map_a.array[idx_map++].v_line = n->line.v_line;
        }
        idx = 0;
        idx_map = 0;
        for EachNode(n, in.B.first)
        {
            for (Editor::CharOffset off = n->line.first; off < n->line.last; off = extend(off))
            {
                block_b.block.underlying_off[idx++] = off;
            }
            off_map_b.array[idx_map].first = n->line.first;
            off_map_b.array[idx_map++].v_line = n->line.v_line;
        }
        block_a.file = in.file_A;
        block_b.file = in.file_B;
        EditList edits = diff_file_block(arena, block_a, block_b);
        // Now our strategy is to iterate this list and create a series of fine-grained edits in each block.
        MergedText current = { .type = EditType::Invalid };
        for EachNode(e, edits.first)
        {
            switch (e->edit.type)
            {
            case EditType::Del:
                // Create if invalid.
                if (current.type == EditType::Invalid)
                {
                    current.type = e->edit.type;
                    current.first = current.last = block_a.block.underlying_off[e->edit.idx_a];
                    current.v_line = v_line_for_offset(&off_map_a, current.first);
                }

                // Commit the current.
                if (current.type != e->edit.type)
                {
                    // Note: We only merge insertions and deletions so this list will always
                    // commit to B.
                    push_merged_text(in.merge_arena, in.merged_B, current);
                    current.type = e->edit.type;
                    current.first = current.last = block_a.block.underlying_off[e->edit.idx_a];
                    current.v_line = v_line_for_offset(&off_map_a, current.first);
                }

                // Test if we have advanced to the next line or skipped a chunk.
                if (current.last != block_a.block.underlying_off[e->edit.idx_a])
                {
                    // Commit it.
                    push_merged_text(in.merge_arena, in.merged_A, current);
                    current.type = e->edit.type;
                    current.first = current.last = block_a.block.underlying_off[e->edit.idx_a];
                    current.v_line = v_line_for_offset(&off_map_a, current.first);
                }
                current.last = extend(current.last);
                break;
            case EditType::Ins:
                // Create if invalid.
                if (current.type == EditType::Invalid)
                {
                    current.type = e->edit.type;
                    current.first = current.last = block_b.block.underlying_off[e->edit.idx_b];
                    current.v_line = v_line_for_offset(&off_map_b, current.first);
                }

                // Commit the current.
                if (current.type != e->edit.type)
                {
                    // Note: We only merge insertions and deletions so this list will always
                    // commit to A.
                    push_merged_text(in.merge_arena, in.merged_A, current);
                    current.type = e->edit.type;
                    current.first = current.last = block_b.block.underlying_off[e->edit.idx_b];
                    current.v_line = v_line_for_offset(&off_map_b, current.first);
                }

                // Test if we have advanced to the next line or skipped a chunk.
                if (current.last != block_b.block.underlying_off[e->edit.idx_b])
                {
                    // Commit it.
                    push_merged_text(in.merge_arena, in.merged_B, current);
                    current.type = e->edit.type;
                    current.first = current.last = block_b.block.underlying_off[e->edit.idx_b];
                    current.v_line = v_line_for_offset(&off_map_b, current.first);
                }
                current.last = extend(current.last);
                break;
            case EditType::Eq:
                break;
            case EditType::Invalid:
                break;
            }
        }
        // Commit the final.
        if (current.type != EditType::Invalid)
        {
            if (current.type == EditType::Del)
            {
                push_merged_text(in.merge_arena, in.merged_A, current);
            }
            else
            {
                assert(current.type == EditType::Ins);
                push_merged_text(in.merge_arena, in.merged_B, current);
            }
        }
    }

    void apply_diff(DiffPanel* panel, Feed::MessageFeed* feed)
    {
        Timers::Stopwatch sw;
        sw.start();
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // This is so that we can avoid possible chaining of scratch above.
        auto scratch2 = Arena::scratch_begin({ &scratch.arena, 1 });
        TextFile* a = text_file(panel->A.view);
        TextFile* b = text_file(panel->B.view);
        EditList edits = diff_file_lines(scratch.arena, *a, *b);
        // What we want is a sequence of 'lines' for both A and B which
        // represent the 'merged' files together.  We will merge deletes
        // and inserts and create 'gap' lines which will be rendered as
        // an empty region in the text view.
        MergedLineList lst_A = {};
        MergedLineList lst_B = {};
        MergedLineList lst_merge_B = {}; // This is to have as a write-back for the B list.
        // These serve as candidate lists for us to, potentially, perform a sub-diff against
        // the respective blocks for better letter-highlighting.
        MergedTextList merged_txt_A = {};
        MergedTextList merged_txt_B = {};
        BuildMergedListInput merged_lst_input = {};
        merged_lst_input.file_A = a;
        merged_lst_input.file_B = b;
        merged_lst_input.merge_arena = scratch.arena;
        merged_lst_input.merged_A = &merged_txt_A;
        merged_lst_input.merged_B = &merged_txt_B;
        for EachNode(e, edits.first)
        {
            switch (e->edit.type)
            {
            case EditType::Del:
                {
                    // Add delete from A.
                    LineRange rng_a = text_file_line_range(*a, Editor::CursorLine(e->edit.idx_a));
                    MergedLine line_a = {
                        .first = rng_a.first,
                        .last = rng_a.last,
                        .v_line = lst_A.count,
                        .line = Editor::CursorLine(e->edit.idx_a),
                        .type = EditType::Del,
                    };
                    push_merge_line(scratch.arena, &lst_A, line_a);
                    MergedLine line_b = {
                        .first = Editor::CharOffset::Sentinel,
                        .last = Editor::CharOffset::Sentinel,
                        .v_line = lst_B.count,
                        .line = Editor::CursorLine::Beginning,
                        .type = EditType::Invalid,
                    };
                    push_merge_line(scratch.arena, &lst_merge_B, line_b);
                    // Now we add the A candidate.
                    push_merge_line(scratch2.arena, &merged_lst_input.A, line_a);
                }
                break;
            case EditType::Ins:
                {
                    // Add insert from B.
                    LineRange rng_b = text_file_line_range(*b, Editor::CursorLine(e->edit.idx_b));
                    MergedLine line_b = {
                        .first = rng_b.first,
                        .last = rng_b.last,
                        .v_line = lst_B.count,
                        .line = Editor::CursorLine(e->edit.idx_b),
                        .type = EditType::Ins,
                    };
                    // Try to pull from the merged list.  If we have one, we don't need to add
                    // a sentinel to the A side.
                    MergedLineNode* node = lst_merge_B.first;
                    if (node == nullptr)
                    {
                        node = push_merge_line(scratch.arena, &lst_B, line_b);
                        MergedLine line_a = {
                            .first = Editor::CharOffset::Sentinel,
                            .last = Editor::CharOffset::Sentinel,
                            .v_line = lst_A.count,
                            .line = Editor::CursorLine::Beginning,
                            .type = EditType::Invalid,
                        };
                        push_merge_line(scratch.arena, &lst_A, line_a);
                    }
                    // We already have an entry for A.
                    else
                    {
                        node->line = line_b;
                        SLLQueuePop(lst_merge_B.first, lst_merge_B.last);
                        node->next = nullptr;
                        --lst_merge_B.count;
                        SLLQueuePush(lst_B.first, lst_B.last, node);
                        ++lst_B.count;
                    }
                    // Add the B candidate.
                    push_merge_line(scratch2.arena, &merged_lst_input.B, line_b);
                }
                break;
            case EditType::Eq:
                {
                    // If there are any entries on the merge list, we need to add them now as gap entries.
                    MergedLineNode* node = lst_merge_B.first;
                    while (node != nullptr)
                    {
                        // Add gap from B.
                        MergedLine line_b = {
                            .first = Editor::CharOffset::Sentinel,
                            .last = Editor::CharOffset::Sentinel,
                            .v_line = lst_B.count,
                            .line = Editor::CursorLine::Beginning,
                            .type = EditType::Invalid,
                        };
                        // Insert B.
                        node->line = line_b;
                        SLLQueuePop(lst_merge_B.first, lst_merge_B.last);
                        node->next = nullptr;
                        --lst_merge_B.count;
                        SLLQueuePush(lst_B.first, lst_B.last, node);
                        ++lst_B.count;

                        // Move node forward.
                        node = lst_merge_B.first;
                    }
                    // Insert on both sides.
                    LineRange rng_b = text_file_line_range(*b, Editor::CursorLine(e->edit.idx_b));
                    LineRange rng_a = text_file_line_range(*a, Editor::CursorLine(e->edit.idx_a));
                    MergedLine line_a = {
                        .first = rng_a.first,
                        .last = rng_a.last,
                        .v_line = lst_A.count,
                        .line = Editor::CursorLine(e->edit.idx_a),
                        .type = EditType::Eq,
                    };
                    MergedLine line_b = {
                        .first = rng_b.first,
                        .last = rng_b.last,
                        .v_line = lst_B.count,
                        .line = Editor::CursorLine(e->edit.idx_b),
                        .type = EditType::Eq,
                    };
                    push_merge_line(scratch.arena, &lst_A, line_a);
                    push_merge_line(scratch.arena, &lst_B, line_b);
                    assert(lst_A.count == lst_B.count);
                    // Try to pull candidates and create a merge block.
                    populate_merged_text_list(scratch2.arena, merged_lst_input);
                    merged_lst_input.A = {};
                    merged_lst_input.B = {};
                    // Clear the temporary arena as well.
                    Arena::pop_to(scratch2.arena, scratch2.pos);
                }
                break;
            case EditType::Invalid:
                break;
            }
        }
        // Perform one final populate just in case the files were completely different.
        populate_merged_text_list(scratch2.arena, merged_lst_input);

        populate_line_diff(panel->A.view, lst_A);
        populate_line_diff(panel->B.view, lst_B);
        populate_text_blocks_diff(panel->A.view, merged_txt_A);
        populate_text_blocks_diff(panel->B.view, merged_txt_B);
        Arena::scratch_end(scratch2);
        sw.stop();
        String8 msg = str8_fmt(scratch.arena, "Diff computed in %ums", sw.to_ms());
        feed->queue_info(msg);
        Arena::scratch_end(scratch);

        // Finally, apply our context window.
        apply_context_window(panel->A.view);
        apply_context_window(panel->B.view);
    }

    void sync_config(DiffPanel* panel)
    {
        apply_context_window(panel->A.view);
        apply_context_window(panel->B.view);
    }

    // Building.
    DiffPanelResponse build_diff_panel(DiffPanel* panel,
                                        CmdBuffer::CmdList* cmd_lst,
                                        CmdBuffer::DrawList* core_lst,
                                        UI::UIState* state,
                                        Feed::MessageFeed* feed)
    {
        PROF_SCOPE();

        DiffPanelResponse resp = {};
        auto clip = CmdBuffer::current_clip(*core_lst);
        const auto& colors = Config::widget_colors();

        // Start the frame for the enclosing editor frame.
        CmdBuffer::new_frame(panel->frame_lst, core_lst->screen, { .dt = core_lst->delta_time, .app_time = core_lst->app_time });
        // Default clip rect for the screen.
        CmdBuffer::push_clip(panel->frame_lst, clip);
        // Default texture (atlas by default).
        CmdBuffer::push_texture(panel->frame_lst, panel->atlas->atlas_texture());
        // Default palette.
        CmdBuffer::push_color_palette(panel->frame_lst, *CmdBuffer::current_palette(*core_lst));

        // Build panel decoration UI.
        {
            CmdBuffer::ClipRect header_clip = clip;
            Glyph::FontSize font_size = Glyph::FontSize{ Config::diff_state().diff_font_size };
            auto font_ctx = panel->atlas->render_font_context(font_size);
            header_clip.height = Height(UI::standard_font_padding(font_size) * 2);
            Vec2f base_pos;
            base_pos.y = static_cast<float>(rep(clip.height));
            // Add function buttons.
            {
                UI::Widgets::ID btn_ids[] = 
                {
                    UI::Widgets::make_id_seed(panel->id, "swap-diffs"),
                    UI::Widgets::make_id_seed(panel->id, "ctx-wnd-dwn"),
                    UI::Widgets::make_id_seed(panel->id, "ctx-wnd-val"),
                    UI::Widgets::make_id_seed(panel->id, "ctx-wnd-up"),
                };

                constexpr String8View btn_tips[] =
                {
                    str8_literal("Swap files"),
                    str8_literal("Lower context window"),
                    str8_literal("Context window"),
                    str8_literal("Expand context window"),
                };

                UI::Widgets::BuildIconicButtonInput btns[] =
                {
                    {
                        .id = btn_ids[rep(PanelButtons::SwapDiffs)],
                        .icon = Glyph::SpecialGlyph::Replace,
                        .padding = { PartitionPanel::padding },
                        .thickness = PartitionPanel::padding,
                    },
                    {
                        .id = btn_ids[rep(PanelButtons::LowerCtxWindow)],
                        .icon = Glyph::SpecialGlyph::Minus,
                        .padding = { PartitionPanel::padding },
                        .thickness = PartitionPanel::padding,
                    },
                    {
                        .id = btn_ids[rep(PanelButtons::CtxWindow)],
                        .icon = Glyph::SpecialGlyph::Trash,
                        .padding = { PartitionPanel::padding },
                        .thickness = PartitionPanel::padding,
                    },
                    {
                        .id = btn_ids[rep(PanelButtons::ExpandCtxWindow)],
                        .icon = Glyph::SpecialGlyph::Plus,
                        .padding = { PartitionPanel::padding },
                        .thickness = PartitionPanel::padding,
                    },
                };
                static_assert(std::size(btn_ids) == count_of<PanelButtons>);
                static_assert(std::size(btn_tips) == count_of<PanelButtons>);
                static_assert(std::size(btns) == count_of<PanelButtons>);

                auto scratch = Arena::scratch_begin(Arena::no_conflicts);
                // Let's just hack in the context window button a bit.  The ID and position
                // will stay the same but we need to generate a nice looking label for it.
                int ctx_window = Config::diff_state().context_window;
                const char* fmt_str[] =
                {
                    "%d",      // ctx_window >= 0
                    "Infinite" // ctx_window < 0
                };
                String8 ctx_window_txt = str8_fmt(scratch.arena, fmt_str[ctx_window < 0], ctx_window);
                UI::Widgets::BuildButtonInput ctx_wnd_btn_in =
                {
                    .id = btns[rep(PanelButtons::CtxWindow)].id,
                    .label = sv_str8(ctx_window_txt),
                    .padding = btns[rep(PanelButtons::CtxWindow)].padding,
                    .thickness = btns[rep(PanelButtons::CtxWindow)].thickness,
                };

                // Find out the total width of the buttons.
                float btn_width = 0.f;
                Vec2f ideal_btn;
                for EachIndex(i, count_of<PanelButtons>)
                {
                    if (i == rep(PanelButtons::CtxWindow))
                    {
                        auto btn_size = UI::Widgets::measure_button(&font_ctx, ctx_wnd_btn_in);
                        btn_width += btn_size.x;
                        // Note: Does not contribute to ideal button sizes.
                    }
                    else
                    {
                        auto btn_size = UI::Widgets::measure_iconic_button(&font_ctx, btns[i]);
                        btn_width += btn_size.x + PartitionPanel::padding;
                        ideal_btn.x = std::max(btn_size.x, ideal_btn.x);
                        ideal_btn.y = std::max(btn_size.y, ideal_btn.y);
                    }
                }
                // Center.
                Vec2f btn_pos = base_pos;
                btn_pos.x = (rep(header_clip.width) - btn_width) / 2.f;
                btn_pos.y -= UI::standard_font_padding(font_size);
                // Layout.
                for EachIndex(i, count_of<PanelButtons>)
                {
                    btns[i].pos = btn_pos;
                    btns[i].forced_size = ideal_btn;
                    bool clicked = false;
                    if (i == rep(PanelButtons::CtxWindow))
                    {
                        ctx_wnd_btn_in.pos = btn_pos;
                        auto btn_resp = UI::Widgets::basic_button(panel->frame_lst, state, &font_ctx, ctx_wnd_btn_in, UI::Widgets::BuildButtonFlags::None);
                        btn_pos.x += btn_resp.btn_size.x;
                    }
                    else
                    {
                        auto btn_resp = UI::Widgets::basic_iconic_button(panel->frame_lst, state, &font_ctx, btns[i], UI::Widgets::BuildButtonFlags::None);
                        clicked = btn_resp.clicked;
                        btn_pos.x += btn_resp.btn_size.x;
                    }
                    // Place tooltips.
                    if (not state->tooltip.enabled and UI::hot_widget_set(*state, btn_ids[i]))
                    {
                        UI::Widgets::TextTooltipInput tip_in = {
                            .text = sv_str8(str8_mut(btn_tips[i])),
                            .padding = PartitionPanel::padding,
                            .screen_pos = state->mouse.ui_mouse
                        };
                        UI::Widgets::basic_text_tooltip(state, core_lst, &font_ctx, tip_in);
                    }
                    // Process click.
                    if (clicked)
                    {
                        switch (PanelButtons(i))
                        {
                        case PanelButtons::SwapDiffs:
                            {
                                // Copy the files out to the scratch, then repopulate them in the opposite order.
                                TextFile new_B = text_file_copy_to(scratch.arena, *text_file(panel->A.view));
                                TextFile new_A = text_file_copy_to(scratch.arena, *text_file(panel->B.view));
                                file_A(panel, new_A);
                                file_B(panel, new_B);
                                // Free up the scratch arena so we can use it to populate diffs.
                                apply_diff(panel, feed);
                            }
                            break;
                        case PanelButtons::LowerCtxWindow:
                            {
                                auto cfg = Config::diff_state();
                                // We only need, at most, a -1 to provide the infinite context behavior.
                                cfg.context_window = std::max(-1, cfg.context_window - 1);
                                Config::update(cfg);
                                resp.updated_ctx_window = true;
                            }
                            break;
                        case PanelButtons::ExpandCtxWindow:
                            {
                                auto cfg = Config::diff_state();
                                // We only need, at most, a -1 to provide the infinite context behavior.
                                cfg.context_window += 1;
                                Config::update(cfg);
                                resp.updated_ctx_window = true;
                            }
                            break;
                        }
                    }
                }
                Arena::scratch_end(scratch);
                // Move down for next row.
                base_pos.y -= UI::standard_font_padding(font_size);
            }
            // Create titles for each panel and center them.
            for (PartitionPanel* child = &panel->A;
                not null_panel(child);
                child = child->sib_next)
            {
                CmdBuffer::ClipRect child_clip = clip_from_parent(clip, &panel->A, child);
                const TextFile* file = text_file(child->view);
                String8 name = file->path;
                CmdBuffer::start_glyph_run(panel->frame_lst, Render::VertShader::OneOneTransform);
                Vec2f pos = base_pos;
                pos.y -= font_ctx.current_font_line_height();
                pos.x = rep(child_clip.offset_x) + (rep(child_clip.width) - font_ctx.measure_text(name).x) / 2.f;
                font_ctx.render_text(panel->frame_lst, name, pos, colors.window_title_font_color);
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
            const float boundary_width_bias = Config::diff_state().diff_font_size / 3.f;
            for (PartitionPanel* child = &panel->A;
                // Non-leaf UI does only involves inner-panels (e.g. the fence post problem).
                not null_panel(child) and not null_panel(child->sib_next);
                child = child->sib_next)
            {
                PartitionPanel* sib = child->sib_next;
                CmdBuffer::ClipRect child_clip = clip_from_parent(clip, &panel->A, child);
                CmdBuffer::ClipRect sib_clip = clip_from_parent(clip, &panel->A, sib);
                CmdBuffer::ClipRect boundary_clip = {};

                Vec4f panelv_clip = clip_as_vec(clip);
                {
                    Vec4f childv_clip = clip_as_vec(child_clip);
                    Vec4f sibv_clip = clip_as_vec(sib_clip);
                    Vec4f boundaryv_clip{};
                    boundaryv_clip.p0[0] = childv_clip.p1[0] - PartitionPanel::padding;
                    boundaryv_clip.p1[0] = sibv_clip.p0[0] + PartitionPanel::padding;
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
                    CmdBuffer::solid_rect(panel->frame_lst, Render::FragShader::BasicColor, pos, size, region_color);
                    change_cursor(state, UI::CursorStyle::LeftRightArrow);
                }
            }
        }

        // Note: This relies on only having the two panels.
        DiffTextView* scroll_changed[] = {
            nullptr,
            nullptr
        };
        uint64_t scroll_idx = 0;

        // Build leaf-UI.
        for (PartitionPanel* child = &panel->A;
            not null_panel(child);
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
            auto r = build_diff_text_view(child->view, child->draw_lst, state);
            scroll_changed[scroll_idx++] = r.scroll_changed ? child->view : nullptr;

            CmdBuffer::pop_clip(child->draw_lst);
            CmdBuffer::pop_texture(child->draw_lst);
            CmdBuffer::pop_color_palette(child->draw_lst);

            CmdBuffer::push_draw_list(cmd_lst, child->draw_lst);
        }

        for EachIndex(i, std::size(scroll_changed))
        {
            DiffTextView* view = scroll_changed[i];
            if (view != nullptr)
            {
                DiffTextView* share_to = panel->A.view;
                if (view == panel->A.view)
                {
                    share_to = panel->B.view;
                }
                share_scroll_pos(share_to, view);
            }
        }

        CmdBuffer::pop_clip(panel->frame_lst);
        CmdBuffer::pop_texture(panel->frame_lst);
        CmdBuffer::pop_color_palette(panel->frame_lst);

        CmdBuffer::push_draw_list(cmd_lst, panel->frame_lst);
        return resp;
    }
} // namespace Diff