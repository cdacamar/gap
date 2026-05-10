#pragma once

#include "gap-strings.h"
#include "vec.h"

namespace Feed
{
    class MessageFeed;
} // namespace Feed

namespace Config
{
#define DAT_CAT_START(T, access, component, path) struct T {
#define DAT_CAT_END() };
#define DAT_COLOR(name, T, desc) T name;
#define DAT_TOGGLE(name, T, desc) T name;
#define DAT_FONTSIZE(name, T, desc) T name;
#define DAT_PX(name, T, desc) T name;
#define DAT_INT(name, T, desc) T name;
#define DAT_PATH(name, T, desc) T name;
#define DAT_FILE(name, T, desc) T name;
#define DAT_STRING(name, T, desc) T name;
#define DAT_SOUND(name, T, desc) T name;
#define DAT_RECT_I(name, T, desc) T name;
#include "config.dat"
#undef DAT_CAT_START
#undef DAT_CAT_END
#undef DAT_COLOR
#undef DAT_TOGGLE
#undef DAT_FONTSIZE
#undef DAT_PX
#undef DAT_INT
#undef DAT_PATH
#undef DAT_FILE
#undef DAT_STRING
#undef DAT_SOUND
#undef DAT_RECT_I

    // Initialization.
    void init();

    // Queries.
    const DiffColors& diff_colors();
    const DiffState& diff_state();
    const FeedColors& feed_colors();
    const FeedState& feed_state();
    const WidgetColors& widget_colors();
    const WidgetState& widget_state();
    const SystemCore& system_core();
    const SystemFonts& system_fonts();
    const SystemEffects& system_effects();
    const HotkeyState& hotkey_state();
    bool needs_save();

    // Updates.
    void update(const DiffColors& new_state);
    void update(const DiffState& new_state);
    void update(const FeedColors& new_state);
    void update(const FeedState& new_state);
    void update(const WidgetColors& new_state);
    void update(const WidgetState& new_state);
    void update(const SystemCore& new_state);
    void update(const SystemFonts& new_state);
    void update(const SystemEffects& new_state);
    void update(const HotkeyState& new_state);
    void add_delta_all_font_sizes(int delta);
    void update_string(String8* cfg_value, String8 value);

    // File handling.
    bool load_config(String8 path, Feed::MessageFeed* feed);
    bool save_config(String8 path, Feed::MessageFeed* feed);

    // Helpers.
    void allow_computed_colors(bool b);
} // namespace Config