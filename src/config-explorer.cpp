#include "config-explorer.h"

#include <vector>

#include "basic-checkbox.h"
#include "basic-colorbox.h"
#include "basic-colorpicker.h"
#include "basic-scrollbox.h"
#include "config.h"
#include "fuzzy-search.h"
#include "gap-strings.h"

namespace Config
{
    using namespace UI;
    namespace
    {
        using Description = FuzzySearch::FuzzyName;

        struct CheckboxOption
        {
            Description name;
            bool state;
            UI::Widgets::ID id;
        };

        using CheckboxOptions = std::vector<CheckboxOption>;

#define DAT_CAT_START(T, a, comp, path) void fill_ ## a ## _chk ([[maybe_unused]] CheckboxOptions* opts) { \
    [[maybe_unused]] int i = 0; \
    [[maybe_unused]] const T&(*access)() = Config:: a;
#define DAT_TOGGLE(name, T, desc) (*opts)[i++].state = access().name;
#define DAT_CAT_END() }

#define DAT_COLOR(name, T, desc)
#define DAT_FONTSIZE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_FILE
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

#define DAT_CAT_START(T, a, comp, path) void write_ ## a ## _chk ([[maybe_unused]] CheckboxOptions* opts) { \
    [[maybe_unused]] int i = 0; \
    [[maybe_unused]] T target = Config:: a();
#define DAT_TOGGLE(name, T, desc) target.name = (*opts)[i++].state;
#define DAT_CAT_END() Config::update(target); }

#define DAT_COLOR(name, T, desc)
#define DAT_FONTSIZE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_FILE
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

        struct FontSizeValue
        {
            Description name;
            int size;
            UI::Widgets::ID id;
        };

        using FontSizeValues = std::vector<FontSizeValue>;

#define DAT_CAT_START(T, a, comp, path) void fill_ ## a ## _fntsz ([[maybe_unused]] FontSizeValues* opts) { \
    [[maybe_unused]] int i = 0; \
    [[maybe_unused]] const T&(*access)() = Config:: a;
#define DAT_FONTSIZE(name, T, desc) (*opts)[i++].size = access().name;
#define DAT_CAT_END() }

#define DAT_COLOR(name, T, desc)
#define DAT_TOGGLE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

#define DAT_CAT_START(T, a, comp, path) void write_ ## a ## _fntsz ([[maybe_unused]] FontSizeValues* opts) { \
    [[maybe_unused]] int i = 0; \
    [[maybe_unused]] T target = Config:: a();
#define DAT_FONTSIZE(name, T, desc) target.name = (*opts)[i++].size;
#define DAT_CAT_END() Config::update(target); }

#define DAT_COLOR(name, T, desc)
#define DAT_TOGGLE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_FILE
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

        struct PathValue
        {
            Description name;
            String8 path;
            UI::Widgets::ID id;
        };

        using PathValues = std::vector<PathValue>;

#define DAT_CAT_START(T, a, comp, path) void fill_ ## a ## _path ([[maybe_unused]] PathValues* opts) { \
    [[maybe_unused]] int i = 0; \
    [[maybe_unused]] const T&(*access)() = Config:: a;
#define DAT_PATH(name, T, desc) (*opts)[i++].path = access().name;
#define DAT_CAT_END() }

#define DAT_COLOR(name, T, desc)
#define DAT_FONTSIZE(name, T, desc)
#define DAT_TOGGLE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

#define DAT_CAT_START(T, a, comp, path) void write_ ## a ## _path ([[maybe_unused]] PathValues* opts) { \
    [[maybe_unused]] int i = 0; \
    [[maybe_unused]] T target = Config:: a();
#define DAT_PATH(name, T, desc) if (not str8_match_exact(target.name, (*opts)[i].path)) /* Update only if there's a change.*/ \
    {                                                                                                                         \
        Config::update_string(&target.name, (*opts)[i].path);                                                                 \
        (*opts)[i++].path = target.name;                                                                                      \
    }
#define DAT_CAT_END() Config::update(target); }

#define DAT_COLOR(name, T, desc)
#define DAT_FONTSIZE(name, T, desc)
#define DAT_TOGGLE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_FILE
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

        struct ColorValue
        {
            Description name;
            Vec4f color;
            UI::Widgets::ID id;
        };

        using ColorValues = std::vector<ColorValue>;

#define DAT_CAT_START(T, a, comp, path) void fill_ ## a ## _color ([[maybe_unused]] ColorValues* opts) { \
    [[maybe_unused]] int i = 0; \
    [[maybe_unused]] const T&(*access)() = Config:: a;
#define DAT_COLOR(name, T, desc) (*opts)[i++].color = access().name;
#define DAT_CAT_END() }

#define DAT_FONTSIZE(name, T, desc)
#define DAT_TOGGLE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_FILE
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

#define DAT_CAT_START(T, a, comp, path) void write_ ## a ## _color ([[maybe_unused]] ColorValues* opts) { \
    [[maybe_unused]] int i = 0; \
    [[maybe_unused]] T target = Config:: a();
#define DAT_COLOR(name, T, desc) target.name = (*opts)[i++].color;
#define DAT_CAT_END() Config::update(target); }

#define DAT_TOGGLE(name, T, desc)
#define DAT_FONTSIZE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

        struct CheckboxCollection
        {
            std::string_view component;
            std::string_view component_path;
            void (*fill)(CheckboxOptions*);
            void (*save)(CheckboxOptions*);
            CheckboxOptions checkboxes;
        };

