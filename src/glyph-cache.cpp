#include "glyph-cache.h"

#include <cassert>

#include <algorithm>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "assets.h"
#include "config.h"
#include "enum-utils.h"
#include "feed.h"
#include "gap-bits.h"
#include "scoped-handle.h"
#include "gap-strings.h"
#include "utf-8.h"
#include "util.h"

// Shamelessly stolen :)
// https://en.wikibooks.org/wiki/OpenGL_Programming/Modern_OpenGL_Tutorial_Text_Rendering_02

namespace Glyph
{
    namespace
    {
        struct CharInfo
        {
            float ax; // advance.x
            float ay; // advance.y

            float aw; // atlas row width.
            float bw; // bitmap.width;
            float bh; // bitmap.rows;

            float bl; // bitmap_left;
            float bt; // bitmap_top;

            float tx; // x offset of glyph in texture coordinates
            float ty; // y offset of glyph in texture coordinates
        };

        constexpr int MarkerGlyphCount = count_of<SpecialGlyph>;
        constexpr int ValidCharStart = 32;
        constexpr int CharInfoCount = 128;
        constexpr int TotalCharInfoCount = CharInfoCount + MarkerGlyphCount;

        struct SpecialGlyphEntry
        {
            int index;
            FT_ULong glyph;
        };

        constexpr SpecialGlyphEntry special_glyph_map[] = {
            { .index = CharInfoCount + rep(SpecialGlyph::Whitespace),     .glyph = 0xE008 },
            { .index = CharInfoCount + rep(SpecialGlyph::CarriageReturn), .glyph = 0xE001 },
            { .index = CharInfoCount + rep(SpecialGlyph::ArrowDown),      .glyph = 0xE005 },
            { .index = CharInfoCount + rep(SpecialGlyph::ArrowUp),        .glyph = 0xE004 },
            { .index = CharInfoCount + rep(SpecialGlyph::ArrowRight),     .glyph = 0xE003 }, // Also Tab
            { .index = CharInfoCount + rep(SpecialGlyph::ArrowLeft),      .glyph = 0xE002 },
            { .index = CharInfoCount + rep(SpecialGlyph::Search),         .glyph = 0xE006 },
            { .index = CharInfoCount + rep(SpecialGlyph::Replace),        .glyph = 0xE000 },
            { .index = CharInfoCount + rep(SpecialGlyph::IncompleteCRLF), .glyph = 0xE007 },
            { .index = CharInfoCount + rep(SpecialGlyph::Checkmark),      .glyph = 0xE00A },
            { .index = CharInfoCount + rep(SpecialGlyph::X),              .glyph = 0xE00B },
            { .index = CharInfoCount + rep(SpecialGlyph::Copy),           .glyph = 0xE00C },
            { .index = CharInfoCount + rep(SpecialGlyph::Terminal),       .glyph = 0xE00D },
            { .index = CharInfoCount + rep(SpecialGlyph::Share),          .glyph = 0xE00E },
            { .index = CharInfoCount + rep(SpecialGlyph::Wrench),         .glyph = 0xE00F },
            { .index = CharInfoCount + rep(SpecialGlyph::Trash),          .glyph = 0xE010 },
            { .index = CharInfoCount + rep(SpecialGlyph::Anchor),         .glyph = 0xE011 },
            { .index = CharInfoCount + rep(SpecialGlyph::Filter),         .glyph = 0xE012 },
            { .index = CharInfoCount + rep(SpecialGlyph::CircleQuestion), .glyph = 0xE013 },
            { .index = CharInfoCount + rep(SpecialGlyph::SaveDisk),       .glyph = 0xE014 },
            { .index = CharInfoCount + rep(SpecialGlyph::Plus),           .glyph = 0xE015 },
            { .index = CharInfoCount + rep(SpecialGlyph::Minus),          .glyph = 0xE016 },
            { .index = CharInfoCount + rep(SpecialGlyph::CaseSensitive),  .glyph = 0xE017 },
            { .index = CharInfoCount + rep(SpecialGlyph::WholeWord),      .glyph = 0xE018 },
            { .index = CharInfoCount + rep(SpecialGlyph::Regex),          .glyph = 0xE019 },
        };

        static_assert(std::size(special_glyph_map) == count_of<SpecialGlyph>);
        static_assert(std::is_sorted(std::begin(special_glyph_map),
                                        std::end(special_glyph_map),
                                        [](const auto& a, const auto& b)
                                        {
                                            return a.index <  b.index;
                                        }));

        struct FTLibraryCleanup
        {
            void operator()(FT_Library lib) const
            {
                if (lib != nullptr)
                {
                    FT_Done_FreeType(lib);
                }
            }
        };

        using FTLibraryHandle = ScopedHandle<FT_Library, FTLibraryCleanup>;

        struct FTFaceCleanup
        {
            void operator()(FT_Face face) const
            {
                if (face != nullptr)
                {
                    FT_Done_Face(face);
                }
            }
        };

        using FTFaceHandle = ScopedHandle<FT_Face, FTFaceCleanup>;

        constexpr FT_UInt texture_width = 1920;
        constexpr FT_UInt texture_height = 1088;

        struct UnicodeGlyphInfo
        {
            CharInfo info;
            FT_Face face;
            bool rasterized = false;
            bool failed_to_rasterize = false;
            bool colored = false;
        };

        struct UnicodeGlyphKey
        {
            UnicodeGlyphKey* next;
            UnicodeGlyphInfo* data;
            UTF8::Codepoint key;
        };

        struct UnicodeGlyphMap
        {
            UnicodeGlyphKey** buckets;
            uint64_t capacity;
            uint64_t count;
            uint64_t nil_buckets;
            uint64_t load;
        };

        UnicodeGlyphMap make_unicode_glyph_map(Arena::Arena* arena, uint64_t hint_size)
        {
            UnicodeGlyphMap map{};
            const uint64_t pow_2_aligned_size = up_pow2(hint_size);
            map.capacity = pow_2_aligned_size;
            map.nil_buckets = map.capacity;
            map.buckets = Arena::push_array<UnicodeGlyphKey*>(arena, map.capacity);
            return map;
        }

        struct MapUnicodeGlyphResult
        {
            UnicodeGlyphInfo* node;
            bool inserted;
        };

        MapUnicodeGlyphResult try_push_unicode_glyph_map_glyph(Arena::Arena* arena, UnicodeGlyphMap* map, UTF8::Codepoint glyph)
        {
            MapUnicodeGlyphResult result{};
            uint64_t idx = (map->capacity - 1) & glyph;
            UnicodeGlyphKey* slot = map->buckets[idx];
            if (slot == nullptr)
            {
                slot = Arena::push_array<UnicodeGlyphKey>(arena, 1);
                slot->key = glyph;
                slot->data = Arena::push_array<UnicodeGlyphInfo>(arena, 1);
                map->buckets[idx] = slot;
                ++map->count;
                --map->nil_buckets;
                map->load = std::max(map->load, uint64_t(1));
                result.inserted = true;
            }
            else
            {
                uint64_t elms = 0;
                bool insert = true;
                while (slot->next != nullptr)
                {
                    if (slot->key == glyph)
                    {
                        insert = false;
                        break;
                    }
                    ++elms;
                    slot = slot->next;
                }

                if (slot->key == glyph)
                {
                    insert = false;
                }

                if (insert)
                {
                    UnicodeGlyphKey* new_entry = Arena::push_array<UnicodeGlyphKey>(arena, 1);
                    new_entry->key = glyph;
                    new_entry->data = Arena::push_array<UnicodeGlyphInfo>(arena, 1);
                    slot->next = new_entry;
                    slot = new_entry;
                    ++map->count;
                    elms += 2; // +1 for current slot and +1 for new node.
                    map->load = std::max(map->load, elms);
                    result.inserted = true;
                }
            }
            assert(slot != nullptr);
            result.node = slot->data;
            return result;
        }

        struct FallbackFontNode
        {
            FallbackFontNode* next;
            FT_Face face;
        };

