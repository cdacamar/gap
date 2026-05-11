#include "diff-text.h"

#include <cassert>

#include "arena.h"
#include "basic-scrollbox.h"

namespace Diff
{
    struct DiffTextView
    {
        Arena::Arena* arena;
        Arena::Arena* diff_arena;
        Arena::Arena* fine_diff_arena;
        Arena::Arena* ctx_diff_arena;
        Arena::Position base_pos;
        TextFile text;
        MergedDiffView full_diffs;
        MergedTextBlocks full_diff_blocks;
        // These two are reduced snapshots of the structures above based on
        // needed context window.
        MergedDiffView diffs;
        MergedTextBlocks diff_blocks;
        uint64_t longest_line;
        uint64_t max_line;
        int64_t idx_page_jump;
        int64_t idx_mid;
        UI::Widgets::ID id;
        UI::Widgets::IndexedScrollBox* scroll;
        Glyph::Atlas* atlas;
    };

    namespace
    {
        UI::Widgets::IndexedScrollContentSize content_size(DiffTextView* widget, Glyph::RenderFontContext* font_ctx)
        {
            UI::Widgets::IndexedScrollContentSize size{};
            const int line_height = font_ctx->current_font_line_height();
            const float glyph_width_est = font_ctx->measure_text("H").x;
            // Prefer v-side of diffs if available.
            size.v_size = widget->diffs.size != 0 ? widget->diffs.size : widget->text.line_starts.size;
            size.entry_size.y = static_cast<float>(line_height);
            size.entry_size.x = glyph_width_est * widget->longest_line;

            // If line numbers are enabled, factor in the largest number.
            if (Config::diff_state().show_line_numbers)
            {
                uint64_t num_digits = digits(size.v_size);
                // +1 more for the '.' and one more for padding.
                size.entry_size.x += num_digits * glyph_width_est + glyph_width_est + glyph_width_est;
            }

            size.entry_size.x += glyph_width_est;

            return size;
        }

        struct MergedTextBlocksView
        {
            MergedText* first;
            MergedText* last;
        };

        // Note: This uses visual lines.
        MergedTextBlocksView merged_text_blocks_for_view(MergedTextBlocks* blocks, uint64_t first_l, uint64_t last_l)
        {
            MergedTextBlocksView result = {};
            // By construction, this list is sorted by line, so we can perform a lower bound
            // binary search to find the nearest one.
            if (blocks->size == 0)
                return result;
            // We can binary search for the line, but we need the lower bound to capture the first instance of
            // first_l.
            uint64_t low = 0;
            uint64_t high = blocks->size;
            uint64_t mid = 0;
            while (low < high)
            {
                mid = low + ((high - low) / 2);
                if (first_l <= blocks->blocks[mid].v_line)
                {
                    high = mid;
                }
                else
                {
                    low = mid + 1;
                }
            }
            // Finding the upperbound can be a simple linear search from here since since
            // the number of blocks should be relatively small.
            result.first = &blocks->blocks[low];
            result.last = blocks->blocks + blocks->size;
            // Note: We start at 'low' to catch the case when v_line is actually > than first_l
            // (which would result in an empty range, as expected).
            for (uint64_t i = low; i < blocks->size; ++i)
            {
                if (blocks->blocks[i].v_line > last_l)
                {
                    result.last = &blocks->blocks[i];
                    break;
                }
            }
            return result;
        }

        // This is a non-atomic version of the circular queue.
        struct CircularDiffWindow
        {
            uint64_t* idx_buf;
            uint64_t cap;
            uint64_t read;
            uint64_t write;
        };

        CircularDiffWindow make_circular_window(Arena::Arena* arena, uint64_t cap_hint)
        {
            CircularDiffWindow result = {};
            // This needs to be a power of 2 for index masking later.
            uint64_t pow_2_aligned_size = up_pow2(cap_hint);
            result.cap = pow_2_aligned_size;
            result.idx_buf = Arena::push_array<uint64_t>(arena, result.cap);
            result.read = result.write = 0;
            return result;
        }

        uint64_t circular_window_mask(uint64_t idx, uint64_t cap)
        {
            return idx & (cap - 1);
        }

        void circular_window_reset(CircularDiffWindow* window)
        {
            window->read = window->write = 0;
        }

