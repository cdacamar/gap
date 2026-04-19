#include "basic-textbox.h"

#include <cassert>

#include <string>
#include <vector>

#include "basic-box.h"
#include "config.h"
#include "ui-common.h"

namespace UI::Widgets
{
    using CharOffset = Diff::CharOffset;

    namespace
    {
        using LineStarts = std::vector<CharOffset>;

        void populate_line_starts(std::string_view text, LineStarts* line_starts, Diff::Length* longest_line)
        {
            line_starts->clear();
            line_starts->push_back(CharOffset{});
            *longest_line = {};
            for (size_t i = 0; i != text.size(); ++i)
            {
                if (text[i] == '\n')
                {
                    // Compute longest line as well.
                    Diff::Length line_len = distance(line_starts->back(), CharOffset{ i + 1 });
                    *longest_line = std::max(line_len, *longest_line);
                    line_starts->push_back(CharOffset{ i + 1 });
                }
            }
        }
    } // namespace [anon]

    struct MultilineTextbox::Data
    {
        std::string text;
        LineStarts line_starts;
        Diff::Length longest_line = {};
        Vec2f offset;
        Glyph::FontSize font_size = Glyph::FontSize{ 18 };
    };

    namespace
    {
        enum class Line : size_t { };
        Line text_start_for_visual_offset(MultilineTextbox::Data* data, Glyph::RenderFontContext* font_ctx)
        {
            // Only consider vertical offset for now.
            const auto line_height = font_ctx->current_font_line_height();
            const auto offset = data->offset.y;
            return Line{ static_cast<size_t>(offset / line_height) };
        }

        struct LineRange
        {
            CharOffset first;
            CharOffset last;
        };

        LineRange line_range(MultilineTextbox::Data* data, Line line)
        {
            if (rep(line) >= data->line_starts.size())
                return { CharOffset{}, CharOffset{ data->text.size() } };
            auto start = data->line_starts[rep(line)];
            auto next_line = extend(line);
            if (rep(next_line) >= data->line_starts.size())
                return { start, CharOffset{ data->text.size() } };
            auto end = data->line_starts[rep(next_line)];
            return { start, retract(end) };
        }

        std::string_view line_text(MultilineTextbox::Data* data, Line line)
        {
            auto [first, last] = line_range(data, line);
            return std::string_view{ &data->text[rep(first)], rep(last) - rep(first) };
        }
    } // namespace [anon]

    MultilineTextbox::MultilineTextbox():
        data{ new Data } { }

    MultilineTextbox::~MultilineTextbox() = default;

    void MultilineTextbox::text(std::string_view text)
    {
        data->text = text;
        populate_line_starts(text, &data->line_starts, &data->longest_line);
    }

    void MultilineTextbox::text(String8List strings)
    {
        data->text.clear();
        data->text.reserve(strings.total_size);
        for (String8 str : strings)
        {
            data->text.append(str.str, str.size);
        }
        populate_line_starts(data->text, &data->line_starts, &data->longest_line);
    }

    void MultilineTextbox::offset(const Vec2f& offset)
    {
        data->offset = offset;
    }

    void MultilineTextbox::font_size(Glyph::FontSize size)
    {
        data->font_size = size;
    }

    Vec2f MultilineTextbox::content_size(Glyph::Atlas* atlas) const
    {
        Vec2f size;
        auto font_ctx = atlas->render_font_context(data->font_size);
        const auto line_height = font_ctx.current_font_line_height();
        // Use the longest line info to heuristically guess what the width should be.
        float glyph_width_est = font_ctx.measure_text("H").x;
        size.x = glyph_width_est * rep(data->longest_line);
        // Note: Add extra line to account for final line.
        size.y = static_cast<float>(data->line_starts.size() * line_height) + line_height;
        return size;
    }

    std::string_view MultilineTextbox::text() const
    {
        return data->text;
    }

    void MultilineTextbox::build(CmdBuffer::DrawList* lst, Glyph::Atlas* atlas)
    {
        const auto clip = CmdBuffer::current_clip(*lst);
        // Find the first line to render.
        auto font_ctx = atlas->render_font_context(data->font_size);
        Line line = text_start_for_visual_offset(data.get(), &font_ctx);
        // Nothing to render.
        if (rep(line) >= data->line_starts.size())
            return;
        CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
        auto line_height = font_ctx.current_font_line_height();
        auto start_y = rep(clip.height) + fmodf(data->offset.y, static_cast<float>(line_height)) - line_height;
        Vec2f pos{ -data->offset.x, start_y };
        auto last = data->line_starts.size();
        auto* palette = CmdBuffer::current_palette(*lst);
        for (; rep(line) < last; line = extend(line))
        {
            auto txt = line_text(data.get(), line);
            font_ctx.render_text(lst, txt, pos, palette->text);
            pos.y -= line_height;

            if (pos.y < -line_height)
                break;
        }
    }