        struct FallbackFontList
        {
            FallbackFontNode* first;
            FallbackFontNode* last;
            uint64_t count;
        };
    } // namespace [anon]

    struct CachedFont
    {
        CachedFont* next;
        Arena::Arena* glyph_map_arena;
        UnicodeGlyphMap cached_glyphs_map;
        int font_size = 64;
        CharInfo infos[TotalCharInfoCount];
    };

    namespace
    {
        struct CachedFontsMap
        {
            CachedFont** buckets;
            uint64_t capacity;
            uint64_t count;
            uint64_t nil_buckets;
            uint64_t load;
        };

        CachedFontsMap make_cached_fonts_map(Arena::Arena* arena, uint64_t hint_size)
        {
            CachedFontsMap map{};
            const uint64_t pow_2_aligned_size = up_pow2(hint_size);
            map.capacity = pow_2_aligned_size;
            map.nil_buckets = map.capacity;
            map.buckets = Arena::push_array<CachedFont*>(arena, map.capacity);
            return map;
        }

        struct MapCachedFontResult
        {
            CachedFont* node;
            bool inserted;
        };

        MapCachedFontResult try_push_cached_fonts_map_font(Arena::Arena* arena, CachedFontsMap* map, int font_size)
        {
            MapCachedFontResult result{};
            uint64_t idx = (map->capacity - 1) & static_cast<uint32_t>(font_size);
            CachedFont* slot = map->buckets[idx];
            if (slot == nullptr)
            {
                slot = Arena::push_array<CachedFont>(arena, 1);
                slot->font_size = font_size;
                map->buckets[idx] = slot;
                ++map->count;
                --map->nil_buckets;
                map->load = std::max(map->load, uint64_t(1));
                result.inserted = true;
            }
            else
            {
                uint64_t elms = 0;
                bool insert = true;
                while (slot->next != nullptr)
                {
                    if (slot->font_size == font_size)
                    {
                        insert = false;
                        break;
                    }
                    ++elms;
                    slot = slot->next;
                }

                if (slot->font_size == font_size)
                {
                    insert = false;
                }

                if (insert)
                {
                    CachedFont* new_entry = Arena::push_array<CachedFont>(arena, 1);
                    new_entry->font_size = font_size;
                    slot->next = new_entry;
                    slot = new_entry;
                    ++map->count;
                    elms += 2; // +1 for current slot and +1 for new node.
                    map->load = std::max(map->load, elms);
                    result.inserted = true;
                }
            }
            assert(slot != nullptr);
            result.node = slot;
            return result;
        }

        uint32_t bitmap_width(const FT_Bitmap& bmp)
        {
            if (bmp.pixel_mode == FT_PIXEL_MODE_LCD)
                return bmp.width / 3;
            return bmp.width;
        }

        uint32_t bitmap_atlas_width(const FT_Bitmap& bmp)
        {
            if (bmp.pixel_mode == FT_PIXEL_MODE_LCD)
                return bmp.width;
            return bmp.width;
        }
    } // namespace [anon]

    struct LoadedFontBuffer
    {
        FT_Byte* data;
        uint64_t size;
    };

    struct Atlas::Data
    {
        static constexpr int default_font_size = 64;

        FTLibraryHandle library;

        FTFaceHandle face;
        FTFaceHandle special_face; // For storing the special glyphs.

        FT_UInt height{};
        FT_UInt width{};

        // For caching glyphs on the fly.
        FT_UInt unicode_row_start{};
        FT_UInt next_x{};
        FT_UInt next_y{};
        FT_UInt cur_row_max_height{};
        Arena::Arena* metadata_arena{};
        Arena::Arena* font_face_arena{};
        Arena::Arena* cached_fonts_arena{};
        FallbackFontList fallback_fonts{};
        CachedFont* selected_font{};
        CachedFontsMap cached_fonts;

        Render::BasicTexture texture{};

        LoadedFontBuffer special_face_buf{};
        LoadedFontBuffer font_face_buf{};
        Render::FragShader text_shader = Render::FragShader::Text; // If this is set to TextSubpixel, we're rendering in subpixel mode.
    };

    namespace
    {
        void push_font_face(Arena::Arena* arena, FallbackFontList* lst, FT_Face face)
        {
            FallbackFontNode* node = Arena::push_array<FallbackFontNode>(arena, 1);
            node->face = face;
            SLLQueuePush(lst->first, lst->last, node);
            ++lst->count;
        }

        FT_Face identify_font_face_for_glyph(Atlas::Data* data, UTF8::Codepoint glyph)
        {
            // Try the most obvious spot first, the font currently selected.
            auto idx = FT_Get_Char_Index(data->face.handle(), glyph);
            if (idx != 0)
                return data->face.handle();
            PROF_SCOPE();
            // Need to load the fallback fonts.
            if (data->fallback_fonts.count == 0)
            {
                PROF_SCOPE();
                PROF_NAME_SCOPE("Populate fallback fonts", 0);
                // Now we need to try fallback fonts.
                // Do the dumb thing for now and load them all.
                auto scratch = Arena::scratch_begin(Arena::no_conflicts);
                FilesInDirList files = files_in_dir(scratch.arena, Config::system_fonts().fallback_fonts_folder, str8_mut(str8_literal(".ttf")));
                // Insert a sentinel value to avoid reloading this.
                push_font_face(data->metadata_arena, &data->fallback_fonts, nullptr);
                // Try to load each face.
                for EachNode(n, files.first)
                {
                    String8 file = n->string;
                    FT_Face face{ };
                    auto error = FT_New_Face(data->library.handle(), file.str, 0, &face);
                    if (error != 0)
                    {
                        const char* log = FT_Error_String(error);
                        fprintf(stderr, "Failed to load fallback font file '%s': %s\n", file.str, log);
                        continue;
                    }

                    constexpr FT_UInt pixel_size = Atlas::Data::default_font_size;
                    // Width == 0.  We don't want bold fonts.
                    error = FT_Set_Pixel_Sizes(face, 0, pixel_size);
                    if (error != 0)
                    {
                        const char* log = FT_Error_String(error);
                        fprintf(stderr, "Failed to set font size on fallback font: %s\n", log);
                        FT_Done_Face(face);
                        continue;
                    }
                    // We've loaded this face, we can now insert it.
                    push_font_face(data->metadata_arena, &data->fallback_fonts, face);
                }
                Arena::scratch_end(scratch);
            }

            // In the fallback fonts, try to find the face which could rasterize this glyph.
            for EachNode(n, data->fallback_fonts.first)
            {
                // This is the sentinel face.
                if (n->face == nullptr)
                    continue;
                idx = FT_Get_Char_Index(n->face, glyph);
                if (idx != 0)
                {
#ifndef NDEBUG
                    printf("Fallback font '%s' selected for glyph %x\n", n->face->family_name, glyph);
#endif // NDEBUG
                    return n->face;
                }
            }
#ifndef NDEBUG
            fprintf(stderr, "Glyph %x has no appropriate font\n", glyph);
#endif // NDEBUG
            // Return the default face so that the renderer can consistently render missing glyph slots.
            return data->face.handle();
        }

        FT_Int32 raster_flags[] =
        {
            FT_LOAD_RENDER | FT_LOAD_COLOR | FT_LOAD_TARGET_NORMAL,  // Normal rasterization.
            // Note: There's something weird about color glyph loading when LCD is enabled.  When the colored glyph is rasterized, the
            // result is stretched for some reason, but not evenly like the regular grayscale glyphs are.  Needs more investigation...
            FT_LOAD_RENDER | /*FT_LOAD_COLOR |*/ FT_LOAD_TARGET_LCD      // Subpixel AA.
        };

        // After measuring a few times, I determined these values were roughly the constant overhead
        // that SDF rendering added to pad each glyph.  I'm willing to be proven wrong, in which case
        // we flip the 'load_flags' to 'FT_RENDER_MODE_SDF' and resolve the problem, but the normal
        // render mode allows for large unicode files to be loaded much faster due to only measure_text
        // being required to tokenize the file.
#if 0
        constexpr FT_UInt sdf_width_addition = 16;
        constexpr FT_UInt sdf_height_addition = 26;
#endif
        constexpr FT_UInt sdf_width_addition = 0;
        constexpr FT_UInt sdf_height_addition = 0;