        uint64_t circular_window_size(const CircularDiffWindow& window)
        {
            return window.write - window.read;
        }

        void circular_window_push(CircularDiffWindow* window, uint64_t idx)
        {
            // Move the read head forward to chop the end if this insert would reach capacity.
            window->read += circular_window_size(*window) + 1 > window->cap;
            window->idx_buf[circular_window_mask(window->write, window->cap)] = idx;
            window->write += 1;
        }

        uint64_t circular_window_begin(const CircularDiffWindow& window)
        {
            return window.read;
        }

        uint64_t circular_window_end(const CircularDiffWindow& window)
        {
            return window.write;
        }

        uint64_t circular_window_fetch(const CircularDiffWindow& window, uint64_t idx)
        {
            return window.idx_buf[circular_window_mask(idx, window.cap)];
        }
    } // namespace [anon]

    // Creation.
    DiffTextView* make_diff_text_view(Glyph::Atlas* atlas, UI::Widgets::ID id)
    {
        Arena::Arena* arena = Arena::alloc(Arena::default_params);
        DiffTextView* widget = Arena::push_array<DiffTextView>(arena, 1);
        widget->arena = arena;
        widget->diff_arena = Arena::alloc(Arena::default_params);
        widget->fine_diff_arena = Arena::alloc(Arena::default_params);
        widget->ctx_diff_arena = Arena::alloc(Arena::default_params);
        // We need to do gross C++ here.
        {
            uint8_t* blob = Arena::push_array_no_zero_aligned<uint8_t>(arena,
                                                                sizeof(UI::Widgets::IndexedScrollBox),
                                                                Arena::Alignment{ alignof(UI::Widgets::IndexedScrollBox) });
            widget->scroll = new(blob) UI::Widgets::IndexedScrollBox{ id };
            widget->scroll->scroll_to({});
        }
        widget->id = id;
        widget->base_pos = Arena::pos(arena);
        widget->atlas = atlas;
        return widget;
    }

    // Cleanup.
    void release_diff_text_view(DiffTextView* widget)
    {
        // Destroy C++ object.
        using SBox = UI::Widgets::IndexedScrollBox;
        widget->scroll->~SBox();

        Arena::release(widget->diff_arena);
        Arena::release(widget->fine_diff_arena);
        Arena::release(widget->ctx_diff_arena);
        Arena::Arena* arena = widget->arena;
        Arena::release(arena);
    }

    // Queries.
    TextFile* diff_text_view_text_file(DiffTextView* widget)
    {
        return &widget->text;
    }

    // Interaction.
    void diff_text_view_sync_config(DiffTextView* widget)
    {
        widget->scroll->sync_config();
    }

    void diff_text_view_populate_text(DiffTextView* widget, const TextFile& text)
    {
        // If we populate text, we should remove the existing diffs as well.
        widget->full_diffs = {};
        widget->full_diff_blocks = {};
        widget->diffs = {};
        widget->diff_blocks = {};
        Arena::clear(widget->diff_arena);
        Arena::clear(widget->fine_diff_arena);
        Arena::clear(widget->ctx_diff_arena);
        // Copy the text file into our widget arena.
        Arena::pop_to(widget->arena, widget->base_pos);
        widget->text = text_file_copy_to(widget->arena, text);
        // Find the longest line.
        widget->longest_line = 0;
        widget->max_line = widget->text.line_starts.size;
        for EachIndex(l, widget->text.line_starts.size)
        {
            // Note: CursorLine is 1-indexed.
            String8 txt = text_file_line_text(widget->text, CursorLine{ l + 1 });
            widget->longest_line = std::max(widget->longest_line, txt.size);
        }
    }

    void diff_text_view_populate_line_diff(DiffTextView* widget, MergedLineList lst)
    {
        widget->full_diffs = {};
        Arena::clear(widget->diff_arena);
        widget->full_diffs = diff_text_view_join_merged_line_list(widget->diff_arena, lst);
        widget->diffs = widget->full_diffs;
    }

    void diff_text_view_populate_text_blocks_diff(DiffTextView* widget, MergedTextList lst)
    {
        widget->full_diff_blocks = {};
        Arena::clear(widget->fine_diff_arena);
        widget->full_diff_blocks = diff_text_view_join_merged_text_blocks_list(widget->fine_diff_arena, lst, widget->text);
        // Set these to be equivalent until we involve the context window.
        widget->diff_blocks = widget->full_diff_blocks;
    }