        using CheckboxCollections = std::vector<CheckboxCollection>;

#define DAT_CAT_START(T, a, comp, path) void fill_chk_ ## a ## _collection(CheckboxCollection* c) {               \
 c->component = comp; c->component_path = path; c->fill = fill_ ## a ## _chk; c->save = write_ ## a ## _chk;
#define DAT_TOGGLE(name, T, desc)     \
 c->checkboxes.emplace_back(          \
  FuzzySearch::make_fuzzy_name(desc), \
  false,                              \
  UI::Widgets::make_id_seed(          \
   UI::Widgets::make_id_seed(         \
    UI::Widgets::ID::ConfigExplorer,  \
    c->component_path),               \
   #name));
#define DAT_CAT_END() }

#define DAT_COLOR(name, T, desc)
#define DAT_FONTSIZE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_FILE
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

        struct FontSizeCollection
        {
            std::string_view component;
            std::string_view component_path;
            void (*fill)(FontSizeValues*);
            void (*save)(FontSizeValues*);
            FontSizeValues font_sizes;
        };

        using FontSizeCollections = std::vector<FontSizeCollection>;

#define DAT_CAT_START(T, a, comp, path) void fill_fntsz_ ## a ## _collection(FontSizeCollection* c) { \
 c->component = comp; c->component_path = path; c->fill = fill_ ## a ## _fntsz; c->save = write_ ## a ## _fntsz;
#define DAT_FONTSIZE(name, T, desc)   \
 c->font_sizes.emplace_back(          \
  FuzzySearch::make_fuzzy_name(desc), \
  1,                                  \
  UI::Widgets::make_id_seed(          \
   UI::Widgets::make_id_seed(         \
    UI::Widgets::ID::ConfigExplorer,  \
    c->component_path),               \
   #name));
#define DAT_CAT_END() }

#define DAT_COLOR(name, T, desc)
#define DAT_TOGGLE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_FILE
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

        struct PathCollection
        {
            std::string_view component;
            std::string_view component_path;
            void (*fill)(PathValues*);
            void (*save)(PathValues*);
            PathValues paths;
        };

        using PathCollections = std::vector<PathCollection>;

#define DAT_CAT_START(T, a, comp, path) void fill_path_ ## a ## _collection(PathCollection* c) { \
 c->component = comp; c->component_path = path; c->fill = fill_ ## a ## _path; c->save = write_ ## a ## _path;
#define DAT_PATH(name, T, desc)       \
 c->paths.emplace_back(               \
  FuzzySearch::make_fuzzy_name(desc), \
  str8_empty,                         \
  UI::Widgets::make_id_seed(          \
   UI::Widgets::make_id_seed(         \
    UI::Widgets::ID::ConfigExplorer,  \
    c->component_path),               \
   #name));
#define DAT_CAT_END() }

#define DAT_COLOR(name, T, desc)
#define DAT_FONTSIZE(name, T, desc)
#define DAT_TOGGLE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_FILE
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

        struct ColorCollection
        {
            std::string_view component;
            std::string_view component_path;
            void (*fill)(ColorValues*);
            void (*save)(ColorValues*);
            ColorValues colors;
        };

        using ColorCollections = std::vector<ColorCollection>;

#define DAT_CAT_START(T, a, comp, path) void fill_color_ ## a ## _collection(ColorCollection* c) { \
 c->component = comp; c->component_path = path; c->fill = fill_ ## a ## _color; c->save = write_ ## a ## _color;
#define DAT_COLOR(name, T, desc)      \
 c->colors.emplace_back(              \
  FuzzySearch::make_fuzzy_name(desc), \
  0.f,                                \
  UI::Widgets::make_id_seed(          \
   UI::Widgets::make_id_seed(         \
    UI::Widgets::ID::ConfigExplorer,  \
    c->component_path),               \
   #name));
#define DAT_CAT_END() }

#define DAT_FONTSIZE(name, T, desc)
#define DAT_TOGGLE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

        CheckboxCollections checkbox_collection;
        FontSizeCollections font_size_collection;
        PathCollections path_collection;
        ColorCollections color_collection;

        void init_collections()
        {
#define DAT_CAT_START(T, a, comp, path) checkbox_collection.push_back({}); \
 font_size_collection.push_back({});                                       \
 path_collection.push_back({});                                            \
 color_collection.push_back({});                                           \
 fill_chk_ ## a ## _collection(&checkbox_collection.back());               \
 fill_fntsz_ ## a ## _collection(&font_size_collection.back());            \
 fill_path_ ## a ## _collection(&path_collection.back());                  \
 fill_color_ ## a ## _collection(&color_collection.back());
#define DAT_CAT_END()

#define DAT_COLOR(name, T, desc)
#define DAT_FONTSIZE(name, T, desc)
#define DAT_TOGGLE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_SOUND(name, T, desc)
#define DAT_RECT_I(name, T, desc)
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_TOGGLE
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_FILE
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I
        }

        enum class CheckboxEntry : size_t
        {
            Sentinel = sentinel_for<CheckboxEntry>
        };

        enum class FontSizeEntry : size_t
        {
            Sentinel = sentinel_for<FontSizeEntry>
        };

        enum class PathEntry : size_t
        {
            Sentinel = sentinel_for<PathEntry>
        };

        enum class ColorEntry : size_t
        {
            Sentinel = sentinel_for<ColorEntry>
        };

        CheckboxCollection* fetch(CheckboxEntry e)
        {
            return &checkbox_collection[rep(e)];
        }

        FontSizeCollection* fetch(FontSizeEntry e)
        {
            return &font_size_collection[rep(e)];
        }

        PathCollection* fetch(PathEntry e)
        {
            return &path_collection[rep(e)];
        }

        ColorCollection* fetch(ColorEntry e)
        {
            return &color_collection[rep(e)];
        }

        struct Component
        {
            std::string_view component;
            CheckboxEntry checkboxes = CheckboxEntry::Sentinel;
            FontSizeEntry font_sizes = FontSizeEntry::Sentinel;
            PathEntry paths = PathEntry::Sentinel;
            ColorEntry colors = ColorEntry::Sentinel;
        };

        using Components = std::vector<Component>;

        using EntryName = FuzzySearch::FuzzyName;

        using RenderCheckboxes = FuzzySearch::FuzzySearchResults<CheckboxOption>;
        using RenderFontSizes = FuzzySearch::FuzzySearchResults<FontSizeValue>;
        using RenderPaths = FuzzySearch::FuzzySearchResults<PathValue>;
        using RenderColors = FuzzySearch::FuzzySearchResults<ColorValue>;

        struct RenderComponent
        {
            std::string_view component;
            RenderCheckboxes checkboxes;
            RenderFontSizes font_sizes;
            RenderPaths paths;
            RenderColors colors;
            Vec2f size;
            float y_offset;
        };

        using RenderComponentsCollection = std::vector<RenderComponent>;

        struct RenderComponents
        {
            RenderComponentsCollection collection;
            float title_height;
            float checkbox_height;
            float font_size_height;
            float path_height;
            float color_height;
        };

        void copy_as_render_components(Components* components, RenderComponents* render_components)
        {
            render_components->collection.clear();
            render_components->collection.reserve(components->size());
            for (auto& c : *components)
            {
                render_components->collection.push_back({});
                auto& new_c = render_components->collection.back();
                new_c.component = c.component;

                if (c.checkboxes != CheckboxEntry::Sentinel)
                {
                    CheckboxCollection* cb_collection = fetch(c.checkboxes);
                    FuzzySearch::reset_states(&cb_collection->checkboxes);
                    for (auto& cb : cb_collection->checkboxes)
                    {
                        new_c.checkboxes.push_back({ .entry = &cb, .score = 0 });
                    }
                }

                if (c.font_sizes != FontSizeEntry::Sentinel)
                {
                    FontSizeCollection* fz_collection = fetch(c.font_sizes);
                    FuzzySearch::reset_states(&fz_collection->font_sizes);
                    for (auto& fz : fz_collection->font_sizes)
                    {
                        new_c.font_sizes.push_back({ .entry = &fz, .score = 0 });
                    }
                }

                if (c.paths != PathEntry::Sentinel)
                {
                    PathCollection* p_collection = fetch(c.paths);
                    FuzzySearch::reset_states(&p_collection->paths);
                    for (auto& path : p_collection->paths)
                    {
                        new_c.paths.push_back({ .entry = &path, .score = 0 });
                    }
                }

                if (c.colors != ColorEntry::Sentinel)
                {
                    ColorCollection* col_collection = fetch(c.colors);
                    FuzzySearch::reset_states(&col_collection->colors);
                    for (auto& color : col_collection->colors)
                    {
                        new_c.colors.push_back({ .entry = &color, .score = 0 });
                    }
                }
            }
        }

        void apply_search_filter(Components* components, RenderComponents* render_components, std::string_view filter)
        {
            render_components->collection.clear();
            render_components->collection.reserve(components->size());
            for (auto& c : *components)
            {
                render_components->collection.push_back({});
                auto& new_c = render_components->collection.back();
                new_c.component = c.component;

                if (c.checkboxes != CheckboxEntry::Sentinel)
                {
                    CheckboxCollection* cb_collection = fetch(c.checkboxes);
                    FuzzySearch::populate_fuzzy_search_results(&cb_collection->checkboxes, &new_c.checkboxes, filter);
                }

                if (c.font_sizes != FontSizeEntry::Sentinel)
                {
                    FontSizeCollection* fz_collection = fetch(c.font_sizes);
                    FuzzySearch::populate_fuzzy_search_results(&fz_collection->font_sizes, &new_c.font_sizes, filter);
                }

                if (c.paths != PathEntry::Sentinel)
                {
                    PathCollection* p_collection = fetch(c.paths);
                    FuzzySearch::populate_fuzzy_search_results(&p_collection->paths, &new_c.paths, filter);
                }

                if (c.colors != ColorEntry::Sentinel)
                {
                    ColorCollection* col_collection = fetch(c.colors);
                    FuzzySearch::populate_fuzzy_search_results(&col_collection->colors, &new_c.colors, filter);
                }

                // Prune the entire section.
                bool has_entry = not new_c.checkboxes.empty()
                                    or not new_c.font_sizes.empty()
                                    or not new_c.paths.empty()
                                    or not new_c.colors.empty();
                if (not has_entry)
                {
                    render_components->collection.pop_back();
                }
            }
        }

        struct HoveredEntry
        {
            CheckboxOption* checkbox = nullptr;
            FontSizeValue* font_size = nullptr;
            PathValue* path = nullptr;
            ColorValue* color = nullptr;
        };

        bool has_hover(const HoveredEntry& x)
        {
            return x.checkbox != nullptr
                    or x.font_size != nullptr
                    or x.path != nullptr
                    or x.color != nullptr;
        }

        struct ColorPickData
        {
            bool picking_color = false;
            ColorValue* color = nullptr;
        };

        struct PathChangeData
        {
            PathValue* path = nullptr;
        };

        struct UIData
        {
            ColorPickData color_pick_data;
            PathChangeData path_change_data;
            float wheel_offset_amount = 0.f;
        };
    } // namespace [anon]

    struct Explorer::Data
    {
        Widgets::BasicWindow window;
        Widgets::ScrollBox scrollbox;
        Widgets::ColorPicker color_picker;
        Glyph::Atlas* atlas;
        Glyph::FontSize font_size = Glyph::FontSize{ 12 };
        Components components;
        RenderComponents render_components;
        UIData ui_data;

        static constexpr int padding = 2;
    };

    namespace
    {
        Render::RenderViewport initial_window_viewport(const ScreenDimensions& screen)
        {
            auto viewport = Render::RenderViewport::basic(screen);
            viewport.width = Width{ rep(viewport.width) / 2 };
            viewport.height = Height{ rep(viewport.height) / 2 };
            viewport.offset_x = Render::ViewportOffsetX{ (rep(screen.width) - rep(viewport.width)) / 2 };
            viewport.offset_y = Render::ViewportOffsetY{ rep(viewport.height) / 2 };
            return viewport;
        }

        struct WindowContentViewports
        {
            Render::RenderViewport scroll_vp;
        };

        WindowContentViewports window_content_viewports(const Render::RenderViewport& viewport)
        {
            auto scroll_vp = viewport;
            return { .scroll_vp = scroll_vp };
        }

        float title_base_height(Glyph::RenderFontContext* font_ctx)
        {
            return font_ctx->current_font_size() + Explorer::Data::padding * 2 + 0.f;
        }

        float checkbox_base_height(Glyph::RenderFontContext* font_ctx)
        {
            Widgets::CheckboxInput cb_in =
            {
                .label = "",
                .checked = Widgets::Checked::Yes
            };
            return Widgets::measure_checkbox(font_ctx, cb_in).y + Explorer::Data::padding;
        }

        float font_size_base_height(Glyph::RenderFontContext* font_ctx)
        {
            Vec2f rect_size{ 0.f, font_ctx->current_font_line_height() + 0.f };
            return rect_size.y * 2 + Explorer::Data::padding * 2;
        }

        float path_base_height(Glyph::RenderFontContext* font_ctx)
        {
            Vec2f rect_size{ 0.f, font_ctx->current_font_line_height() + 0.f };
            return rect_size.y * 2 + Explorer::Data::padding * 2;
        }

        float colorbox_base_height(Glyph::RenderFontContext* font_ctx)
        {
            Widgets::ColorboxData colorbox_data =
            {
                .label = "",
                .color = {}
            };
            return Widgets::measure_colorbox(font_ctx, colorbox_data).y + Explorer::Data::padding;
        }

        Vec2f content_size(Explorer::Data* data)
        {
            auto font_ctx = data->atlas->render_font_context(data->font_size);
            // Keep total_size 'x' as 0 so we don't spawn a horizontal scrollbar.
            Vec2f total_size{ 0.f, 0.f };
            Vec2f current_size = total_size;

            data->render_components.title_height = title_base_height(&font_ctx);
            data->render_components.checkbox_height = checkbox_base_height(&font_ctx);
            data->render_components.font_size_height = font_size_base_height(&font_ctx);
            data->render_components.path_height = path_base_height(&font_ctx);
            data->render_components.color_height = colorbox_base_height(&font_ctx);
            for (auto& comp : data->render_components.collection)
            {
                current_size.y = data->render_components.title_height;
                // Checkbox.
                {
                    current_size.y += (data->render_components.checkbox_height) * static_cast<int>(comp.checkboxes.size());
                }

                // Font size.
                {
                    current_size.y += (data->render_components.font_size_height) * static_cast<int>(comp.font_sizes.size());
                }

                // Path.
                {
                    current_size.y += (data->render_components.path_height) * static_cast<int>(comp.paths.size());
                }

                // Color.
                {
                    current_size.y += (data->render_components.color_height) * static_cast<int>(comp.colors.size());
                }

                comp.size = current_size;
                // This is the top of the previous rect stacked.
                comp.y_offset = total_size.y;

                // We really only care about the height additions.
                total_size.y += current_size.y;
            }
            return total_size;
        }

        void setup_ui_data(Explorer::Data* data)
        {
            data->ui_data = {};
            data->ui_data.wheel_offset_amount = UI::standard_font_padding(data->font_size) * 2;
            data->window.background_alpha(0.8f);
            data->scrollbox.scroll_to(0.f);
            copy_as_render_components(&data->components, &data->render_components);
        }

        Component* find_or_create_component(Explorer::Data* data, std::string_view component)
        {
            auto itr = std::find_if(begin(data->components),
                                    end(data->components),
                                    [&](const Component& c)
                                    {
                                        return c.component == component;
                                    });
            if (itr != end(data->components))
                return &*itr;
            data->components.push_back({ .component = component });
            return &data->components.back();
        }

        void populate_components(Explorer::Data* data)
        {
            // We will suppress any kind of computed values while setting these so we can pull the 'real' ones set
            // by the user.
            Config::allow_computed_colors(false);
            // Checkboxes.
            for (size_t i = 0; i != checkbox_collection.size(); ++i)
            {
                CheckboxCollection* checkbox = fetch(CheckboxEntry{ i });
                if (checkbox->checkboxes.empty())
                    continue;
                checkbox->fill(&checkbox->checkboxes);
                Component* comp = find_or_create_component(data, checkbox->component);
                comp->checkboxes = CheckboxEntry{ i };
            }

            // Font sizes.
            for (size_t i = 0; i != font_size_collection.size(); ++i)
            {
                FontSizeCollection* font_sizes = fetch(FontSizeEntry{ i });
                if (font_sizes->font_sizes.empty())
                    continue;
                font_sizes->fill(&font_sizes->font_sizes);
                Component* comp = find_or_create_component(data, font_sizes->component);
                comp->font_sizes = FontSizeEntry{ i };
            }

            // Paths.
            for (size_t i = 0; i != path_collection.size(); ++i)
            {
                PathCollection* paths = fetch(PathEntry{ i });
                if (paths->paths.empty())
                    continue;
                paths->fill(&paths->paths);
                Component* comp = find_or_create_component(data, paths->component);
                comp->paths = PathEntry{ i };
            }

            // Colors.
            for (size_t i = 0; i != color_collection.size(); ++i)
            {
                ColorCollection* colors = fetch(ColorEntry{ i });
                if (colors->colors.empty())
                    continue;
                colors->fill(&colors->colors);
                Component* comp = find_or_create_component(data, colors->component);
                comp->colors = ColorEntry{ i };
            }

            // Allow them again.
            Config::allow_computed_colors(true);
        }

        void save_components()
        {
            // Checkboxes.
            for (size_t i = 0; i != checkbox_collection.size(); ++i)
            {
                CheckboxCollection* checkbox = fetch(CheckboxEntry{ i });
                if (checkbox->checkboxes.empty())
                    continue;
                checkbox->save(&checkbox->checkboxes);
            }

            // Font sizes.
            for (size_t i = 0; i != font_size_collection.size(); ++i)
            {
                FontSizeCollection* font_sizes = fetch(FontSizeEntry{ i });
                if (font_sizes->font_sizes.empty())
                    continue;
                font_sizes->save(&font_sizes->font_sizes);
            }

            // Paths.
            for (size_t i = 0; i != path_collection.size(); ++i)
            {
                PathCollection* paths = fetch(PathEntry{ i });
                if (paths->paths.empty())
                    continue;
                paths->save(&paths->paths);
            }

            // Colors.
            for (size_t i = 0; i != color_collection.size(); ++i)
            {
                ColorCollection* colors = fetch(ColorEntry{ i });
                if (colors->colors.empty())
                    continue;
                colors->save(&colors->colors);
            }
        }

        struct RenderDescriptionData
        {
            Vec2f pos;
            Vec4f unmatched_color;
            Vec4f match_color;
            Description* desc;
        };

        Vec2f build_description(CmdBuffer::DrawList* lst, Glyph::RenderFontContext* font_ctx, const RenderDescriptionData& in)
        {
            auto pos = in.pos;
            CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
            for (const auto& c : *in.desc)
            {
                if (c.matched)
                {
                    pos.x = font_ctx->render_glyph(lst, c.c, pos, in.match_color).x;
                }
                else
                {
                    pos.x = font_ctx->render_glyph(lst, c.c, pos, in.unmatched_color).x;
                }
            }
            return pos;
        }

        struct VisibleEntryResult
        {
            RenderComponent* first;
            float y_offset;
        };

        VisibleEntryResult first_visible_entry_and_offset(Explorer::Data* data)
        {
            float y_offset = 0.f;
            float current_y = 0.f;
            size_t i = 0;
            const float offset = data->scrollbox.position().y;
            // Find the first component to render.
            for (;i < data->render_components.collection.size(); ++i)
            {
                auto& component = data->render_components.collection[i];
                if (current_y + component.size.y <= offset)
                {
                    current_y += component.size.y;
                    continue;
                }
                y_offset = fmodf(offset - current_y, component.size.y);
                break;
            }
            return { .first = data->render_components.collection.data() + i, .y_offset = y_offset };
        }

        float offset_y_for_component_container(Explorer::Data* data, RenderComponent* component)
        {
            return component->y_offset - data->scrollbox.position().y;
        }

        AABBData box_for_component(Explorer::Data* data, RenderComponent* component, const Render::RenderViewport& viewport)
        {
            float offset_y = offset_y_for_component_container(data, component);
            Vec2f size{ rep(viewport.width) + 0.f, component->size.y };
            return { .pos = { 0.f, offset_y }, .size = size };
        }

        AABBData box_for_component_checkboxes(Explorer::Data* data, RenderComponent* component, const Render::RenderViewport& viewport)
        {
            float offset_y = offset_y_for_component_container(data, component);
            float size_y = data->render_components.checkbox_height * static_cast<int>(component->checkboxes.size());
            // Note: We need to also skip the title box.
            offset_y += data->render_components.title_height;
            Vec2f size{ rep(viewport.width) + 0.f, size_y };
            return { .pos = { 0.f, offset_y }, .size = size };
        }

        AABBData box_for_component_font_size(Explorer::Data* data, RenderComponent* component, const Render::RenderViewport& viewport)
        {
            auto prev = box_for_component_checkboxes(data, component, viewport);
            float size_y = data->render_components.font_size_height * static_cast<int>(component->font_sizes.size());
            auto next = prev;
            next.pos.y += prev.size.y;
            next.size.y = size_y;
            return next;
        }

        AABBData box_for_component_path(Explorer::Data* data, RenderComponent* component, const Render::RenderViewport& viewport)
        {
            auto prev = box_for_component_font_size(data, component, viewport);
            float size_y = data->render_components.path_height * static_cast<int>(component->paths.size());
            auto next = prev;
            next.pos.y += prev.size.y;
            next.size.y = size_y;
            return next;
        }

        AABBData box_for_component_color(Explorer::Data* data, RenderComponent* component, const Render::RenderViewport& viewport)
        {
            auto prev = box_for_component_path(data, component, viewport);
            float size_y = data->render_components.color_height * static_cast<int>(component->colors.size());
            auto next = prev;
            next.pos.y += prev.size.y;
            next.size.y = size_y;
            return next;
        }

        bool mouse_in_component(Explorer::Data* data, RenderComponent* component, const Vec2i& mouse_pos, const Render::RenderViewport& viewport)
        {
            auto box = box_for_component(data, component, viewport);
            return basic_aabb(box, mouse_pos);
        }

        HoveredEntry hovered_config_value(Explorer::Data* data, RenderComponent* component, const Vec2i& mouse_pos, const Render::RenderViewport& viewport)
        {
            // Split this up into separate AABB boxes for each section to see where our mouse is and more efficiently.
            // Checkboxes.
            auto box = box_for_component_checkboxes(data, component, viewport);
            if (basic_aabb(box, mouse_pos))
            {
                // Identify specific checkbox.
                auto cb_box = box;
                cb_box.size.y = data->render_components.checkbox_height;
                for (auto& cb : component->checkboxes)
                {
                    if (basic_aabb(cb_box, mouse_pos))
                    {
                        return { .checkbox = cb.entry };
                    }
                    cb_box.pos.y += data->render_components.checkbox_height;
                }
                // A failed hit-test in the loop implies there are no valid hover results to be had below.
                return {};
            }

            box = box_for_component_font_size(data, component, viewport);
            if (basic_aabb(box, mouse_pos))
            {
                // Identify specific font_size.
                auto fz_box = box;
                fz_box.size.y = data->render_components.font_size_height;
                for (auto& fz : component->font_sizes)
                {
                    if (basic_aabb(fz_box, mouse_pos))
                    {
                        return { .font_size = fz.entry };
                    }
                    fz_box.pos.y += data->render_components.font_size_height;
                }
                // A failed hit-test in the loop implies there are no valid hover results to be had below.
                return {};
            }

            box = box_for_component_path(data, component, viewport);
            if (basic_aabb(box, mouse_pos))
            {
                // Identify specific path.
                auto path_box = box;
                path_box.size.y = data->render_components.path_height;
                for (auto& path : component->paths)
                {
                    if (basic_aabb(path_box, mouse_pos))
                    {
                        return { .path = path.entry };
                    }
                    path_box.pos.y += data->render_components.path_height;
                }
                // A failed hit-test in the loop implies there are no valid hover results to be had below.
                return {};
            }

            box = box_for_component_color(data, component, viewport);
            if (basic_aabb(box, mouse_pos))
            {
                // Identify specific color.
                auto cl_box = box;
                cl_box.size.y = data->render_components.color_height;
                for (auto& cl : component->colors)
                {
                    if (basic_aabb(cl_box, mouse_pos))
                    {
                        return { .color = cl.entry };
                    }
                    cl_box.pos.y += data->render_components.color_height;
                }
                // A failed hit-test in the loop implies there are no valid hover results to be had below.
                return {};
            }

            return { };
        }

        bool execute_click_on_hovered_entry(Explorer::Data* data, BuildExplorerResponse* resp, const HoveredEntry& hover, const ScreenDimensions& screen, UIState* state)
        {
            if (hover.checkbox != nullptr)
            {
                hover.checkbox->state = not hover.checkbox->state;
                save_components();
                return true;
            }

            if (hover.path != nullptr)
            {
                data->ui_data.path_change_data.path = hover.path;
                resp->request_config_path = true;
            }

            if (hover.color != nullptr)
            {
                data->color_picker.start(screen, state->mouse.ui_mouse, hover.color->color, state);
                data->ui_data.color_pick_data.color = hover.color;
                data->ui_data.color_pick_data.picking_color = true;
                return false;
            }

            return false;
        }
    } // namespace [anon]

    Explorer::Explorer(Glyph::Atlas* atlas):
        data{ new Data{
                .window = { UI::Widgets::ID::ConfigExplorer },
                .scrollbox { UI::Widgets::ID::ConfigExplorer },
                // The ID is computed as-needed.
                .color_picker{ atlas, UI::Widgets::make_id_seed(UI::Widgets::ID::ConfigExplorer, "picker") }, .atlas = atlas } }
    {
        init_collections();
        data->window.title("Config Explorer");
    }

    Explorer::~Explorer() = default;

    // Interaction.
    void Explorer::start(const ScreenDimensions& screen, UI::UIState* state)
    {
        populate_components(data.get());

        UI::Widgets::ShowWindowData show_data{
            .initial_viewport = initial_window_viewport(screen),
            .expand_point = { 0.5f, 0.5f }
        };
        data->window.show(show_data);
        UI::set_focus_window(state, UI::Widgets::ID::ConfigExplorer);
        setup_ui_data(data.get());
    }

    void Explorer::sync_config()
    {
        populate_components(data.get());
        data->font_size = Glyph::FontSize{ Config::widget_state().window_title_font_size };
        data->window.sync_config(data->atlas);
        data->scrollbox.sync_config();
        data->color_picker.sync_config();
        setup_ui_data(data.get());
    }

    bool Explorer::try_close_dialog()
    {
        if (data->ui_data.color_pick_data.picking_color)
        {
            data->ui_data.color_pick_data.picking_color = false;
            return true;
        }
        return false;
    }

    bool Explorer::sub_dialog_open(UI::UIState* state)
    {
        return state->focus_window == data->color_picker.id();
    }

    void Explorer::populate_path_value(String8 path)
    {
        if (data->ui_data.path_change_data.path != nullptr)
        {
            data->ui_data.path_change_data.path->path = path;
            save_components();
        }
    }

    // Queries.
    UI::Widgets::ID Explorer::id() const
    {
        return data->window.id();
    }

    String8 Explorer::target_config_path() const
    {
        if (data->ui_data.path_change_data.path != nullptr)
            return data->ui_data.path_change_data.path->path;
        return str8_empty;
    }

    BuildExplorerResponse Explorer::build(CmdBuffer::DrawList* lst,
                                            UI::UIState* state,
                                            Feed::MessageFeed* feed)
    {
        BuildExplorerResponse resp{};
        // Process input.
        {
            // If we're modal, we won't process events until the color picker is done.
            if (not data->ui_data.color_pick_data.picking_color)
            {
                auto window_vp = data->window.window_viewport();
                HoveredEntry hover{};
                Widgets::ID hot_widget = Widgets::ID::Zero;
                if (mouse_in_viewport(state->mouse.ui_mouse, window_vp))
                {
                    auto [scroll_vp] = window_content_viewports(data->window.content_viewport(window_vp));
                    auto content_vp = data->scrollbox.content_viewport(scroll_vp);
                    if (mouse_in_viewport(state->mouse.ui_mouse, content_vp))
                    {
                        auto adjusted_mouse = adjusted_mouse_for_viewport(state->mouse.ui_mouse, content_vp);
                        // Adjust the mouse 'y' since mouse coords start at the top-left.
                        adjusted_mouse.y = rep(content_vp.height) - adjusted_mouse.y;
                        auto [first, offset] = first_visible_entry_and_offset(data.get());
                        RenderComponent* last = data->render_components.collection.data() + data->render_components.collection.size();
                        for (; first != last; ++first)
                        {
                            if (mouse_in_component(data.get(), first, adjusted_mouse, content_vp))
                            {
                                hover = hovered_config_value(data.get(), first, adjusted_mouse, content_vp);
                                if (has_hover(hover))
                                {
                                    if (hover.checkbox != nullptr)
                                    {
                                        hot_widget = hover.checkbox->id;
                                    }
                                    else if (hover.color != nullptr)
                                    {
                                        hot_widget = hover.color->id;
                                    }
                                    else if (hover.font_size != nullptr)
                                    {
                                        hot_widget = hover.font_size->id;
                                    }
                                    else if (hover.path != nullptr)
                                    {
                                        hot_widget = hover.path->id;
                                    }
                                }
                            }

                            float pos_y = offset_y_for_component_container(data.get(), first);

                            if (pos_y > rep(content_vp.height))
                                break;
                        }
                    }

                    try_set_hot_widget(state, hot_widget);
                    if (UI::down(*state, MouseButton::L))
                    {
                        try_set_focus_widget(state, hot_widget);
                        set_focus_window(state, Widgets::ID::ConfigExplorer);
                    }

                    if (empty_focus_widget(*state))
                    {
                        if (hot_widget_set(*state, hot_widget))
                        {
                            change_cursor(state, CursorStyle::Default);
                        }
                    }
                }

                // Process config changes.
                if (std_click_trigger(*state, hot_widget))
                {
                    resp.config_updated = execute_click_on_hovered_entry(data.get(), &resp, hover, CmdBuffer::screen(*lst), state);
                }
            }
        }
        const auto& colors = Config::widget_colors();

        // Render all widgets.
        {
            auto window_resp = data->window.build(lst, data->atlas, state);
            resp.close = window_resp.close;
        }
        auto [scroll_vp] = window_content_viewports(data->window.content_viewport(data->window.window_viewport()));
        auto content_vp = data->scrollbox.content_viewport(scroll_vp);
        // Adjust the content based on viewport size so we don't get extra space at the bottom
        // of the viewport when scrolled to bottom.
        data->scrollbox.content_size(content_size(data.get()), content_vp);
        CmdBuffer::push_clip(lst, UI::convert(scroll_vp));
        {
            auto scroll_resp = data->scrollbox.build(lst, state, data->ui_data.wheel_offset_amount, Widgets::BuildScrollBoxFlags::None);
            // If the scroll changed set the focus so no other widgets try to process the event.
            if (scroll_resp.scroll_changed)
            {
                try_set_focus_widget(state, Widgets::ID::ConfigExplorer);
            }
        }
        CmdBuffer::push_clip(lst, UI::convert(content_vp));

        // Hopefully we won't have numbers larger than this??  + '#' for null byte.
        char num_buf[] = "999999999#";
        auto font_ctx = data->atlas->render_font_context(data->font_size);
        Vec2f pos;
        pos.y = 0.f + rep(content_vp.height) + Data::padding; // Add in the extra padding for the first entry.

        auto [first_c, y_offset] = first_visible_entry_and_offset(data.get());
        pos.y += y_offset;

        // Common data to all rendering of descriptions.
        RenderDescriptionData desc_data =
        {
            .pos = pos,
            .unmatched_color = colors.window_title_font_color,
            .match_color = hex_to_vec4f(0xffffffff),
            .desc = nullptr,
        };

        RenderComponent* last_c = data->render_components.collection.data() + data->render_components.collection.size();
        for (;first_c != last_c; ++first_c)
        {
            auto& component = *first_c;
            pos.y -= font_ctx.current_font_size() + Data::padding;

            // Title.
            {
                auto text_pos = pos;
                // Center the component title.
                float text_w = font_ctx.measure_text(component.component).x;
                text_pos.x += (rep(content_vp.width) - text_w) / 2.f;
                CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
                font_ctx.render_text(lst, component.component, text_pos, colors.window_title_font_color);
                pos.y -= Data::padding;
            }

            // Checkboxes.
            {
                for (auto& checkbox : component.checkboxes)
                {
                    Widgets::CheckboxInput cb_in =
                    {
                        .label = "",
                        .checked = make_yes_no<Widgets::Checked>(checkbox.entry->state)
                    };

                    pos.y -= Widgets::measure_checkbox(&font_ctx, cb_in).y + Data::padding;

                    if (checkbox.entry->id == state->hot_widget)
                    {
                        CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
                        Vec2f size{ rep(content_vp.width) + 0.f, data->render_components.checkbox_height };
                        CmdBuffer::solid_rect(lst, Render::FragShader::BasicColor, pos, size, colors.window_border);
                    }
                    auto label_pos = Widgets::build_checkbox_no_label(lst,
                                                &font_ctx,
                                                pos,
                                                cb_in.checked);
                    desc_data.desc = &checkbox.entry->name;
                    desc_data.pos = label_pos;
                    build_description(lst, &font_ctx, desc_data);
                }
            }

            // Font sizes.
            {
                Vec2f rect_size{ rep(content_vp.width) + 0.f, font_ctx.current_font_line_height() + 0.f };
                for (auto& font_size : component.font_sizes)
                {
                    if (font_size.entry->id == state->hot_widget)
                    {
                        CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
                        Vec2f size{ rep(content_vp.width) + 0.f, data->render_components.font_size_height };
                        auto bg_pos = pos;
                        bg_pos.y -= size.y;
                        CmdBuffer::solid_rect(lst, Render::FragShader::BasicColor, bg_pos, size, colors.window_border);
                    }

                    pos.y -= rect_size.y + Data::padding;

                    // Position description above the edit range.
                    desc_data.desc = &font_size.entry->name;
                    desc_data.pos = pos;
                    build_description(lst, &font_ctx, desc_data);

                    pos.y -= rect_size.y + Data::padding;
                    // Render just the text.
                    CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
                    CmdBuffer::strike_rect(lst, Render::FragShader::BasicColor, pos, rect_size, Data::padding, colors.window_border);

                    // Render the string.
                    String8 num = fmt_string(num_buf, "%d", font_size.entry->size);
                    CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
                    auto center_pos = pos;
                    center_pos.y += (rect_size.y + font_ctx.current_font_size()) / 5.f;
                    center_pos.x += Data::padding;
                    font_ctx.render_text(lst, num, center_pos, colors.window_title_font_color);
                }
            }

            // Paths.
            {
                Vec2f rect_size{ rep(content_vp.width) + 0.f, font_ctx.current_font_line_height() + 0.f };
                for (auto& path : component.paths)
                {
                    if (path.entry->id == state->hot_widget)
                    {
                        CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
                        Vec2f size{ rep(content_vp.width) + 0.f, data->render_components.path_height };
                        auto bg_pos = pos;
                        bg_pos.y -= size.y;
                        CmdBuffer::solid_rect(lst, Render::FragShader::BasicColor, bg_pos, size, colors.window_border);
                    }

                    pos.y -= rect_size.y + Data::padding;

                    // Position description above the edit range.
                    desc_data.desc = &path.entry->name;
                    desc_data.pos = pos;
                    build_description(lst, &font_ctx, desc_data);

                    pos.y -= rect_size.y + Data::padding;
                    // Render just the text.
                    CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
                    CmdBuffer::strike_rect(lst, Render::FragShader::BasicColor, pos, rect_size, Data::padding, colors.window_border);

                    // Render the string.
                    CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
                    auto center_pos = pos;
                    center_pos.y += (rect_size.y + font_ctx.current_font_size()) / 5.f;
                    center_pos.x += Data::padding;
                    font_ctx.render_text(lst, path.entry->path, center_pos, colors.window_title_font_color);
                }
            }

            // Colors.
            {
                for (auto& c : component.colors)
                {
                    Widgets::ColorboxData checkbox_data =
                    {
                        .label = "",
                        .color = c.entry->color
                    };

                    pos.y -= Widgets::measure_colorbox(&font_ctx, checkbox_data).y + Data::padding;

                    if (c.entry->id == state->hot_widget)
                    {
                        CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
                        Vec2f size{ rep(content_vp.width) + 0.f, data->render_components.color_height };
                        CmdBuffer::solid_rect(lst, Render::FragShader::BasicColor, pos, size, colors.window_border);
                    }

                    auto label_pos = Widgets::build_colorbox_no_label(lst,
                                                &font_ctx,
                                                pos,
                                                c.entry->color);
                    desc_data.desc = &c.entry->name;
                    desc_data.pos = label_pos;
                    build_description(lst, &font_ctx, desc_data);
                }
            }

            if (pos.y < -font_ctx.current_font_line_height())
                break;
        }

        // Render color picker.
        if (data->ui_data.color_pick_data.picking_color)
        {
            auto color_resp = data->color_picker.build(lst, state);
            if (color_resp.close)
            {
                try_close_dialog();
            }

            if (color_resp.color_change)
            {
                if (data->ui_data.color_pick_data.color == nullptr)
                {
                    feed->queue_error("Bad UI state: color picker null.");
                }
                else if (data->ui_data.color_pick_data.color->color != color_resp.result_color)
                {
                    data->ui_data.color_pick_data.color->color = color_resp.result_color;
                    save_components();
                    resp.config_updated = true;
                }
            }
        }

        data->window.end(state);

        // Content viewport.
        CmdBuffer::pop_clip(lst);
        // Scroll viewport.
        CmdBuffer::pop_clip(lst);

        return resp;
    }
} // namespace Config