#pragma once

#include <memory>
#include <string_view>

#include "assets.h"
#include "cmd-buffer-api.h"
#include "gap-strings.h"
#include "renderer.h"
#include "types.h"
#include "utf-8.h"
#include "vec.h"

namespace Feed
{
    class MessageFeed;
} // namespace Feed

namespace Glyph
{
    struct CachedFont;
    class Atlas;

    struct CustomContextColors
    {
        Vec4f whitespace;
        Vec4f carriage_return;
        Vec4f color_glyph_bg;
    };

    class RenderFontContext
    {
    public:
        // Returns a new position to start rendering from.
        Vec2f render_text(CmdBuffer::DrawList* lst,
                            String8 text,
                            const Vec2f& pos,
                            const Vec4f& color);
        Vec2f render_text(CmdBuffer::DrawList* lst,
                            std::string_view text,
                            const Vec2f& pos,
                            const Vec4f& color);
        Vec2f render_text_ignore_clip(CmdBuffer::DrawList* lst,
                            std::string_view text,
                            const Vec2f& pos,
                            const Vec4f& color);
        Vec2f render_glyph(CmdBuffer::DrawList* lst,
                            UTF8::Codepoint cp,
                            const Vec2f& pos,
                            const Vec4f& color);
        Vec2f render_icon_glyph(CmdBuffer::DrawList* lst,
                            SpecialGlyph glyph,
                            const Vec2f& pos,
                            const Vec4f& color);
        // Similar to the above, but does not take bitmap top or left into account.
        Vec2f render_glyph_no_offsets(CmdBuffer::DrawList* lst,
                            UTF8::Codepoint cp,
                            const Vec2f& pos,
                            const Vec4f& color);
        Vec2f render_icon_glyph_no_offsets(CmdBuffer::DrawList* lst,
                            SpecialGlyph glyph,
                            const Vec2f& pos,
                            const Vec4f& color);
        Vec2f render_icon_glyph_no_offsets_rotation(CmdBuffer::DrawList* lst,
                            SpecialGlyph glyph,
                            float rotation,
                            const Vec2f& pos,
                            const Vec4f& color);

        // Measurement functions.
        Vec2f measure_text(String8 text);
        Vec2f measure_text(std::string_view text);
        Vec2f measure_scaled_text(std::string_view text, float scalar);
        Vec2f glyph_size(UTF8::Codepoint cp);
        Vec2f icon_glyph_size(SpecialGlyph glyph);
        size_t glyph_count_to_point(std::string_view text, float x_point);
        int current_font_size();
        int current_font_line_height();

        // Configuration.
        void tabstop(Tabstop ts);
        void whitespace_color(const Vec4f& color);
        void carriage_return_color(const Vec4f& color);
        void render_whitespace(bool b);
        void modulate_colored_glyphs(bool b);

        // Queries.
        Render::BasicTexture atlas_texture() const;
    private:
        friend Atlas;
        RenderFontContext(Atlas* atlas, CachedFont* font);

        Atlas* atlas;
        CachedFont* font;
        Tabstop tabs;
        CustomContextColors colors;
        bool render_ws;
        bool mod_colored_glyphs;
    };

    class Atlas
    {
    public:
        struct Data;

        Atlas();
        ~Atlas();

        // Only used for library initialization.
        bool init(Feed::MessageFeed* feed);
        bool populate_atlas();

        // App interaction.
        bool rebuild_atlas();
        void try_load_font_face(String8 path, Feed::MessageFeed* feed);
        void try_load_default_font_asset(Feed::MessageFeed* feed);
        String8 font_family() const;
        void sync_config();

        // Acquire font renderer.
        RenderFontContext render_font_context(FontSize size);

        // Queries.
        Render::BasicTexture atlas_texture() const;

    private:
        friend RenderFontContext;
        std::unique_ptr<Data> data;
    };
} // namespace Glyph