    void diff_text_view_share_scroll_pos(DiffTextView* widget, const DiffTextView* share_from)
    {
        UI::Widgets::IndexedScrollOffset off = share_from->scroll->position_no_offset();
        UI::Widgets::IndexedScrollContentSize size_target = widget->scroll->content_size();
        off.offset.x = std::min(off.offset.x, size_target.entry_size.x);
        widget->scroll->scroll_to(off);
    }

    void diff_text_view_reset_scroll_pos(DiffTextView* widget)
    {
        widget->scroll->scroll_to({});
    }

    void diff_text_view_apply_context_window(DiffTextView* widget)
    {
        // Note: This is not the best way to do this.  Perhaps there's an alternative data structure
        // to be used here vs rebuilding the view array.
        Arena::clear(widget->ctx_diff_arena);
        int context = Config::diff_state().context_window;
        // Negative context window implies an infinitely wide window (no filtering).
        if (context < 0)
        {
            widget->diffs = widget->full_diffs;
            widget->diff_blocks = widget->full_diff_blocks;
            return;
        }
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        MergedLineList trimmed_lines = {};
        // These will stay the same, but we will end up mutating line offsets in here, which is
        // why we need to copy it.
        widget->diff_blocks.size = widget->full_diff_blocks.size;
        widget->diff_blocks.blocks = Arena::push_array_no_zero<MergedText>(widget->ctx_diff_arena, widget->diff_blocks.size);
        memcpy(widget->diff_blocks.blocks, widget->full_diff_blocks.blocks, sizeof(MergedText) * widget->diff_blocks.size);
        // Our goal is to create 'context' lines before and after each diff line.  These context
        // lines correspond to EditType::Eq.
        // We need this because context windows can overlap.  To account for this, we make our circular window
        // twice as large which catches the cases where this overlap can happen between two diff regions.
        uint64_t context2 = context * 2;
        CircularDiffWindow cw = make_circular_window(scratch.arena, context2);
        constexpr uint64_t block_sentinel = uint64_t(-1);
        uint64_t prev_block = block_sentinel;
        for EachIndex(i, widget->full_diffs.size)
        {
            const MergedLine* line = &widget->full_diffs.lines[i];
            uint64_t cw_size = circular_window_size(cw);
            bool in_block = line->type != EditType::Eq;
            if (not in_block)
            {
                circular_window_push(&cw, i);
            }
            else if (cw_size != 0)
            {
                uint64_t blk_dist = prev_block != block_sentinel
                                    ? i - prev_block - 1
                                    : i + 1;
                // We don't need to trim, just take everything.
                if (blk_dist <= context2)
                {
                    uint64_t first = circular_window_begin(cw);
                    uint64_t last = circular_window_end(cw);
                    for (;first != last; ++first)
                    {
                        MergedLine l = widget->full_diffs.lines[circular_window_fetch(cw, first)];
                        l.v_line = trimmed_lines.count;
                        assert(l.type == EditType::Eq);
                        diff_text_view_push_merge_line(scratch.arena, &trimmed_lines, l);
                    }
                }
                else
                {
                    // Take indices from the previous block first.
                    // This only happens once but... meh.
                    if (prev_block != block_sentinel)
                    {
                        for (uint64_t ctx_blk = prev_block + 1; ctx_blk <= prev_block + context; ++ctx_blk)
                        {
                            MergedLine l = widget->full_diffs.lines[ctx_blk];
                            l.v_line = trimmed_lines.count;
                            assert(l.type == EditType::Eq);
                            diff_text_view_push_merge_line(scratch.arena, &trimmed_lines, l);
                        }
                    }
                    // Add a separator line which tell us this was a skip.
                    {
                        MergedLine sep = {
                            .first = CharOffset::Sentinel,
                            .last = CharOffset::Sentinel,
                            .v_line = trimmed_lines.count,
                            .line = CursorLine::Beginning,
                            .type = EditType::Skip,
                        };
                        diff_text_view_push_merge_line(scratch.arena, &trimmed_lines, sep);
                    }
                    uint64_t last = circular_window_end(cw);
                    // We want to start from the slot where the context is above our lines.
                    uint64_t first = last - uint64_t(context);
                    for (;first != last; ++first)
                    {
                        MergedLine l = widget->full_diffs.lines[circular_window_fetch(cw, first)];
                        l.v_line = trimmed_lines.count;
                        assert(l.type == EditType::Eq);
                        diff_text_view_push_merge_line(scratch.arena, &trimmed_lines, l);
                    }
                }
                // Clear the circular buffer.
                circular_window_reset(&cw);
                prev_block = i;
            }
            else
            {
                prev_block = i;
            }

            if (in_block)
            {
                MergedLine l = *line;
                l.v_line = trimmed_lines.count;
                diff_text_view_push_merge_line(scratch.arena, &trimmed_lines, l);
                // Now we need to adjust any blocks associated with this line.
                // There's a nice invariant here.  Since we're only removing lines, the v_line will
                // only ever get smaller, which means that our binary search for visual lines in the
                // new buffer will still work.  Let's use that and then adjust the v_line to match
                // that of the line we just inserted above.
                MergedTextBlocksView merged_view = merged_text_blocks_for_view(&widget->diff_blocks, line->v_line, line->v_line);
                for (; merged_view.first != merged_view.last; ++merged_view.first)
                {
                    merged_view.first->v_line = l.v_line;
                }
            }
        }

        // A cleanup step to add more context to the bottom.
        if (prev_block != block_sentinel)
        {
            for (uint64_t ctx_blk = prev_block + 1; ctx_blk <= prev_block + context and ctx_blk < widget->full_diffs.size; ++ctx_blk)
            {
                MergedLine l = widget->full_diffs.lines[ctx_blk];
                l.v_line = trimmed_lines.count;
                assert(l.type == EditType::Eq);
                diff_text_view_push_merge_line(scratch.arena, &trimmed_lines, l);
            }
        }
        // Collapse the list into an array.
        widget->diffs = diff_text_view_join_merged_line_list(widget->ctx_diff_arena, trimmed_lines);
        Arena::scratch_end(scratch);
    }