        constexpr auto standard_reporter = [](String8 msg)
        {
            fprintf(stderr, "%.*s\n", int(msg.size), msg.str);
        };

        template <typename Reporter>
        bool resize_font(FT_Face face, int size, Reporter&& reporter)
        {
            // Width == 0.  We don't want bold fonts.
#if 1 // DPI experiments.
            auto error = FT_Set_Pixel_Sizes(face, 0, size);
#else
            DPI dpi = get_platform_dpi();
#if 0
            float scaled_dpi = 92.f / rep(dpi);
            int scaled_size = static_cast<int>(size * scaled_dpi);
#else
            int scaled_size = size;
#endif
            auto error = FT_Set_Char_Size(face,
                                            0,
                                            scaled_size * 64,
                                            0,
                                            rep(dpi));
#endif
            if (error)
            {
                const char* log = FT_Error_String(error);
                char fmt_buf[1024];
                String8 msg = fmt_string(fmt_buf, "Failed to set font size: %s", log);
                reporter(msg);
                return false;
            }
            return true;
        }

        struct GlyphBufferData
        {
            uint8_t* data;
            uint64_t size;
        };

        const uint8_t* buffer_for_glyph_pixels(Arena::Arena* arena, const FT_Bitmap* bitmap, GlyphBufferData* blit_buffer)
        {
            if (bitmap->pixel_mode == FT_PIXEL_MODE_BGRA)
            {
                const size_t w = bitmap->width;
                const size_t r = bitmap->rows;
                const size_t p = bitmap->pitch;
                // All we want to do is swap B and R values to make this an RGBA texture
                // in order to avoid the renderer having to do any conversions.
                for (size_t ii = 0; ii < r; ++ii)
                {
                    for (size_t jj = 0; jj < w; ++jj)
                    {
                        unsigned char B = bitmap->buffer[ii * p + jj * 4 + 0];
                        bitmap->buffer[ii * p + jj * 4 + 0] = bitmap->buffer[ii * p + jj * 4 + 2];
                        bitmap->buffer[ii * p + jj * 4 + 2] = B;
                    }
                }
                return bitmap->buffer;
            }

            if (bitmap->pixel_mode == FT_PIXEL_MODE_LCD)
            {
                const size_t w = bitmap->width;
                const size_t r = bitmap->rows;
                const size_t p = bitmap->pitch;
                // Make the blit buffer account for the loss in color depth.
                blit_buffer->size = w * r * 4;
                blit_buffer->data = Arena::push_array<uint8_t>(arena, blit_buffer->size);
                for (size_t ii = 0; ii < r; ++ii)
                {
                    for (size_t jj = 0; jj < w; ++jj)
                    {
                        blit_buffer->data[ii * w * 4 + jj * 4 + 0] = bitmap->buffer[ii * p + jj];
                        blit_buffer->data[ii * w * 4 + jj * 4 + 1] = bitmap->buffer[ii * p + jj];
                        blit_buffer->data[ii * w * 4 + jj * 4 + 2] = bitmap->buffer[ii * p + jj];
                        blit_buffer->data[ii * w * 4 + jj * 4 + 3] = bitmap->buffer[ii * p + jj];
                    }
                }
                return blit_buffer->data;
            }
            const size_t w = bitmap_atlas_width(*bitmap);
            const size_t r = bitmap->rows;
            const size_t p = bitmap->pitch;
            // Make the blit buffer account for the loss in color depth.
            blit_buffer->size = w * r * 4;
            blit_buffer->data = Arena::push_array<uint8_t>(arena, blit_buffer->size);
            for (size_t ii = 0; ii < r; ++ii)
            {
                for (size_t jj = 0; jj < w; ++jj)
                {
                    blit_buffer->data[ii * w * 4 + jj * 4 + 0] = 255;
                    blit_buffer->data[ii * w * 4 + jj * 4 + 1] = 255;
                    blit_buffer->data[ii * w * 4 + jj * 4 + 2] = 255;
                    blit_buffer->data[ii * w * 4 + jj * 4 + 3] = bitmap->buffer[ii * p + jj];
                }
            }
            return blit_buffer->data;
        }

        bool rasterize_cached_glyph(Atlas::Data* data, CachedFont* font, UnicodeGlyphInfo* info, UTF8::Codepoint glyph)
        {
            // Do not attempt to rasterize an invalid codepoint (what would we do anyway?).
            if (glyph == UTF8::invalid_codepoint)
                return false;
            auto* face = info->face;
            // If we could not identify a font face for this glyph, we're done.
            if (face == nullptr)
                return false;
            if (not resize_font(face, font->font_size, standard_reporter))
                return false;
            // Now we cache the resulting render.
            auto error = FT_Load_Char(face, static_cast<FT_ULong>(glyph), raster_flags[data->text_shader == Render::FragShader::TextSubpixel]);
            if (error != 0)
            {
                const char* log = FT_Error_String(error);
                fprintf(stderr, "Failed to load the glyph: %s\n", log);
                return false;
            }

            error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
            if (error != 0)
            {
                const char* log = FT_Error_String(error);
                fprintf(stderr, "Failed to render the glyph: %s\n", log);
                return false;
            }

            FT_UInt x = static_cast<FT_UInt>(info->info.tx * data->width);
            FT_UInt y = static_cast<FT_UInt>(info->info.ty * data->height);

            // This glyph cannot be rasterized.
            if (y + face->glyph->bitmap.rows > data->height)
                return false;

            info->info.ax = static_cast<float>(face->glyph->advance.x >> 6);
            info->info.ay = static_cast<float>(face->glyph->advance.y >> 6);
            info->info.aw = static_cast<float>(bitmap_atlas_width(face->glyph->bitmap));
            info->info.bw = static_cast<float>(bitmap_width(face->glyph->bitmap));
            info->info.bh = static_cast<float>(face->glyph->bitmap.rows);
            info->info.bl = static_cast<float>(face->glyph->bitmap_left);
            info->info.bt = static_cast<float>(face->glyph->bitmap_top);
            // Note: we do not need to update the texture coordinates because they're already
            // SDF-adjusted when we measured.

            auto scratch = Arena::scratch_begin(Arena::no_conflicts);
            GlyphBufferData blit_buffer{};
            const uint8_t* buffer = buffer_for_glyph_pixels(scratch.arena, &face->glyph->bitmap, &blit_buffer);

            Render::GlyphEntry entry{
                .offset_x = Render::GlyphOffsetX(x),
                .offset_y = Render::GlyphOffsetY(y),
                .width = Width(bitmap_atlas_width(face->glyph->bitmap)),
                .height = Height(face->glyph->bitmap.rows),
                .buffer = buffer
            };
            Render::submit_glyph_data(data->texture, entry);

            // Fill in the info.
            info->rasterized = true;
            info->colored = face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;

            Arena::scratch_end(scratch);
            return info;
        }

        void print_map_stats(UnicodeGlyphMap* map)
        {
            auto scratch = Arena::scratch_begin(Arena::no_conflicts);
            Feed::MessageFeed* feed = Feed::global_feed();
            String8 msg = str8_fmt(scratch.arena, "Capacity: %llu, Count: %llu, Load: %llu, Null slots: %llu", map->capacity, map->count, map->load, map->nil_buckets);
            feed->queue_info(msg);
            Arena::scratch_end(scratch);
        }

        enum class Rasterize : bool { No, Yes };