    // Free textbox functions.
    void build_clip_aligned_textbox(CmdBuffer::DrawList* lst, Glyph::RenderFontContext* font_ctx, const ClipAlignedTextboxInput& in)
    {
        auto clip = CmdBuffer::current_clip(*lst);
        const auto* palette = CmdBuffer::current_palette(*lst);

        bool padded_clip = false;
        if (in.border_width != Width{})
        {
            CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
            Vec2f size{ rep(clip.width) + 0.f, rep(clip.height) + 0.f };
            CmdBuffer::strike_rect(lst, Render::FragShader::BasicColor, {}, size, rep(in.border_width) + 0.f, palette->border);
            // Pad the viewport.
            clip = pad_clip(clip, rep(in.border_width));
            CmdBuffer::push_clip(lst, clip);
            padded_clip = true;
        }

        Vec2f pos;
        pos.y = (rep(clip.height) + font_ctx->current_font_size()) / 5.f;

        switch (in.align)
        {
        case TextAlign::Left:
            break;
        case TextAlign::Center:
            {
                float width = font_ctx->measure_text(in.text).x;
                pos.x = (rep(clip.width) - width) / 2.f;
            }
            break;
        case TextAlign::Right:
            assert(not "NYI");
            break;
        }

        CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
        font_ctx->render_text(lst, in.text, pos, palette->text);

        if (padded_clip)
        {
            CmdBuffer::pop_clip(lst);
        }
    }

    Vec2f measure_textbox(Glyph::RenderFontContext* font_ctx, const TextboxInput& in)
    {
        Vec2f txtbox_size{};
        txtbox_size.x = font_ctx->measure_text(in.text).x + in.thickness * 2 + in.padding.x * 2;
        txtbox_size.y = UI::standard_font_padding(Glyph::FontSize{ font_ctx->current_font_size() }) + in.padding.y * 2;
        return txtbox_size;
    }

    void build_textbox(CmdBuffer::DrawList* lst, UIState* state, Glyph::RenderFontContext* font_ctx, const TextboxInput& in, BuildTextboxFlags flags)
    {
        Vec2f txtbox_size = measure_textbox(font_ctx, in);
        BuildBoxInput box_in{
            .id = ID::Zero,
            .pos = in.pos,
            .size = txtbox_size,
            .thickness = in.thickness
        };
        using Flgs = BuildBoxFlags;
        Flgs box_flags = Flgs::None;
        if (implies(flags, BuildTextboxFlags::Fill))
        {
            box_flags |= Flgs::Fill;
        }

        if (implies(flags, BuildTextboxFlags::Strike))
        {
            box_flags |= Flgs::Strike;
        }
        basic_box(lst, state, box_in, box_flags);
        CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
        auto text_pos = in.pos;
        text_pos.x += (txtbox_size.x - font_ctx->measure_text(in.text).x) / 2.f;
        text_pos.y += (txtbox_size.y + font_ctx->current_font_line_height() + in.padding.y * 2) / 5.f;
        const auto* palette = CmdBuffer::current_palette(*lst);
        font_ctx->render_text(lst, in.text, text_pos, palette->text);
    }

    Vec2f measure_multiline_textbox(Glyph::RenderFontContext* font_ctx, const MultiLineTextboxInput& in)
    {
        Vec2f size{};
        size = in.padding * 2.f;
        // Count lines.
        std::string_view start = in.text;
        const float line_height = font_ctx->current_font_line_height() + 0.f;
        do
        {
            auto line_end = start.find('\n');
            if (line_end == std::string_view::npos)
            {
                size.x = std::max(size.x, font_ctx->measure_text(start).x);
                size.y += line_height;
                break;
            }
            auto sub = start.substr(0, line_end);
            size.x = std::max(size.x, font_ctx->measure_text(sub).x);
            size.y += line_height;
            // Move past the '\n'.
            start = start.substr(line_end + 1);
        } while (true);
        return size;
    }

    void build_multiline_textbox(CmdBuffer::DrawList* lst, Glyph::RenderFontContext* font_ctx, const MultiLineTextboxInput& in)
    {
        CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
        const auto* palette = CmdBuffer::current_palette(*lst);
        // Count lines.
        std::string_view start = in.text;
        const float line_height = font_ctx->current_font_line_height() + 0.f;
        Vec2f pos = in.pos;
        pos.x += in.padding.x;
        pos.y -= in.padding.y;
        do
        {
            auto line_end = start.find('\n');
            if (line_end == std::string_view::npos)
            {
                font_ctx->render_text(lst, start, pos, palette->text);
                break;
            }
            auto sub = start.substr(0, line_end);
            font_ctx->render_text(lst, sub, pos, palette->text);
            pos.y -= line_height;
            // Move past the '\n'.
            start = start.substr(line_end + 1);
        } while (true);
    }
} // namespace UI::Widgets