    void diff_text_view_sink_cached_diffs(DiffTextView* widget, const TextFile& text, MergedDiffView diff_lines, MergedTextBlocks diff_text_blocks)
    {
        // Just copy everything.
        diff_text_view_populate_text(widget, text);
        // Note: the above already clears all our arenas.
        // Lines.
        widget->full_diffs.size = diff_lines.size;
        widget->full_diffs.lines = Arena::push_array_no_zero<MergedLine>(widget->diff_arena, widget->full_diffs.size);
        memcpy(widget->full_diffs.lines, diff_lines.lines, sizeof(MergedLine) * diff_lines.size);
        // Wait until context window is applied.
        widget->diffs = widget->full_diffs;
        // Blocks.
        widget->full_diff_blocks.size = diff_text_blocks.size;
        widget->full_diff_blocks.blocks = Arena::push_array_no_zero<MergedText>(widget->fine_diff_arena, widget->full_diff_blocks.size);
        memcpy(widget->full_diff_blocks.blocks, diff_text_blocks.blocks, sizeof(MergedText) * diff_text_blocks.size);
        widget->diff_blocks = widget->full_diff_blocks;
    }

    // Helpers.
    MergedLineNode* diff_text_view_push_merge_line(Arena::Arena* arena, MergedLineList* lst, MergedLine line)
    {
        MergedLineNode* node = Arena::push_array<MergedLineNode>(arena, 1);
        node->line = line;
        SLLQueuePush(lst->first, lst->last, node);
        ++lst->count;
        return node;
    }

    MergedTextNode* diff_text_view_push_merged_text(Arena::Arena* arena, MergedTextList* lst, MergedText merged)
    {
        MergedTextNode* node = Arena::push_array<MergedTextNode>(arena, 1);
        node->merged = merged;
        SLLQueuePush(lst->first, lst->last, node);
        ++lst->count;
        return node;
    }

    MergedDiffView diff_text_view_join_merged_line_list(Arena::Arena* arena, MergedLineList lst)
    {
        MergedDiffView result = {};
        result.size = lst.count;
        result.lines = Arena::push_array_no_zero<MergedLine>(arena, result.size);
        uint64_t idx = 0;
        for EachNode(n, lst.first)
        {
            result.lines[idx++] = n->line;
        }
        return result;
    }