        UnicodeGlyphInfo* request_cached_glyph(Atlas::Data* data, CachedFont* font, UTF8::Codepoint glyph, Rasterize rasterize)
        {
            // Do not attempt to rasterize an invalid codepoint (what would we do anyway?).
            if (glyph == UTF8::invalid_codepoint)
                return nullptr;

            PROF_SCOPE();
            auto [info, inserted] = try_push_unicode_glyph_map_glyph(font->glyph_map_arena, &font->cached_glyphs_map, glyph);
            if (not inserted)
            {
                if (not info->rasterized
                    and is_yes(rasterize))
                {
                    if (info->failed_to_rasterize)
                        return nullptr;
                    info->failed_to_rasterize = not rasterize_cached_glyph(data, font, info, glyph);
                    if (info->failed_to_rasterize)
                        return nullptr;
                    return info;
                }
                return info;
            }
            auto* face = identify_font_face_for_glyph(data, glyph);
            if (not resize_font(face, font->font_size, standard_reporter))
                return nullptr;
            // This ensures we append each successive bitmap image to the RHS of the last.
            FT_UInt x = data->next_x;
            FT_UInt y = data->next_y;
            // Now we cache the resulting glyph info.
            auto error = FT_Load_Char(face, static_cast<FT_ULong>(glyph), raster_flags[data->text_shader == Render::FragShader::TextSubpixel]);
            if (error != 0)
            {
                const char* log = FT_Error_String(error);
                fprintf(stderr, "Failed to load the glyph: %s\n", log);
                return nullptr;
            }

            if (x + sdf_width_addition + bitmap_atlas_width(face->glyph->bitmap) > data->width)
            {
                y += data->cur_row_max_height;
                x = 0;
                data->cur_row_max_height = 0;
            }

            // Note: these are all updated by the time we go to rasterize.
            info->info.ax = static_cast<float>(face->glyph->advance.x >> 6);
            info->info.ay = static_cast<float>(face->glyph->advance.y >> 6);
            info->info.aw = static_cast<float>(bitmap_atlas_width(face->glyph->bitmap));
            info->info.bw = static_cast<float>(bitmap_width(face->glyph->bitmap));
            info->info.bh = static_cast<float>(face->glyph->bitmap.rows);
            info->info.bl = static_cast<float>(face->glyph->bitmap_left);
            info->info.bt = static_cast<float>(face->glyph->bitmap_top);

            info->info.tx = static_cast<float>(x) / static_cast<float>(data->width);
            info->info.ty = static_cast<float>(y) / static_cast<float>(data->height);

            // Note: because we're only measuring, we add the SDF width adjustments here.
            x += bitmap_atlas_width(face->glyph->bitmap) + sdf_width_addition;

            // Write back to the data starts.
            data->next_y = y;
            data->next_x = x;
            data->cur_row_max_height = std::max(data->cur_row_max_height, face->glyph->bitmap.rows + sdf_height_addition);

            // Tell the rasterization process which face to use.
            info->face = face;

            if (is_yes(rasterize))
            {
                info->failed_to_rasterize = not rasterize_cached_glyph(data, font, info, glyph);
                if (info->failed_to_rasterize)
                    return nullptr;
            }
            return info;
        }

        const Vec4f* default_color_filter(const Vec4f* default_color, const CustomContextColors*)
        {
            return default_color;
        }

        const Vec4f* whitespace_glyph_color_filter(const Vec4f*, const CustomContextColors* colors)
        {
            return &colors->whitespace;
        }

        const Vec4f* carriage_return_glyph_color_filter(const Vec4f*, const CustomContextColors* colors)
        {
            return &colors->carriage_return;
        }

        const Vec4f* color_glyph_filter(const Vec4f*, const CustomContextColors* colors)
        {
            return &colors->color_glyph_bg;
        }

        using ColorFilter = const Vec4f*(*)(const Vec4f*, const CustomContextColors*);

        struct GlyphExtractResult
        {
            const CharInfo& info;
            // Sometimes we need to adjust the x_advance based on config info such as tabstop.
            float x_advance;
            ColorFilter color_filter;
        };

        enum class RenderWhitespace : bool { No, Yes };
        enum class ModulateColoredGlyphs : bool { No, Yes };

        GlyphExtractResult extract_glyph_info(Atlas::Data* data,
                                                CachedFont* font,
                                                Tabstop tabstop,
                                                UTF8::Codepoint glyph,
                                                Rasterize rasterize,
                                                RenderWhitespace render_whitespace,
                                                ModulateColoredGlyphs modulate_colored_glyphs)
        {
            ColorFilter filter = default_color_filter;
            if (glyph >= CharInfoCount)
            {
                // Sentinel value.
                if (glyph == UTF8::invalid_codepoint)
                {
                    glyph = '?';
                }
                else if (auto* info = request_cached_glyph(data, font, glyph, rasterize))
                {
                    if (is_no(modulate_colored_glyphs) and info->colored)
                        return { .info = info->info, .x_advance = info->info.ax, .color_filter = color_glyph_filter };
                    return { .info = info->info, .x_advance = info->info.ax, .color_filter = filter };
                }
                // Either the glyph failed to rasterize or there's simply no mapping for it.
                else
                {
                    glyph = '?';
                }
            }

            if (glyph == ' ' and is_yes(render_whitespace))
            {
                glyph = special_glyph_map[rep(SpecialGlyph::Whitespace)].index;
                filter = whitespace_glyph_color_filter;
            }

            if (glyph == '\r')
            {
                glyph = special_glyph_map[rep(SpecialGlyph::CarriageReturn)].index;
                filter = carriage_return_glyph_color_filter;
            }

            if (glyph == '\t')
            {
                glyph = special_glyph_map[rep(SpecialGlyph::Tab)].index;
                filter = whitespace_glyph_color_filter;
                // Compute the additional advance factor (based on the tab character glyph or the whitespace
                // glyph if render whitespace is off).
                // We specifically want the advance of the space character.
                auto ax = font->infos[' '].ax;
                if (not is_yes(render_whitespace))
                {
                    glyph = ' ';
                    filter = default_color_filter;
                }
                const float advance_x = ax * static_cast<float>(rep(tabstop));
                return { .info = font->infos[glyph], .x_advance = advance_x, .color_filter = filter };
            }

            // If we still somehow have a control character, don't render it.
            if (glyph < ValidCharStart)
            {
                glyph = '?';
            }

            return { .info = font->infos[glyph], .x_advance = font->infos[glyph].ax, .color_filter = filter };
        }

