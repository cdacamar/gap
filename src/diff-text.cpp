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
        Arena::Position base_pos;
        TextFile text;
        MergedDiffView diffs;
        MergedTextBlocks diff_blocks;
        uint64_t longest_line;
        int64_t idx_page_jump;
        int64_t idx_mid;
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
        MergedTextBlocksView merged_text_blocks_for_view(DiffTextView* widget, uint64_t first_l, uint64_t last_l)
        {
            MergedTextBlocksView result = {};
            // By construction, this list is sorted by line, so we can perform a lower bound
            // binary search to find the nearest one.
            if (widget->diff_blocks.size == 0)
                return result;
            // We can binary search for the line, but we need the lower bound to capture the first instance of
            // first_l.
            uint64_t low = 0;
            uint64_t high = widget->diff_blocks.size;
            uint64_t mid = 0;
            while (low < high)
            {
                mid = low + ((high - low) / 2);
                if (first_l <= widget->diff_blocks.blocks[mid].v_line)
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
            result.first = &widget->diff_blocks.blocks[low];
            result.last = widget->diff_blocks.blocks + widget->diff_blocks.size;
            for (uint64_t i = low + 1; i < widget->diff_blocks.size; ++i)
            {
                if (widget->diff_blocks.blocks[i].v_line > last_l)
                {
                    result.last = &widget->diff_blocks.blocks[i];
                    break;
                }
            }
            return result;

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
        // We need to do gross C++ here.
        {
            uint8_t* blob = Arena::push_array_no_zero_aligned<uint8_t>(arena,
                                                                sizeof(UI::Widgets::IndexedScrollBox),
                                                                Arena::Alignment{ alignof(UI::Widgets::IndexedScrollBox) });
            widget->scroll = new(blob) UI::Widgets::IndexedScrollBox{ id };
        }
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
        Arena::Arena* arena = widget->arena;
        Arena::release(arena);
    }

    // Queries.
    TextFile* text_file(DiffTextView* widget)
    {
        return &widget->text;
    }

    // Interaction.
    void populate_text(DiffTextView* widget, const TextFile& text)
    {
        // Copy the text file into our widget arena.
        Arena::pop_to(widget->arena, widget->base_pos);
        widget->text = text_file_copy_to(widget->arena, text);
        // Find the longest line.
        widget->longest_line = 0;
        for EachIndex(l, widget->text.line_starts.size)
        {
            // Note: CursorLine is 1-indexed.
            String8 txt = text_file_line_text(widget->text, Editor::CursorLine{ l + 1 });
            widget->longest_line = std::max(widget->longest_line, txt.size);
        }
        widget->scroll->scroll_to({});
    }

    void populate_line_diff(DiffTextView* widget, MergedLineList lst)
    {
        widget->diffs = {};
        Arena::clear(widget->diff_arena);
        widget->diffs.size = lst.count;
        widget->diffs.lines = Arena::push_array_no_zero<MergedLine>(widget->diff_arena, widget->diffs.size);
        uint64_t idx = 0;
        for EachNode(n, lst.first)
        {
            widget->diffs.lines[idx++] = n->line;
        }
    }

    void populate_text_blocks_diff(DiffTextView* widget, MergedTextList lst)
    {
        widget->diff_blocks = {};
        Arena::clear(widget->fine_diff_arena);
        widget->diff_blocks.size = lst.count;
        widget->diff_blocks.blocks = Arena::push_array_no_zero<MergedText>(widget->fine_diff_arena, widget->diff_blocks.size);
        uint64_t idx = 0;
        Editor::CursorLine prev_line = {};
        for EachNode(n, lst.first)
        {
            MergedText* merged = &widget->diff_blocks.blocks[idx++];
            *merged = n->merged;
            // Compute the line.
            // We save a lot of time by precomputing this because line lookup by offset is more expensive,
            // a O(lg n) operation vs an O(1) if we know the line.
            merged->line = text_file_line_for_offset(widget->text, merged->first);
            assert(prev_line <= merged->line);
            prev_line = merged->line;
        }
    }

    void share_scroll_pos(DiffTextView* widget, const DiffTextView* share_from)
    {
        UI::Widgets::IndexedScrollOffset off = share_from->scroll->position_no_offset();
        UI::Widgets::IndexedScrollContentSize size_target = widget->scroll->content_size();
        off.offset.x = std::min(off.offset.x, size_target.entry_size.x);
        widget->scroll->scroll_to(off);
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
        if (mouse_in_clip(state->mouse.ui_mouse, clip))
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
            Editor::CursorLine first = Editor::CursorLine(off.idx + 1);
            Editor::CursorLine last = Editor::CursorLine{ rep(first) + (off.offset.y > 0.f) + lines_per_v };
            last = std::clamp(last, first, Editor::CursorLine{ widget->text.line_starts.size });

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
            };

            Vec4f colors_txt_map[] =
            {
                colors.del_txt,                // EditType::Del
                colors.ins_txt,                // EditType::Ins
                colors.eq_txt,                 // EditType::Eq
                colors.gap_line,               // EditType::Invalid
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
                const uint64_t max_digits = digits(widget->diffs.size);
                constexpr char target_string[] = "999999999.";
                char line_num_buf[std::size(target_string)];
                String8 line_num = fmt_string(line_num_buf, "%I64d.", widget->diffs.size);
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
                    if (line.type != EditType::Invalid)
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

            CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
            MergedTextBlocksView merged_blocks_view = merged_text_blocks_for_view(widget, first, last);
            // Iterate the more fine-diff highlights.
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