    MergedTextBlocks diff_text_view_join_merged_text_blocks_list(Arena::Arena* arena, MergedTextList lst, const TextFile& file)
    {
        MergedTextBlocks result = {};
        result.size = lst.count;
        result.blocks = Arena::push_array_no_zero<MergedText>(arena, result.size);
        uint64_t idx = 0;
        CursorLine prev_line = {};
        for EachNode(n, lst.first)
        {
            MergedText* merged = &result.blocks[idx++];
            *merged = n->merged;
            // Compute the line.
            // We save a lot of time by precomputing this because line lookup by offset is more expensive,
            // a O(lg n) operation vs an O(1) if we know the line.
            merged->line = text_file_line_for_offset(file, merged->first);
            assert(prev_line <= merged->line);
            prev_line = merged->line;
        }
        return result;
    }

    // Building.
    DiffTextViewResponse build_diff_text_view(DiffTextView* widget,
                                                CmdBuffer::DrawList* lst,
                                                UI::UIState* state)
    {
        DiffTextViewResponse resp = {};
        CmdBuffer::ClipRect clip = CmdBuffer::current_clip(*lst);
        const Config::DiffColors& colors = Config::diff_colors();
        Glyph::RenderFontContext font_ctx = widget->atlas->render_font_context(Glyph::FontSize{ Config::diff_state().diff_font_size });
        UI::Widgets::IndexedScrollContentSize scroll_size = content_size(widget, &font_ctx);
        const int line_height = font_ctx.current_font_line_height();
        // Process input.
        if (UI::empty_focus_widget(*state) and mouse_in_clip(state->mouse.ui_mouse, clip))
        {
            if (hotkey(*state, Hotkey::GLB_TextLineDown))
            {
                UI::Widgets::IndexedScrollOffset off = widget->scroll->position();
                off.offset.y = 0.f;
                off.idx = std::min(off.idx + 1, scroll_size.v_size - 1);
                widget->scroll->scroll_to(off);
                resp.scroll_changed = true;
            }

            if (hotkey(*state, Hotkey::GLB_TextLineUp))
            {
                UI::Widgets::IndexedScrollOffset off = widget->scroll->position();
                off.offset.y = 0.f;
                off.idx = std::max(off.idx - 1, int64_t(0));
                widget->scroll->scroll_to(off);
                resp.scroll_changed = true;
            }

            if (hotkey(*state, Hotkey::GLB_TextPageDown))
            {
                UI::Widgets::IndexedScrollOffset off = widget->scroll->position();
                off.offset.y = 0.f;
                off.idx = std::min(off.idx + widget->idx_page_jump, scroll_size.v_size - 1);
                widget->scroll->scroll_to(off);
                resp.scroll_changed = true;
            }

            if (hotkey(*state, Hotkey::GLB_TextPageUp))
            {
                UI::Widgets::IndexedScrollOffset off = widget->scroll->position();
                off.offset.y = 0.f;
                off.idx = std::max(off.idx - widget->idx_page_jump, int64_t(0));
                widget->scroll->scroll_to(off);
                resp.scroll_changed = true;
            }

            if (hotkey(*state, Hotkey::GLB_TextBeginning))
            {
                UI::Widgets::IndexedScrollOffset off = {};
                widget->scroll->scroll_to(off);
                resp.scroll_changed = true;
            }

            if (hotkey(*state, Hotkey::GLB_TextEnd))
            {
                UI::Widgets::IndexedScrollOffset off = {};
                off.idx = scroll_size.v_size - 1;
                widget->scroll->scroll_to(off);
                resp.scroll_changed = true;
            }
        }
        font_ctx.render_whitespace(Config::system_effects().render_whitespace);
        font_ctx.whitespace_color(colors.whitespace);

        const float wheel_offset_amt = UI::standard_font_padding(Glyph::FontSize{ font_ctx.current_font_size() }) * 2.f;
        const float glyph_width_est = font_ctx.measure_text("H").x;

        // Setup the scroll widget.
        CmdBuffer::ClipRect content_clip = UI::convert(widget->scroll->content_viewport(UI::convert(clip)));
        {
            // Constrain the 'x' size here so we don't get a phantom horizontal scrollbar.
            scroll_size.entry_size.x = std::clamp(scroll_size.entry_size.x - rep(content_clip.width), 0.f, scroll_size.entry_size.x);
            widget->scroll->content_size(scroll_size);
            auto r = widget->scroll->build(lst, state, wheel_offset_amt, UI::Widgets::BuildScrollBoxFlags::None);
            resp.scroll_changed |= r.scroll_changed;
            if (resp.scroll_changed)
            {
                UI::try_set_focus_widget(state, widget->id);
            }
        }
        CmdBuffer::push_clip(lst, content_clip);

        // Find the line ranges.
        UI::Widgets::IndexedScrollOffset off = widget->scroll->position();
        const int lines_per_v = rep(content_clip.height) / line_height;
        Vec2f start_pos;
        // Note: X-offset needs to pull text to left of viewport.
        start_pos.x = -off.offset.x;
        start_pos.y = rep(content_clip.height) + off.offset.y - font_ctx.current_font_size();
        // Let's also cache the page jump amount.
        widget->idx_page_jump = static_cast<uint64_t>(lines_per_v * .75f);
        widget->idx_mid = (off.idx + lines_per_v) / 2;

        // Core text.
        if (widget->diffs.size == 0)
        {
            // Note: the scrollbox starts at offset 0, but CursorLine is 1-indexed.
            CursorLine first = CursorLine(off.idx + 1);
            CursorLine last = CursorLine{ rep(first) + (off.offset.y > 0.f) + lines_per_v };
            last = std::clamp(last, first, CursorLine{ widget->text.line_starts.size });

            CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
            for (; first <= last; first = extend(first))
            {
                String8 txt = text_file_line_text(widget->text, first);
                font_ctx.render_text(lst, txt, start_pos, colors.eq_txt);
                start_pos.y -= line_height;
            }
        }
        else
        {
            // Note: the scrollbox starts at offset 0 and so is the diff lines.
            uint64_t first = uint64_t(off.idx);
            uint64_t last = first + (off.offset.y > 0.f) + lines_per_v;
            last = std::clamp(last, first, widget->diffs.size - 1);
            Vec4f colors_line_map[] =
            {
                colors.del_line,                // EditType::Del
                colors.ins_line,                // EditType::Ins
                colors.eq_line,                 // EditType::Eq
                colors.gap_line,                // EditType::Invalid
                colors.trimmed_text,            // EditType::Skip
            };

            Vec4f colors_txt_map[] =
            {
                colors.del_txt,                // EditType::Del
                colors.ins_txt,                // EditType::Ins
                colors.eq_txt,                 // EditType::Eq
                colors.gap_line,               // EditType::Invalid
                colors.trimmed_text,           // EditType::Skip
            };
            Vec4f color;

            // First, go through and add the boxes.
            CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
            Vec2f hl_pos = start_pos;
            // The line isn't going to be exactly centered without this slight offset.
            constexpr float line_hl_offset = 0.13f;
            hl_pos.y -= line_hl_offset * line_height;
            for (uint64_t hl_line = first; hl_line <= last; ++hl_line)
            {
                MergedLine l = widget->diffs.lines[hl_line];
                Vec2f size = { rep(content_clip.width) + off.offset.x, line_height + 0.f };
                color = colors_line_map[rep(l.type)];
                // Don't highlight equal lines.
                if (l.type != EditType::Eq)
                {
                    CmdBuffer::solid_rect(lst, Render::FragShader::BasicColor, hl_pos, size, color);
                }
                hl_pos.y -= line_height;
            }

            // Render the line numbers now so it will offset all of the positions for highlights/text below.
            CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
            if (Config::diff_state().show_line_numbers)
            {
                const uint64_t max_digits = digits(widget->max_line);
                constexpr char target_string[] = "999999999.";
                char line_num_buf[std::size(target_string)];
                String8 line_num = fmt_string(line_num_buf, "%I64d.", widget->max_line);
                // Measure this because it will serve as the basis to left-align all the others.
                Vec2f size = font_ctx.measure_text(line_num);
                // Note: we add +1 to 'max_digits' here because we need to factor in the size of each mono-space
                // numeric glyph + the '.' character.
                const float padding_per_digit = size.x / (max_digits + 1);
                // We're going to create a lookup index so we can very quickly compute how the
                // line numbers should be adjusted.
                // Note: do we really have files over 999,999,999 lines?
                constexpr int max_line_number_digits = 9;
                float table[max_line_number_digits];
                for (int i = 0; i < max_line_number_digits; ++i)
                {
                    table[i] = (max_digits - (i + 1)) * (padding_per_digit);
                }
                Vec2f line_num_pos = start_pos;
                // We don't want to offset the line numbers horizontally the same way the text is.
                line_num_pos.x = 0.f;
                uint64_t first_line = first;
                uint64_t last_line = last;
                // Seed the first line position.
                Vec2f pos;
                for (; first_line <= last_line; ++first_line)
                {
                    // Pull the actual line number out, if there is one.
                    MergedLine line = widget->diffs.lines[first_line];
                    if (line.type != EditType::Invalid and line.type != EditType::Skip)
                    {
                        pos = line_num_pos;
                        pos.x += table[digits(rep(line.line)) - 1];
                        line_num = fmt_string(line_num_buf, "%I64d.", rep(line.line));
                        pos = font_ctx.render_text(lst, line_num, pos, colors.line_numbers);
                    }
                    line_num_pos.y -= line_height;
                }
                // Finally, offset the start pos for the text blow.
                // Replace the current clip so we can clip any text that tries to enter this region.
                CmdBuffer::pop_clip(lst);
                int off_width = static_cast<int>((max_digits + 2) * padding_per_digit);
                content_clip.offset_x = UI::offset_from(content_clip.offset_x, off_width);
                content_clip.width = Width{ rep(content_clip.width) - off_width };
                CmdBuffer::push_clip(lst, content_clip);
            }

            MergedTextBlocksView merged_blocks_view = merged_text_blocks_for_view(&widget->diff_blocks, first, last);
            // Iterate the more fine-diff highlights.
            CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
            for (; merged_blocks_view.first != merged_blocks_view.last; ++merged_blocks_view.first)
            {
                MergedText* merged = merged_blocks_view.first;
                // Pull the text line so we can figure out how far to offset this highlight.
                // Note: These lines need to be offset by 1 because they're indexed by 1 internally.
                LineRange line_rng = text_file_line_range(widget->text, merged->line);
                String8 line_txt = text_file_line_text(widget->text, merged->line);
                String8 before_txt = str8_substr(line_txt, { .len = rep(distance(line_rng.first, merged->first)) });
                String8 after_txt = str8_substr(line_txt, { .off = before_txt.size, .len = rep(distance(merged->first, merged->last)) });
                Vec2f size;
                size.x = font_ctx.measure_text(after_txt).x;
                size.y = line_height + 0.f;
                // Add glyph_width_est to skip the insertion/deletion designator.
                hl_pos.x = start_pos.x + glyph_width_est + font_ctx.measure_text(before_txt).x;
                hl_pos.y = start_pos.y - (static_cast<float>(merged->v_line - first) * line_height) - line_hl_offset * line_height;
                color = colors_txt_map[rep(merged->type)];
                CmdBuffer::solid_rect(lst, Render::FragShader::BasicColor, hl_pos, size, color);
            }

            CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
            for (; first <= last; ++first)
            {
                MergedLine l = widget->diffs.lines[first];
                String8 txt = str8_empty;
                Vec2f pos = start_pos;
                switch (l.type)
                {
                case EditType::Del:
                    pos = font_ctx.render_text(lst, "-", pos, colors.del_mark);
                    txt = str8_substr(widget->text.content, { .off = rep(l.first), .len = rep(distance(l.first, l.last)) });
                    break;
                case EditType::Ins:
                    pos = font_ctx.render_text(lst, "+", pos, colors.ins_mark);
                    txt = str8_substr(widget->text.content, { .off = rep(l.first), .len = rep(distance(l.first, l.last)) });
                    break;
                case EditType::Eq:
                    // Just shift the text past the marker.
                    pos.x += glyph_width_est;
                    txt = str8_substr(widget->text.content, { .off = rep(l.first), .len = rep(distance(l.first, l.last)) });
                    break;
                case EditType::Invalid:
                case EditType::Skip:
                    break;
                }
                font_ctx.render_text(lst, txt, pos, colors.eq_txt);
                start_pos.y -= line_height;
            }
        }
        CmdBuffer::pop_clip(lst);
        return resp;
    }
} // namespace Diff