        template <typename Reporter>
        bool populate_standard_glyphs(Atlas::Data* data, CachedFont* font, Reporter&& reporter)
        {
            // It is assumed on entry that the unicode map has not been populated and that the
            // texture has been cleared.

            // Note: (just like the wiki above) we skip the first 32 characters of the ASCII table
            // because they're simply control codes which we cannot render.
            auto* face = data->face.handle();
            auto* special_face = data->special_face.handle();

            // This ensures we append each successive bitmap image to the RHS of the last.
            int x = data->next_x;
            int y = data->next_y;
            int max_glyph_height_for_row = data->cur_row_max_height;
            // Set the font size for this population.
            if (not resize_font(face, font->font_size, reporter))
                return false;
            // Set the font size for this population.
            if (not resize_font(special_face, font->font_size, reporter))
                return false;
            // Now we cache the resulting render.
            auto scratch = Arena::scratch_begin(Arena::no_conflicts);
            GlyphBufferData blit_buffer{};
            char fmt_buf[1024];
            bool result = true;
            for (int i = ValidCharStart; i < CharInfoCount; ++i)
            {
                auto error = FT_Load_Char(face, i, raster_flags[data->text_shader == Render::FragShader::TextSubpixel]);
                if (error != 0)
                {
                    const char* log = FT_Error_String(error);
                    String8 msg = fmt_string(fmt_buf, "Failed to load the glyph: %s", log);
                    reporter(msg);
                    result = false;
                    break;
                }

                error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
                if (error != 0)
                {
                    const char* log = FT_Error_String(error);
                    String8 msg = fmt_string(fmt_buf, "Failed to render the glyph: %s", log);
                    reporter(msg);
                    result = false;
                    break;
                }

                if (x + bitmap_atlas_width(face->glyph->bitmap) > data->width)
                {
                    y += max_glyph_height_for_row;
                    x = 0;
                    max_glyph_height_for_row = 0;
                }

                font->infos[i].ax = static_cast<float>(face->glyph->advance.x >> 6);
                font->infos[i].ay = static_cast<float>(face->glyph->advance.y >> 6);
                font->infos[i].aw = static_cast<float>(bitmap_atlas_width(face->glyph->bitmap));
                font->infos[i].bw = static_cast<float>(bitmap_width(face->glyph->bitmap));
                font->infos[i].bh = static_cast<float>(face->glyph->bitmap.rows);
                font->infos[i].bl = static_cast<float>(face->glyph->bitmap_left);
                font->infos[i].bt = static_cast<float>(face->glyph->bitmap_top);
                font->infos[i].tx = static_cast<float>(x) / static_cast<float>(data->width);
                font->infos[i].ty = static_cast<float>(y) / static_cast<float>(data->height);

                auto tmp = Arena::temp_begin(scratch.arena);
                const uint8_t* buffer = buffer_for_glyph_pixels(tmp.arena, &face->glyph->bitmap, &blit_buffer);

                Render::GlyphEntry entry{
                    .offset_x = Render::GlyphOffsetX{ x },
                    .offset_y = Render::GlyphOffsetY{ y },
                    .width = Width(bitmap_atlas_width(face->glyph->bitmap)),
                    .height = Height(face->glyph->bitmap.rows),
                    .buffer = buffer
                };
                Render::submit_glyph_data(data->texture, entry);
                Arena::temp_end(tmp);

                x += bitmap_atlas_width(face->glyph->bitmap);
                max_glyph_height_for_row = std::max(max_glyph_height_for_row, static_cast<int>(face->glyph->bitmap.rows));
            }

            if (result)
            {
                // Special glyphs.
                for (const auto& e : special_glyph_map)
                {
                    auto error = FT_Load_Char(special_face, e.glyph, raster_flags[data->text_shader == Render::FragShader::TextSubpixel]);
                    if (error != 0)
                    {
                        const char* log = FT_Error_String(error);
                        String8 msg = fmt_string(fmt_buf, "Failed to load the glyph: %s", log);
                        reporter(msg);
                        result = false;
                        break;
                    }

                    error = FT_Render_Glyph(special_face->glyph, FT_RENDER_MODE_NORMAL);
                    if (error != 0)
                    {
                        const char* log = FT_Error_String(error);
                        String8 msg = fmt_string(fmt_buf, "Failed to render the glyph: %s", log);
                        reporter(msg);
                        result = false;
                        break;
                    }

                    if (x + bitmap_atlas_width(special_face->glyph->bitmap) > data->width)
                    {
                        y += max_glyph_height_for_row;
                        x = 0;
                        max_glyph_height_for_row = 0;
                    }

                    font->infos[e.index].ax = static_cast<float>(special_face->glyph->advance.x >> 6);
                    font->infos[e.index].ay = static_cast<float>(special_face->glyph->advance.y >> 6);
                    font->infos[e.index].aw = static_cast<float>(bitmap_atlas_width(special_face->glyph->bitmap));
                    font->infos[e.index].bw = static_cast<float>(bitmap_width(special_face->glyph->bitmap));
                    font->infos[e.index].bh = static_cast<float>(special_face->glyph->bitmap.rows);
                    font->infos[e.index].bl = static_cast<float>(special_face->glyph->bitmap_left);
                    font->infos[e.index].bt = static_cast<float>(special_face->glyph->bitmap_top);
                    font->infos[e.index].tx = static_cast<float>(x) / static_cast<float>(data->width);
                    font->infos[e.index].ty = static_cast<float>(y) / static_cast<float>(data->height);

                    auto tmp = Arena::temp_begin(scratch.arena);
                    const uint8_t* buffer = buffer_for_glyph_pixels(tmp.arena, &special_face->glyph->bitmap, &blit_buffer);

                    Render::GlyphEntry entry{
                        .offset_x = Render::GlyphOffsetX{ x },
                        .offset_y = Render::GlyphOffsetY{ y },
                        .width = Width(bitmap_atlas_width(special_face->glyph->bitmap)),
                        .height = Height(special_face->glyph->bitmap.rows),
                        .buffer = buffer
                    };
                    Render::submit_glyph_data(data->texture, entry);
                    Arena::temp_end(tmp);

                    x += bitmap_atlas_width(special_face->glyph->bitmap);
                    max_glyph_height_for_row = std::max(max_glyph_height_for_row, static_cast<int>(special_face->glyph->bitmap.rows));
                }
            }

            if (result)
            {
                // This is a bit of a hack, but in order to ensure that the whitespace substitute character is spaced the same
                // as the actual whitespace character, we need to adjust the advance on our special glyph character to get this
                // size.
                const float ax_whitespace_target = font->infos[' '].ax;
                font->infos[special_glyph_map[rep(SpecialGlyph::Whitespace)].index].ax = ax_whitespace_target;

                // Start on the row just under the standard glyphs.
                data->unicode_row_start = y + max_glyph_height_for_row;
                data->next_y = data->unicode_row_start;
                data->next_x = 0;
                data->cur_row_max_height = 0;
            }

            Arena::scratch_end(scratch);
            return result;
        }

        template <typename Reporter>
        bool try_set_font_size(Atlas::Data* data, int size, Reporter&& reporter)
        {
            auto [font, inserted] = try_push_cached_fonts_map_font(data->cached_fonts_arena, &data->cached_fonts, size);
            data->selected_font = font;
            if (not inserted)
                return true;
            // Let's also clear the existing unicode cache.
            data->selected_font->font_size = size;
            data->selected_font->glyph_map_arena = Arena::alloc(Arena::default_params);
            data->selected_font->cached_glyphs_map = make_unicode_glyph_map(
                                                            data->selected_font->glyph_map_arena,
                                                            KB(1));
            return populate_standard_glyphs(data,
                                            data->selected_font,
                                            reporter);
        }

        template <typename Reporter>
        FTFaceHandle try_load_font_asset(FT_Library library, Arena::Arena* arena, LoadedFontBuffer* face_buf, Assets::AssetID font_asset, Reporter&& reporter)
        {
            face_buf->size = rep(Assets::asset_length(font_asset));
            face_buf->data = Arena::push_array<FT_Byte>(arena, face_buf->size);
            Assets::AssetBuffer ass_buf{ .buf = face_buf->data, .len = face_buf->size };
            if (not Assets::populate_asset(ass_buf, font_asset))
            {
                char fmt_buf[1024];
                auto desc = Assets::describe(font_asset);
                String8 msg = fmt_string(fmt_buf, "Failed to load asset font '%S'\n", desc.proxy_file);
                reporter(msg);
                return { };
            }
            FT_Face face{ };
            auto file_size = static_cast<FT_Long>(face_buf->size);
            auto error = FT_New_Memory_Face(library,
                                        face_buf->data,
                                        file_size,
                                        0,
                                        &face);
            if (error != 0)
            {
                char fmt_buf[1024];
                const char* log = FT_Error_String(error);
                auto desc = Assets::describe(font_asset);
                String8 msg = fmt_string(fmt_buf, "Failed to load font file '%S': %s", desc.proxy_file, log);
                reporter(msg);
                return { };
            }
            return FTFaceHandle{ face };
        }

        constexpr Vec4f sentinel_color = hex_to_vec4f(0x00000000);
        constexpr Vec4f white_pixel = hex_to_vec4f(0xFFFFFFFF);

        constexpr void render_glyph_empty(CmdBuffer::DrawList*,
                                            Render::FragShader,
                                            const Vec2f&,
                                            const Vec2f&,
                                            const Vec2f&,
                                            const Vec2f&,
                                            const Vec4f&,
                                            float)
        {
        }

        void render_glyph_no_subpix(CmdBuffer::DrawList* lst,
                                    Render::FragShader frag,
                                    const Vec2f& pos,
                                    const Vec2f& size,
                                    const Vec2f& uv_pos,
                                    const Vec2f& uv_size,
                                    const Vec4f& color,
                                    float)
        {
            CmdBuffer::render_glyph(lst, frag, pos, size, uv_pos, uv_size, color);
        }

