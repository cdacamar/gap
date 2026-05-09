#include "diff-dir-list.h"

#include "arena.h"
#include "basic-scrollbox.h"

namespace Diff
{
    struct DiffDirListView
    {
        Arena::Arena* arena;
        Arena::Arena* merged_arena;
        Arena::Position base_pos;
        DirFileArray files;
        MergedFileArray merged_files;
        uint64_t longest_line;
        int64_t idx_page_jump;
        UI::Widgets::ID id;
        UI::Widgets::IndexedScrollBox* scroll;
        Glyph::Atlas* atlas;
    };

    namespace
    {
        UI::Widgets::IndexedScrollContentSize content_size(DiffDirListView* widget, Glyph::RenderFontContext* font_ctx)
        {
            UI::Widgets::IndexedScrollContentSize size{};
            const int line_height = font_ctx->current_font_line_height();
            const float glyph_width_est = font_ctx->measure_text("H").x;
            size.v_size = widget->merged_files.size != 0 ? widget->merged_files.size : widget->files.size;
            size.entry_size.y = static_cast<float>(line_height);
            size.entry_size.x = glyph_width_est * widget->longest_line;
            // One extra width for padding.
            size.entry_size.x += glyph_width_est;

            return size;
        }
    } // namespace [anon]

    // Creation.
    DiffDirListView* make_diff_dir_list_view(Glyph::Atlas* atlas, UI::Widgets::ID id)
    {
        Arena::Arena* arena = Arena::alloc(Arena::default_params);
        DiffDirListView* widget = Arena::push_array<DiffDirListView>(arena, 1);
        widget->arena = arena;
        widget->merged_arena = Arena::alloc(Arena::default_params);
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
    void release_diff_dir_list_view(DiffDirListView* widget)
    {
        // Destroy C++ object.
        using SBox = UI::Widgets::IndexedScrollBox;
        widget->scroll->~SBox();

        Arena::release(widget->merged_arena);
        Arena::Arena* arena = widget->arena;
        Arena::release(arena);
    }

    // Interaction.
    void diff_dir_list_view_populate_files(DiffDirListView* widget, FlatDirEntryList lst)
    {
        widget->files = {};
        Arena::pop_to(widget->arena, widget->base_pos);
        widget->files.size = lst.count;
        widget->files.array = Arena::push_array_no_zero<OS::DirIterResult>(widget->arena, widget->files.size);
        widget->files.base_dir = str8_copy(widget->arena, lst.base_dir);
        widget->longest_line = 0;
        uint64_t idx = 0;
        for EachNode(n, lst.first)
        {
            widget->files.array[idx] = n->item;
            widget->files.array[idx++].path = str8_copy(widget->arena, n->item.path);
            widget->longest_line = std::max(widget->longest_line, n->item.path.size);
        }
    }

    void diff_dir_list_view_populate_merged_files(DiffDirListView* widget, MergedFileList lst)
    {
        widget->merged_files = {};
        Arena::clear(widget->merged_arena);
        widget->merged_files.size = lst.count;
        widget->merged_files.array = Arena::push_array_no_zero<MergedFile>(widget->merged_arena, widget->merged_files.size);
        uint64_t idx = 0;
        for EachNode(n, lst.first)
        {
            MergedFile* file = &widget->merged_files.array[idx++];
            *file = n->merged;
            if (file->file.path.size != 0)
            {
                // We want to copy the full path.
                file->file.path = OS::combine_paths(widget->merged_arena, widget->files.base_dir, file->file.path);
                file->rel_path = str8_chop_prefix(file->file.path, widget->files.base_dir);
            }
        }
    }

    void diff_dir_list_view_share_scroll_pos(DiffDirListView* widget, const DiffDirListView* share_from)
    {
        UI::Widgets::IndexedScrollOffset off = share_from->scroll->position_no_offset();
        UI::Widgets::IndexedScrollContentSize size_target = widget->scroll->content_size();
        off.offset.x = std::min(off.offset.x, size_target.entry_size.x);
        widget->scroll->scroll_to(off);
    }

    // Queries.
    DirFileArray diff_dir_list_view_file_array(DiffDirListView* widget)
    {
        return widget->files;
    }

    MergedFileArray diff_dir_list_view_merged_file_array(DiffDirListView* widget)
    {
        return widget->merged_files;
    }

    // Building.
    DiffDirListViewResponse build_diff_dir_list_view(DiffDirListView* widget,
                                                        CmdBuffer::DrawList* lst,
                                                        UI::UIState* state)
    {
        DiffDirListViewResponse resp = {};
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
        if (widget->merged_files.size == 0)
        {
            if (widget->files.size != 0)
            {
                uint64_t first = uint64_t(off.idx);
                uint64_t last = first + (off.offset.y > 0.f) + lines_per_v;
                last = std::clamp(last, first, widget->files.size - 1);
                CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
                for (; first <= last; ++first)
                {
                    String8 path = widget->files.array[first].path;
                    font_ctx.render_text(lst, path, start_pos, colors.eq_txt);
                    start_pos.y -= line_height;
                }
            }
        }
        else
        {
            uint64_t first = uint64_t(off.idx);
            uint64_t last = first + (off.offset.y > 0.f) + lines_per_v;
            last = std::clamp(last, first, widget->merged_files.size - 1);
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
                MergedFile* f = &widget->merged_files.array[hl_line];
                Vec2f size = { rep(content_clip.width) + off.offset.x, line_height + 0.f };
                color = colors_line_map[rep(f->type)];
                // Don't highlight equal lines.
                if (f->type != EditType::Eq)
                {
                    CmdBuffer::solid_rect(lst, Render::FragShader::BasicColor, hl_pos, size, color);
                }
                hl_pos.y -= line_height;
            }
            CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
            for (; first <= last; ++first)
            {
                MergedFile* f = &widget->merged_files.array[first];
                String8 txt = f->rel_path;
                Vec2f pos = start_pos;
                switch (f->type)
                {
                case EditType::Del:
                    pos = font_ctx.render_text(lst, "-", pos, colors.del_mark);
                    break;
                case EditType::Ins:
                    pos = font_ctx.render_text(lst, "+", pos, colors.ins_mark);
                    break;
                case EditType::Eq:
                    // Just shift the text past the marker.
                    pos.x += glyph_width_est;
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