        using RenderGlyphSelectFn = void(*)(CmdBuffer::DrawList*,
                                            Render::FragShader,
                                            const Vec2f&,
                                            const Vec2f&,
                                            const Vec2f&,
                                            const Vec2f&,
                                            const Vec4f&,
                                            float);

        RenderGlyphSelectFn render_glyph_selector[] =
        {
            render_glyph_empty,
            CmdBuffer::render_subpixel_glyph
        };
    } // namespace [anon]

    Atlas::Atlas():
        data{ new Data } { }

    Atlas::~Atlas()
    {
        for EachNode(n, data->fallback_fonts.first)
        {
            if (n->face != nullptr)
            {
                FT_Done_Face(n->face);
            }
        }
    }

    RenderFontContext::RenderFontContext(Atlas* atlas, CachedFont* font):
        atlas{ atlas },
        font{ font },
        tabs{ 1 },
        colors{ .whitespace = sentinel_color,
                .carriage_return = sentinel_color,
                .color_glyph_bg = white_pixel },
        render_ws{ false },
        mod_colored_glyphs{ false }
    { }

    bool Atlas::init(Feed::MessageFeed* feed)
    {
        PROF_SCOPE();

        // TODO: Separate arena for mapping glyphs.
        data->metadata_arena = Arena::alloc(Arena::default_params);
        data->font_face_arena = Arena::alloc(Arena::default_params);
        data->cached_fonts_arena = Arena::alloc(Arena::default_params);
        data->cached_fonts = make_cached_fonts_map(data->cached_fonts_arena, 32);
        data->text_shader = Config::system_effects().subpixel_font_aa
                                ? Render::FragShader::TextSubpixel
                                : Render::FragShader::Text;
        const auto& system_fonts = Config::system_fonts();
        String8 font_path = system_fonts.current_font;
        FT_Library lib;
        auto error = FT_Init_FreeType(&lib);
        if (error != 0)
        {
            char fmt_buf[1024];
            const char* log = FT_Error_String(error);
            String8 msg = fmt_string(fmt_buf, "Failed to initialize FreeType2 library: %s", log);
            feed->queue_error(msg);
            return false;
        }
        data->library = FTLibraryHandle{ lib };

        FT_Face face{ };
        if (font_path.size != 0)
        {
            error = FT_New_Face(data->library.handle(), font_path.str, 0, &face);
            if (error != 0)
            {
                char fmt_buf[1024];
                const char* log = FT_Error_String(error);
                String8 msg = fmt_string(fmt_buf, "Failed to load font file '%S': %s", font_path, log);
                feed->queue_error(msg);
                feed->queue_warning("Defaulting to builtin font...");
            }
            else
            {
                data->face = FTFaceHandle{ face };
            }
        }

        if (not data->face.valid())
        {
            auto new_face = try_load_font_asset(data->library.handle(), data->font_face_arena, &data->font_face_buf, Assets::AssetID::FontDefault, standard_reporter);
            if (not new_face.valid())
                return false;
            data->face = std::move(new_face);
        }

        constexpr FT_UInt pixel_size = Data::default_font_size;
        bool resize_success = resize_font(data->face.handle(), pixel_size, standard_reporter);

        // We don't need to worry as much about resizing the special glyphs yet.
        {
            constexpr auto icon_asset = Assets::AssetID::FontIcons;
            data->special_face_buf.size = rep(Assets::asset_length(icon_asset));
            data->special_face_buf.data = Arena::push_array<FT_Byte>(data->metadata_arena, data->special_face_buf.size);
            Assets::AssetBuffer ass_buf{ .buf = data->special_face_buf.data, .len = data->special_face_buf.size };
            if (not Assets::populate_asset(ass_buf, icon_asset))
            {
                auto desc = Assets::describe(icon_asset);
                fprintf(stderr, "Failed to load asset font '%.*s'\n", int(desc.proxy_file.size), desc.proxy_file.str);
                return false;
            }
            face = {};
            auto file_size = static_cast<FT_Long>(data->special_face_buf.size);
            error = FT_New_Memory_Face(data->library.handle(),
                                        data->special_face_buf.data,
                                        file_size,
                                        0,
                                        &face);
            if (error != 0)
            {
                char fmt_buf[1024];
                const char* log = FT_Error_String(error);
                auto desc = Assets::describe(icon_asset);
                String8 msg = fmt_string(fmt_buf, "Failed to load font file '%S': %s\n", desc.proxy_file, log);
                feed->queue_error(msg);
                return false;
            }
            data->special_face = FTFaceHandle{ face };
        }
        return resize_success;
    }

    bool Atlas::populate_atlas()
    {
        PROF_SCOPE();

        // Set the width to the maximum width of the image.
        data->width = texture_width;
        data->height = texture_height;

        ScreenDimensions dim{ Width(data->width), Height(data->height) };
        data->texture = Render::create_glyph_texture(dim);

        // Render the white pixel.
        {
            uint8_t white_px[] = { 255, 255, 255, 255,   255, 255, 255, 255,
                                   255, 255, 255, 255,   255, 255, 255, 255, };
            Render::GlyphEntry entry{
                .offset_x = Render::GlyphOffsetX{ 0 },
                .offset_y = Render::GlyphOffsetY{ 0 },
                .width = Width(1),
                .height = Height(1),
                .buffer = white_px
            };
            Render::submit_glyph_data(data->texture, entry);
            // Offset the first glyph to avoid the white pixel.
            data->next_x = 2;
        }

        return true;
    }

    bool Atlas::rebuild_atlas()
    {
        // Clear out all cached fonts and start a new texture.
        for EachIndex(i, data->cached_fonts.capacity)
        {
            for EachNode(n, data->cached_fonts.buckets[i])
            {
                Arena::release(n->glyph_map_arena);
            }
        }
        Arena::clear(data->cached_fonts_arena);
        data->cached_fonts = make_cached_fonts_map(data->cached_fonts_arena, 32);
        // Reset position trackers.
        data->next_x = 0;
        data->next_y = 0;
        Render::delete_basic_texture(data->texture);
        return populate_atlas();
    }

    void Atlas::try_load_font_face(String8 path, Feed::MessageFeed* feed)
    {
        {
            FT_Face face{ };
            auto error = FT_New_Face(data->library.handle(), path.str, 0, &face);
            if (error != 0)
            {
                char fmt_buf[1024];
                const char* log = FT_Error_String(error);
                String8 msg = fmt_string(fmt_buf, "Failed to load font file '%S': %s", path, log);
                feed->queue_error(msg);
                return;
            }
            auto new_face = FTFaceHandle{ face };

            constexpr FT_UInt pixel_size = Data::default_font_size;
            bool resize_success = resize_font(new_face.handle(),
                                                pixel_size,
                                                [&](String8 view)
                                                {
                                                    feed->queue_error(view);
                                                });
            if (not resize_success)
                return;
            // At this point we can set the new font.
            data->face = std::move(new_face);
        }

        const bool success = rebuild_atlas();

        if (success)
        {
            feed->queue_info("Font loaded.");
        }
    }

    void Atlas::try_load_default_font_asset(Feed::MessageFeed* feed)
    {
        auto reporter = [&](String8 msg) { feed->queue_error(msg); };
        Arena::clear(data->font_face_arena);
        auto new_face = try_load_font_asset(data->library.handle(), data->font_face_arena, &data->font_face_buf, Assets::AssetID::FontDefault, reporter);
        if (not new_face.valid())
            return;
        constexpr FT_UInt pixel_size = Data::default_font_size;
        bool resize_success = resize_font(new_face.handle(),
                                            pixel_size,
                                            reporter);
        if (not resize_success)
            return;
        data->face = std::move(new_face);
        const bool success = rebuild_atlas();
        if (success)
        {
            feed->queue_info("Font loaded.");
        }
    }

    String8 Atlas::font_family() const
    {
        return str8_cstr(data->face.handle()->family_name);
    }

    void Atlas::sync_config()
    {
        const bool subpix_aa = Config::system_effects().subpixel_font_aa;
        const Render::FragShader new_shader = subpix_aa
                                ? Render::FragShader::TextSubpixel
                                : Render::FragShader::Text;
        render_glyph_selector[true] = subpix_aa
                                ? CmdBuffer::render_subpixel_glyph
                                : render_glyph_no_subpix;
        if (data->text_shader != new_shader)
        {
            data->text_shader = new_shader;
            rebuild_atlas();
        }
    }

    Vec2f RenderFontContext::render_text(CmdBuffer::DrawList* lst,
                        String8 text,
                        const Vec2f& pos,
                        const Vec4f& color)
    {
        Vec2f new_pos = pos;
        UTF8::CodepointWalker walker{ sv_str8(text) };
        Vec4f glyph_box;
        while (not walker.exhausted())
        {
            auto cp = walker.next();
            const auto& [info, ax, filter] = extract_glyph_info(atlas->data.get(),
                                                                font,
                                                                tabs,
                                                                cp,
                                                                Rasterize::Yes,
                                                                make_yes_no<RenderWhitespace>(render_ws),
                                                                make_yes_no<ModulateColoredGlyphs>(mod_colored_glyphs));
            float x2 = new_pos.x + info.bl;
            float y2 = -new_pos.y - info.bt;
            float aw = info.aw;
            float w = info.bw;
            float h = info.bh;

            // Compute the subpixel position.
            float snapped_x = x2;
            float subpixel_shift = modff(x2, &snapped_x);

            // Create the bounding box for this glyph.
            glyph_box.p0[0] = new_pos.x;
            glyph_box.p0[1] = new_pos.y;
            glyph_box.p1[0] = new_pos.x + ax;
            glyph_box.p1[1] = new_pos.y + info.ay + h;

            new_pos.x += ax;
            new_pos.y += info.ay;

            auto* filtered_color = filter(&color, &colors);

            const bool inside_clip = not CmdBuffer::box_outside_clip(lst, glyph_box);

            render_glyph_selector[inside_clip](lst,
                                                atlas->data->text_shader,
                                                Vec2f(snapped_x, -y2),
                                                Vec2f(w, -h),
                                                Vec2f(info.tx, info.ty),
                                                Vec2f((aw) / static_cast<float>(atlas->data->width), (h) / static_cast<float>(atlas->data->height)),
                                                *filtered_color,
                                                subpixel_shift);
        }
        return new_pos;
    }

    Vec2f RenderFontContext::render_text(CmdBuffer::DrawList* lst,
                        std::string_view text,
                        const Vec2f& pos,
                        const Vec4f& color)
    {
        return render_text(lst, str8_cppview(text), pos, color);
    }

    Vec2f RenderFontContext::render_text_ignore_clip(CmdBuffer::DrawList* lst,
                        std::string_view text,
                        const Vec2f& pos,
                        const Vec4f& color)
    {
        Vec2f new_pos = pos;
        UTF8::CodepointWalker walker{ text };
        Vec4f glyph_box;
        while (not walker.exhausted())
        {
            auto cp = walker.next();
            const auto& [info, ax, filter] = extract_glyph_info(atlas->data.get(),
                                                                font,
                                                                tabs,
                                                                cp,
                                                                Rasterize::Yes,
                                                                make_yes_no<RenderWhitespace>(render_ws),
                                                                make_yes_no<ModulateColoredGlyphs>(mod_colored_glyphs));
            float x2 = new_pos.x + info.bl;
            float y2 = -new_pos.y - info.bt;
            float aw = info.aw;
            float w = info.bw;
            float h = info.bh;

            // Create the bounding box for this glyph.
            glyph_box.p0[0] = new_pos.x;
            glyph_box.p0[1] = new_pos.y;
            glyph_box.p1[0] = new_pos.x + ax;
            glyph_box.p1[1] = new_pos.y + info.ay + h;

            new_pos.x += ax;
            new_pos.y += info.ay;

            auto* filtered_color = filter(&color, &colors);

            CmdBuffer::render_glyph(lst,
                                    atlas->data->text_shader,
                                    Vec2f(x2, -y2),
                                    Vec2f(w, -h),
                                    Vec2f(info.tx, info.ty),
                                    Vec2f((aw) / static_cast<float>(atlas->data->width), (h) / static_cast<float>(atlas->data->height)),
                                    *filtered_color);
        }
        return new_pos;
    }

    Vec2f RenderFontContext::render_glyph(CmdBuffer::DrawList* lst,
                        UTF8::Codepoint cp,
                        const Vec2f& pos,
                        const Vec4f& color)
    {
        Vec2f new_pos = pos;

        const auto& [info, ax, filter] = extract_glyph_info(atlas->data.get(),
                                                            font,
                                                            tabs,
                                                            cp,
                                                            Rasterize::Yes,
                                                            make_yes_no<RenderWhitespace>(render_ws),
                                                            make_yes_no<ModulateColoredGlyphs>(mod_colored_glyphs));
        float x2 = new_pos.x + info.bl;
        float y2 = -new_pos.y - info.bt;
        float aw = info.aw;
        float w = info.bw;
        float h = info.bh;

        // Compute the subpixel position.
        float snapped_x = x2;
        float subpixel_shift = modff(x2, &snapped_x);

        // Create the bounding box for this glyph.
        Vec4f glyph_box;
        glyph_box.p0[0] = new_pos.x;
        glyph_box.p0[1] = new_pos.y;
        glyph_box.p1[0] = new_pos.x + ax;
        glyph_box.p1[1] = new_pos.y + info.ay + h;

        new_pos.x += ax;
        new_pos.y += info.ay;

        auto* filtered_color = filter(&color, &colors);

        const bool inside_clip = not CmdBuffer::box_outside_clip(lst, glyph_box);

        render_glyph_selector[inside_clip](lst,
                                            atlas->data->text_shader,
                                            Vec2f(snapped_x, -y2),
                                            Vec2f(w, -h),
                                            Vec2f(info.tx, info.ty),
                                            Vec2f((aw) / static_cast<float>(atlas->data->width), (h) / static_cast<float>(atlas->data->height)),
                                            *filtered_color,
                                            subpixel_shift);

        return new_pos;
    }

    Vec2f RenderFontContext::render_icon_glyph(CmdBuffer::DrawList* lst,
                                                SpecialGlyph glyph,
                                                const Vec2f& pos,
                                                const Vec4f& color)
    {
        Vec2f new_pos = pos;

        const auto& info = font->infos[special_glyph_map[rep(glyph)].index];
        float x2 = new_pos.x + info.bl;
        float y2 = -new_pos.y - info.bt;
        float aw = info.aw;
        float w = info.bw;
        float h = info.bh;

        new_pos.x += info.ax;
        new_pos.y += info.ay;

        CmdBuffer::render_glyph(lst,
                                atlas->data->text_shader,
                                Vec2f(x2, -y2),
                                Vec2f(w, -h),
                                Vec2f(info.tx, info.ty),
                                Vec2f((aw) / static_cast<float>(atlas->data->width), (h) / static_cast<float>(atlas->data->height)),
                                color);

        return new_pos;
    }

    Vec2f RenderFontContext::render_glyph_no_offsets(CmdBuffer::DrawList* lst,
                    UTF8::Codepoint cp,
                    const Vec2f& pos,
                    const Vec4f& color)
    {
        Vec2f new_pos = pos;

        const auto& [info, ax, filter] = extract_glyph_info(atlas->data.get(),
                                                            font,
                                                            tabs,
                                                            cp,
                                                            Rasterize::Yes,
                                                            make_yes_no<RenderWhitespace>(render_ws),
                                                            make_yes_no<ModulateColoredGlyphs>(mod_colored_glyphs));
        float x2 = new_pos.x;
        float y2 = -new_pos.y;
        float aw = info.aw;
        float w = info.bw;
        float h = info.bh;

        new_pos.x += ax;
        new_pos.y += info.ay;

        auto* filtered_color = filter(&color, &colors);

        CmdBuffer::render_glyph(lst,
                                atlas->data->text_shader,
                                Vec2f(x2, -y2),
                                Vec2f(w, -h),
                                Vec2f(info.tx, info.ty),
                                Vec2f((aw) / static_cast<float>(atlas->data->width), (h) / static_cast<float>(atlas->data->height)),
                                *filtered_color);

        return new_pos;
    }

    Vec2f RenderFontContext::render_icon_glyph_no_offsets(CmdBuffer::DrawList* lst,
                                                            SpecialGlyph glyph,
                                                            const Vec2f& pos,
                                                            const Vec4f& color)
    {
        Vec2f new_pos = pos;

        const auto& info = font->infos[special_glyph_map[rep(glyph)].index];
        float x2 = new_pos.x;
        float y2 = -new_pos.y;
        float aw = info.aw;
        float w = info.bw;
        float h = info.bh;

        new_pos.x += info.ax;
        new_pos.y += info.ay;

        CmdBuffer::render_glyph(lst,
                                atlas->data->text_shader,
                                Vec2f(x2, -y2),
                                Vec2f(w, -h),
                                Vec2f(info.tx, info.ty),
                                Vec2f((aw) / static_cast<float>(atlas->data->width), (h) / static_cast<float>(atlas->data->height)),
                                color);

        return new_pos;
    }

    Vec2f RenderFontContext::render_icon_glyph_no_offsets_rotation(CmdBuffer::DrawList* lst,
                                                                    SpecialGlyph glyph,
                                                                    float rotation,
                                                                    const Vec2f& pos,
                                                                    const Vec4f& color)
    {
        Vec2f new_pos = pos;

        const auto& info = font->infos[special_glyph_map[rep(glyph)].index];
        float x2 = new_pos.x;
        float y2 = -new_pos.y;
        float aw = info.aw;
        float w = info.bw;
        float h = info.bh;

        // Now that we know the size, we can apply rotation.
        CmdBuffer::QuadInput quad_in = {
            .p0123 = {
                { x2,     -y2 },
                { x2 + w, -y2 },
                { x2 + w, -y2 + -h },
                { x2,     -y2 + -h },
            }
        };
        float cos_a = std::cosf(rotation * 2.f * Constants::pi32);
        float sin_a = std::sinf(rotation * 2.f * Constants::pi32);
        Vec2f center = { quad_in.p0123[1].x - quad_in.p0123[0].x, quad_in.p0123[2].y - quad_in.p0123[0].y };
        center.x = quad_in.p0123[0].x + center.x / 2.f;
        center.y = quad_in.p0123[2].y - center.y / 2.f;
        for EachIndex(i, 4)
        {
            Vec2f v = quad_in.p0123[i];
            v = v - center;
            quad_in.p0123[i].x = center.x + (v.x * cos_a - v.y * sin_a);
            quad_in.p0123[i].y = center.y + (v.x * sin_a + v.y * cos_a);
        }
        CmdBuffer::quad_image(lst,
                                atlas->data->text_shader,
                                quad_in,
                                Vec2f(info.tx, info.ty),
                                Vec2f((aw) / static_cast<float>(atlas->data->width), (h) / static_cast<float>(atlas->data->height)),
                                color);

        new_pos.x += info.ax;
        new_pos.y += info.ay;

        return new_pos;
    }

    Vec2f RenderFontContext::measure_text(String8 text)
    {
        Vec2f size{};
        UTF8::CodepointWalker walker{ sv_str8(text) };
        while (not walker.exhausted())
        {
            UTF8::Codepoint glyph_index = walker.next();
            const auto& [info, ax, filter] = extract_glyph_info(atlas->data.get(),
                                                                font,
                                                                tabs,
                                                                glyph_index,
                                                                Rasterize::No,
                                                                make_yes_no<RenderWhitespace>(render_ws),
                                                                make_yes_no<ModulateColoredGlyphs>(mod_colored_glyphs));
            size.x += ax;
            size.y += info.ay;
        }
        return size;
    }

    Vec2f RenderFontContext::measure_text(std::string_view text)
    {
        return measure_text(str8_cppview(text));
    }

    Vec2f RenderFontContext::measure_scaled_text(std::string_view text, float scalar)
    {
        Vec2f size{};
        UTF8::CodepointWalker walker{ text };
        while (not walker.exhausted())
        {
            UTF8::Codepoint glyph_index = walker.next();
            const auto& [info, ax, filter] = extract_glyph_info(atlas->data.get(),
                                                                font,
                                                                tabs,
                                                                glyph_index,
                                                                Rasterize::No,
                                                                make_yes_no<RenderWhitespace>(render_ws),
                                                                make_yes_no<ModulateColoredGlyphs>(mod_colored_glyphs));
            size.x += ax * scalar;
            size.y += info.ay * scalar;
        }
        return size;
    }

    Vec2f RenderFontContext::glyph_size(UTF8::Codepoint cp)
    {
        Vec2f size{};
        const auto& [info, ax, filter] = extract_glyph_info(atlas->data.get(),
                                                            font,
                                                            tabs,
                                                            cp,
                                                            Rasterize::No,
                                                            make_yes_no<RenderWhitespace>(render_ws),
                                                            make_yes_no<ModulateColoredGlyphs>(mod_colored_glyphs));
        size.x = info.bw;
        size.y = info.bh;
        return size;
    }

    Vec2f RenderFontContext::icon_glyph_size(SpecialGlyph glyph)
    {
        Vec2f size{};
        const auto& info = font->infos[special_glyph_map[rep(glyph)].index];
        size.x = info.bw;
        size.y = info.bh;
        return size;
    }

    size_t RenderFontContext::glyph_count_to_point(std::string_view text, float x_point)
    {
        size_t count = 0;
        float running_length = 0.f;
        UTF8::CodepointWalker walker{ text };
        while (not walker.exhausted())
        {
            UTF8::Codepoint glyph_index = walker.next();
            const auto& [info, ax, filter] = extract_glyph_info(atlas->data.get(),
                                                                font,
                                                                tabs,
                                                                glyph_index,
                                                                Rasterize::No,
                                                                make_yes_no<RenderWhitespace>(render_ws),
                                                                make_yes_no<ModulateColoredGlyphs>(mod_colored_glyphs));
            running_length += ax;
            if (running_length >= x_point)
            {
                // Let's do something nice.  If the point is > 50% of this glyph width, then we
                // will move the count forward.
                const float threshold = ax / 2.f;
                const float threshold_length = (running_length - info.ax) + threshold;
                if (threshold_length >= x_point)
                    return count;
            }
            ++count;
        }
        return count;
    }

    int RenderFontContext::current_font_size()
    {
        return font->font_size;
    }

    int RenderFontContext::current_font_line_height()
    {
        // The line height is always relative to the known default font size.
        constexpr double target_pct = 25. / Atlas::Data::default_font_size;
        const int padding = static_cast<int>(target_pct * font->font_size);
        return font->font_size + padding;
    }

    void RenderFontContext::tabstop(Tabstop ts)
    {
        tabs = ts;
    }

    void RenderFontContext::whitespace_color(const Vec4f& color)
    {
        colors.whitespace = color;
    }

    void RenderFontContext::carriage_return_color(const Vec4f& color)
    {
        colors.carriage_return = color;
    }

    void RenderFontContext::render_whitespace(bool b)
    {
        render_ws = b;
    }

    void RenderFontContext::modulate_colored_glyphs(bool b)
    {
        mod_colored_glyphs = b;
    }

    Render::BasicTexture RenderFontContext::atlas_texture() const
    {
        return atlas->atlas_texture();
    }

    RenderFontContext Atlas::render_font_context(FontSize size)
    {
        try_set_font_size(data.get(), rep(size), standard_reporter);
        return { this, data->selected_font };
    }

    Render::BasicTexture Atlas::atlas_texture() const
    {
        return data->texture;
    }
} // namespace Glyph