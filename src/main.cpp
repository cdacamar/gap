#include <stdio.h>
#include <stdint.h>

#include <cassert>

#include <algorithm>

// This gets included first because it contains configuration for the entire project.
#include "gap-core.h"

#include "arena-report.h"
#include "basic-confirm.h"
#include "basic-textbox.h"
#include "clipboard-manager.h"
#include "cmd-buffer-api.h"
#include "cmd-buffer.h"
#include "config-explorer.h"
#include "config.h"
#include "constants.h"
#include "diff-core.h"
#include "diff-dir-panel.h"
#include "diff-dir-list.h"
#include "diff-panel.h"
#include "enum-utils.h"
#include "feed.h"
#include "gap-strings.h"
#include "glyph-cache.h"
#include "help.h"
#include "hotkeys.h"
#include "os.h"
#include "renderer.h"
#include "svg.h"
#include "thread-ctx.h"
#include "thread.h"
#include "types.h"
#include "ui-common.h"
#include "util.h"
#include "vec.h"
#include "window-theming.h"

enum class CommandMode
{
    None,
    Help,
    ConfigExplorer,
    DiffDirPanel,
};

using namespace UI;

void change_mode(CommandMode* mode, CommandMode new_mode)
{
    *mode = new_mode;
}

struct MsFrameCounter
{
    static constexpr size_t len = 64;
    static constexpr size_t min_entries = 32;

    void record_dt(uint32_t dt)
    {
        dt_history[idx % len] = dt;
        ++idx;
    }

    bool history_populated() const
    {
        return idx >= min_entries;
    }

    uint32_t previous_dt() const
    {
        return dt_history[(idx - 1) % len];
    }

    uint32_t average_dt() const
    {
        size_t frame_count = std::min(idx, len);
        uint64_t dt_sum = 0;
        for (size_t i = 0; i < frame_count; ++i)
        {
            dt_sum += dt_history[i];
        }
        uint32_t dt_avg = static_cast<uint32_t>(dt_sum / frame_count);
        return dt_avg;
    }

    size_t idx = 0;
    uint32_t dt_history[len]{};
};

using OS::Hz;

Hz optimal_hz(const MsFrameCounter& counter)
{
    Hz target_hz = OS::monitor_refresh_rate();
    if (not counter.history_populated())
        return target_hz;
    uint32_t average_dt = counter.average_dt();
    const Hz candidate_hz_targets[] = { target_hz, Hz{ 60 }, Hz{ 120 }, Hz{ 144 }, Hz{ 240 } };
    Hz best_hz = target_hz;
    constexpr auto largest = Constants::max_S32;
    auto best_dt = largest;
    for (Hz candidate : candidate_hz_targets)
    {
        if (candidate <= target_hz)
        {
            int32_t frame_time = 1000 / static_cast<int32_t>(candidate);
            int32_t frame_time_diff = static_cast<int32_t>(average_dt) - frame_time;
            if (std::abs(frame_time_diff) < best_dt)
            {
                best_hz = candidate;
                best_dt = frame_time_diff;
            }
        }
    }
    return best_hz;
}

struct FPSData
{
    char fmt_buf[128];
    String8 fps_text;
    uint32_t last_update = 0;
    uint32_t last_fps_update = 0;
    uint32_t last_frame_dt = 0;
    uint32_t last_frame_layout_dt = 0;
    float fps = 0.;
};

struct RenderCoreData
{
    ScreenDimensions* screen;
    CommandMode* cmd_mode;
    Render::FrameRenderer* renderer;
    Glyph::Atlas* atlas;
    Feed::MessageFeed* feed;
    Diff::DiffPanel* diff_panel;
    Diff::DiffDirPanel* diff_dir_panel;
    Config::Explorer* config_explorer;
    Help::Help* help;
    MsFrameCounter* ms_frame_counter;
    FPSData* fps_data;
    UIState* ui_state;
    CmdBuffer::CmdList* cmd_lst;
    CmdBuffer::DrawList* core_draw_lst;
    Clipboard::ClipboardManager* clipboard;
    Arena::Report::ArenaReport* arena_report;
};

enum class NotifyConfigExplorer : bool { No, Yes };

void notify_config_update(NotifyConfigExplorer notify_explorer, RenderCoreData* render_data)
{
    // Update components.
    Theme::apply_border_color(OS::core_window(), render_data->feed);
    render_data->atlas->sync_config();
    render_data->help->sync_config();
    render_data->arena_report->sync_config();
    Diff::diff_panel_sync_config(render_data->diff_panel, render_data->feed);
    Diff::diff_dir_panel_sync_config(render_data->diff_dir_panel);

    if (is_yes(notify_explorer))
    {
        render_data->config_explorer->sync_config();
    }
}

void notify_hotkey_update(RenderCoreData* data)
{
    data->help->hotkeys_updated();
}

void clear_command_mode(RenderCoreData* data)
{
    assert(*data->cmd_mode != CommandMode::None);
    auto focus_id = Widgets::ID::Zero;
    // Clear the search term.
    switch (*data->cmd_mode)
    {
    case CommandMode::ConfigExplorer:
        // Clear the color picker.
        if (data->config_explorer->sub_dialog_open(data->ui_state))
        {
            data->config_explorer->try_close_dialog();
            focus_id = data->config_explorer->id();
        }
        else
        {
            *data->cmd_mode = CommandMode::None;
        }
        break;
    default:
        *data->cmd_mode = CommandMode::None;
        break;
    }

    // Return focus to the core window.
    set_focus_window(data->ui_state, focus_id);
}

void build_config_explorer(RenderCoreData* data)
{
    auto resp = data->config_explorer->build(data->core_draw_lst,
                                                data->ui_state,
                                                data->feed);
    if (resp.close)
    {
        clear_command_mode(data);
        return;
    }

    if (resp.request_config_path)
    {
    }

    if (resp.config_updated)
    {
        notify_config_update(NotifyConfigExplorer::No, data);
    }
}

void build_help(RenderCoreData* data)
{
    auto resp = data->help->build(data->core_draw_lst,
                                    data->ui_state,
                                    data->clipboard,
                                    data->feed);

    if (resp.reload_hotkeys)
    {
        if (Hotkeys::load(Config::hotkey_state().hotkeys, data->feed))
        {
            char fmt_buf[512];
            String8 msg = fmt_string(fmt_buf, "Hotkeys reloaded at: %S", Config::hotkey_state().hotkeys);
            data->feed->queue_info(msg);
            notify_hotkey_update(data);
        }
        else
        {
            data->feed->queue_warning("Hotkeys failed to reload.");
        }
    }

    if (resp.close)
    {
        clear_command_mode(data);
    }
}

void build_arena_report(RenderCoreData* data)
{
    auto resp = data->arena_report->build(data->core_draw_lst,
                                            data->ui_state);
    if (resp.close)
    {
        data->ui_state->special = remove_flag(data->ui_state->special, SpecialModes::ShowArenaReport);
        return;
    }

    if (resp.open_locus)
    {
    }
}

void build_diff_dir_panel(RenderCoreData* data)
{
    auto resp = Diff::build_diff_dir_panel(data->diff_dir_panel,
                                            data->cmd_lst,
                                            data->core_draw_lst,
                                            data->ui_state,
                                            data->feed);
    if (resp.close)
    {
        clear_command_mode(data);
    }

    if (resp.pop_to_diff)
    {
        Diff::DiffDirDiffResults results = Diff::diff_dir_panel_cached_diffs(data->diff_dir_panel, resp.diff_idx);
        Diff::diff_panel_sink_cached_diffs(data->diff_panel, results);
        clear_command_mode(data);
    }
}

Vec2i ui_mouse_pos(const OS::Event& e, const ScreenDimensions& screen)
{
    // Convert to screen coords.
    return Vec2i{ static_cast<int>(e.pos.x), rep(screen.height) - static_cast<int>(e.pos.y) };
}

MouseButton convert(OS::Key k)
{
    switch (k)
    {
    case OS::Key::LeftMouseButton:
        return MouseButton::L;
    case OS::Key::RightMouseButton:
        return MouseButton::R;
    case OS::Key::MiddleMouseButton:
        return MouseButton::Mid;
    }
    assert(not "invalid key");
    return MouseButton::Count;
}

bool process_hotkeys(const OS::Event& e, UIState* state)
{
    bool hotkey_matched = false;
    auto global_hotkey = Hotkeys::match_group(CustomHotkeyGroup::GLB, e.key, state->mods);
    // Descend into editor hotkeys.
    if (Hotkeys::valid_match(global_hotkey))
    {
        state->hotkeys.hk = global_hotkey.hk;
        state->hotkeys.cust_hk.id = global_hotkey.cust_hk_id;
        state->hotkeys.cust_hk.group = CustomHotkeyGroup::GLB;
        hotkey_matched = true;
    }
    return hotkey_matched;
}

void ui_mouse_down(const OS::Event& e, UIState* state, const ScreenDimensions& screen, RenderCoreData* data)
{
    const MouseButton btn = convert(e.key);
    // Convert to screen coords.
    state->mouse.ui_mouse = ui_mouse_pos(e, screen);

    // Process hotkeys.
    if (state->rebinding_hotkey)
    {
        // Eat all the key down events while binding.
        if (e.key == OS::Key::Esc)
        {
            clear_hotkey_rebind_state(state);
        }

        // As long as there's a mod active, we can rebind.
        if (state->mods != KeyMods::None)
        {
            if (state->hotkeys.rebind_target != Hotkey::None)
            {
                Hotkeys::rebind(state->hotkeys.rebind_target, e.key, state->mods);
            }

            if (state->hotkeys.cust_hk.rebind_target != CustomHotkeyID::None)
            {
                Hotkeys::rebind(state->hotkeys.cust_hk.rebind_target, e.key, state->mods);
            }
            clear_hotkey_rebind_state(state);
            notify_hotkey_update(data);
        }
    }
    else
    {
        process_hotkeys(e, state);
    }

    // Also set the key value so we can detect quick mouse events.
    state->keys[rep(e.key)].down = true;

    // Detect clicks.
    if (not state->mouse.down[rep(btn)])
    {
        state->mouse.down[rep(btn)] = true;
        // See if we should be registering this as double-click in the same spot or a new series of
        // clicks in a different spot.
        constexpr float click_delta_thresh = 12.f;
        Vec2i dx = state->mouse.ui_mouse - state->mouse.initial_down_mouse;
        bool consider_multi_click = false;
        if (dx.mag2() < click_delta_thresh)
        {
            consider_multi_click = true;
        }
        state->mouse.initial_down_mouse = state->mouse.ui_mouse;

        // See if we should register this as a click.
        if (consider_multi_click
            and OS::delta_meets_double_click_time(state->mouse.last_click[rep(btn)],
                                                    OS::get_ticks32()))
        {
            ++state->mouse.click_count[rep(btn)];
        }
        else
        {
            state->mouse.click_count[rep(btn)] = 1;
        }
        state->mouse.last_click[rep(btn)] = OS::get_ticks32();
    }
}

void ui_clear_key_states(UI::UIState* state)
{
    // We can just memset this.
    zero_bytes(state->keys, std::size(state->keys));
    // But do not clear mods.
}

void ui_clear_mouse_state(UI::UIState* state)
{
    zero_bytes(state->mouse.down, std::size(state->mouse.down));
    zero_bytes(state->mouse.click_count, std::size(state->mouse.click_count));
    zero_bytes(state->mouse.last_click, std::size(state->mouse.last_click));
    state->mouse.ui_mouse = state->mouse.ui_prev_mouse = Vec2i(-1, -1);
}

void ui_clear_widget_state(UIState* state)
{
    state->hot_widget = Widgets::ID::Zero;
    state->focus_widget = Widgets::ID::Zero;
    state->context_widget = Widgets::ID::Zero;
    state->highlight_widget = Widgets::ID::Zero;
}

void ui_clear_hotkey_state(UIState* state)
{
    state->hotkeys = {};
    state->rebinding_hotkey = false;
}

void ui_focus_lost(UIState* state)
{
    ui_clear_key_states(state);
    ui_clear_mouse_state(state);
    ui_clear_widget_state(state);
    ui_clear_hotkey_state(state);
    clear_drag(state);
    // Clear mods too.
    state->mods = KeyMods::None;
    // Keep:
    // - Keyboard
    // - Window
    // Widgets active.

    // Reap any system memory if we can.
    OS::mem_clear_working_set_pages();

    // Request frames so that we can render the new inert state.
    Render::request_frames();
}

void ui_mouse_up(const OS::Event& e, UIState* state, const ScreenDimensions& screen)
{
    // Convert to screen coords.
    state->mouse.ui_mouse = ui_mouse_pos(e, screen);
    state->mouse.down[rep(convert(e.key))] = false;
}

struct KeyDownResult
{
    char c;
    bool hotkey_matched;
};

KeyDownResult ui_key_down(const OS::Event& e, UIState* state, RenderCoreData* data)
{
    KeyDownResult result = {};

    switch (e.key)
    {
    case OS::Key::Shift:
        state->mods |= KeyMods::Shift;
        break;
    case OS::Key::Alt:
        state->mods |= KeyMods::Alt;
        break;
    case OS::Key::Ctrl:
        state->mods |= KeyMods::Ctrl;
        break;
    }

    // Process hotkeys.
    if (state->rebinding_hotkey)
    {
        // Clear the keyboard widget so the text event is not processed.
        state->focus_keyboard = Widgets::ID::Zero;
        // Eat all the key down events while binding.
        result.hotkey_matched = true;
        if (e.key == OS::Key::Esc)
        {
            clear_hotkey_rebind_state(state);
        }

        if (e.key != OS::Key::Esc
            and e.key != OS::Key::Shift
            and e.key != OS::Key::Alt
            and e.key != OS::Key::Ctrl)
        {
            if (state->hotkeys.rebind_target != Hotkey::None)
            {
                Hotkeys::rebind(state->hotkeys.rebind_target, e.key, state->mods);
            }

            if (state->hotkeys.cust_hk.rebind_target != CustomHotkeyID::None)
            {
                Hotkeys::rebind(state->hotkeys.cust_hk.rebind_target, e.key, state->mods);
            }
            clear_hotkey_rebind_state(state);
            notify_hotkey_update(data);
        }
    }
    // Filter out null keys.
    else if (e.key != OS::Key::Null)
    {
        result.hotkey_matched = process_hotkeys(e, state);
    }

    // Only record alpha-numeric values.
    bool alpha_key = (rep(e.key) <= 'z' and rep(e.key) >= 'a')
                    or (rep(e.key) <= '9' and rep(e.key) >= '0');
    result.c = alpha_key * char(rep(e.key));

    // Only record the key down state if a hotkey did not eat it.
    state->keys[rep(e.key)].down |= not result.hotkey_matched;

    return result;
}

void ui_key_up(const OS::Event& e, UIState* state)
{
    state->keys[rep(e.key)].down = false;

    switch (e.key)
    {
    case OS::Key::Shift:
        state->mods = remove_flag(state->mods, KeyMods::Shift);
        break;
    case OS::Key::Alt:
        state->mods = remove_flag(state->mods, KeyMods::Alt);
        break;
    case OS::Key::Ctrl:
        state->mods = remove_flag(state->mods, KeyMods::Ctrl);
        break;
    }
}

template <typename Widget>
void ui_mouse_wheel_widget(const OS::Event& e, UIState* state, Widget* widget, const Vec2i& mouse_pos, const ScreenDimensions& screen)
{
    if (e.wheel_delta.y > 0.f)
    {
        widget->mouse_wheel_up(*state, mouse_pos, screen);
    }
    else
    {
        widget->mouse_wheel_down(*state, mouse_pos, screen);
    }
}

void ui_end_frame(UIState* state)
{
    // Don't let widgets think they can set this.
    if (not any_mouse_down(*state))
    {
        force_set_focus_widget(state, Widgets::ID::Zero);
        clear_drag(state);
    }
    // Kind of a catch-all when the focus widget is lost somehow.
    else if (state->focus_widget == Widgets::ID::Zero)
    {
        force_set_focus_widget(state, Widgets::ID::Sentinel);
        clear_drag(state);
    }
    // Post the cursor.
    OS::set_cursor(state->cursor);
    state->cursor = CursorStyle::Default;
    state->mouse.ui_prev_mouse = state->mouse.ui_mouse;
    state->mouse.wheel_delta = 0.f;
    state->in_buf = str8_empty;
    state->in_mod_buf = str8_empty;
    state->new_suggestions = false;
    state->hotkeys.hk = Hotkey::None;
    state->hotkeys.cust_hk.id = CustomHotkeyID::None;
    // Tooltips need constant easing applied so when they are opened->closed->opened
    // they'll be properly tweened.
    {
        float expand = (static_cast<float>(state->tooltip.enabled) - state->tooltip.expand_offset) * state->anim_faster_rate;
        state->tooltip.expand_offset += expand;
        if (state->tooltip.enabled and state->tooltip.expand_offset >= 0.99f)
        {
            state->tooltip.expand_offset = 1.f;
        }
        if (state->tooltip.enabled and state->tooltip.expand_offset != 1.f)
        {
            Render::request_frames();
        }
        state->tooltip.enabled = false;
    }
    // Clear key states (key mods are retained).
    ui_clear_key_states(state);

    // This was observed in RADDBG.  If we clear the working set, it will allow the system to throw away a lot of the
    // memory left over in the working set from startup--which isn't needed for general processing.
    if (state->frame_index == FrameIndex{ 15 })
    {
        OS::mem_clear_working_set_pages();
    }

    Arena::clear(state->post_frame_arena);

    PROF_FRAME_END();
}

void ui_new_frame(UIState* state, float dt)
{
    PROF_FRAME_START();

    state->frame_index = extend(state->frame_index);

    state->hot_widget = Widgets::ID::Zero;
    state->highlight_widget = Widgets::ID::Zero;
    // Setup the animation states.
    state->anim_fast_rate = 1.f - std::pow(2.f, (-40.f * dt));
    state->anim_faster_rate = 1.f - std::pow(2.f, (-60.f * dt));

    // Cleanup the frame arena.
    Arena::clear(state->frame_arena);
}

void process_window_state(OS::OSWindow wind)
{
    bool wind_fs = OS::window_fullscreened(wind);
    bool wind_max = OS::window_maximized(wind);
    bool changed = false;
    Config::SystemCore sys_core = Config::system_core();
    if (wind_fs != sys_core.fullscreen)
    {
        sys_core.fullscreen = wind_fs;
        changed = true;
    }

    if (wind_max != sys_core.maximized)
    {
        sys_core.maximized = wind_max;
        changed = true;
    }

    if (not wind_fs and not wind_max)
    {
        Vec4i wind_pos = OS::window_rect(wind);
        if (wind_pos != sys_core.core_window_rect)
        {
            sys_core.core_window_rect = wind_pos;
            changed = true;
        }
    }

    if (changed)
    {
        Feed::global_feed()->queue_info("updated");
        Config::update(sys_core);
    }
}

void render_core(RenderCoreData* data)
{
    const uint32_t start = rep(OS::get_ticks32());
    const ScreenDimensions screen = *data->screen;

    // We will wrap 'time' for the renderer so that we do not hit floating point limitations.
    // Wrap this at 60 minutes (or 60m * 60s * 1000ms).
    constexpr uint32_t wrap_time = 60 * 60 * 1000;
    const float wrapped_time = static_cast<float>(start % wrap_time) / 1000.f;
    const Hz refresh = optimal_hz(*data->ms_frame_counter);
    const float dt = 1.f / rep(refresh);
    Render::update_time(data->renderer, wrapped_time, dt);

    ui_new_frame(data->ui_state, dt);

    PROF_SCOPE();

    CmdBuffer::new_frame(data->core_draw_lst, screen, { .dt = dt, .app_time = wrapped_time });
    // Default clip rect for the screen.
    CmdBuffer::push_clip(data->core_draw_lst, CmdBuffer::ClipRect::basic(screen));
    // Default texture (atlas by default).
    CmdBuffer::push_texture(data->core_draw_lst, data->atlas->atlas_texture());
    // Core color palette.
    {
        const auto& colors = Config::widget_colors();
        CmdBuffer::ColorPalette palette = {
            .fill = colors.window_title_background,
            .background = Config::diff_colors().background,
            .border = colors.window_border,
            .highlight = colors.window_title_background,
            .outline_selection = colors.outline_selection,
            .text = colors.window_title_font_color
        };
        CmdBuffer::push_color_palette(data->core_draw_lst, palette);
    }

    // Primary render.

    switch (*data->cmd_mode)
    {
    case CommandMode::None:
        break;
    case CommandMode::Help:
        build_help(data);
        break;
    case CommandMode::ConfigExplorer:
        build_config_explorer(data);
        break;
    }

#ifdef BUILD_TRACK_ARENA
    if (implies(data->ui_state->special, SpecialModes::ShowArenaReport))
    {
        build_arena_report(data);
    }
#endif // BUILD_TRACK_ARENA

    data->feed->build(data->core_draw_lst, data->atlas);

    // Draw some FPS.
    if (implies(data->ui_state->special, SpecialModes::ShowFPS))
    {
        const bool update_fps_txt = (data->fps_data->last_update - data->fps_data->last_fps_update) > 250;
        if (update_fps_txt)
        {
            data->fps_data->fps_text = fmt_string(data->fps_data->fmt_buf, "FPS: %.2f\nAverage dt: %ums\nLast frame dt: %ums\nLast layout dt: %ums",
                                            data->fps_data->fps,
                                            data->ms_frame_counter->average_dt(),
                                            data->fps_data->last_frame_dt,
                                            data->fps_data->last_frame_layout_dt);
            data->fps_data->last_fps_update = data->fps_data->last_update;
        }
        constexpr std::string_view longest_str = "Last layout dt: 100ms";
        constexpr Vec4f color = hex_to_vec4f(0x00FF00FF);
        const Glyph::FontSize font_size = Glyph::FontSize{ Config::diff_state().diff_font_size };
        auto font_ctx = data->atlas->render_font_context(font_size);
        // Get the longest width so we can layout the fps counter on the RHS.
        float longest_x = font_ctx.measure_text(longest_str).x;
        CmdBuffer::start_glyph_run(data->core_draw_lst, Render::VertShader::OneOneTransform);
        // Put it in the top right corner.
        Vec2f pos{ rep(screen.width) - (longest_x + 10.f), rep(screen.height) - rep(font_size) + 0.f };
        for EachIndex(i, data->fps_data->fps_text.size)
        {
            char c = data->fps_data->fps_text.str[i];
            if (c == '\n')
            {
                pos.x = rep(screen.width) - (longest_x + 10.f);
                pos.y -= rep(font_size) + 2.f;
            }
            else
            {
                pos = font_ctx.render_glyph(data->core_draw_lst, c, pos, color);
            }
        }
    }

    if (implies(data->ui_state->special, SpecialModes::ShowGlyphs))
    {
        CmdBuffer::start_images(data->core_draw_lst, Render::VertShader::NoTransform);
        auto width = rep(screen.width);
        auto height = rep(screen.height);
        CmdBuffer::render_image(data->core_draw_lst,
                                Render::FragShader::BasicColor,
                                Vec2f(-width + 0.f, 0.f),
                                Vec2f(width * 2.f, -height * 2.f),
                                Vec2f(0.f, 0.f),
                                Vec2f(1.f, 1.f),
                            hex_to_vec4f(0xFFFFFF00));
    }

    if (*data->cmd_mode == CommandMode::DiffDirPanel)
    {
        build_diff_dir_panel(data);
    }

    // Build this last.
    {
        auto resp = Diff::build_diff_panel(data->diff_panel, data->cmd_lst, data->core_draw_lst, data->ui_state, data->feed);
        if (resp.updated_cfg)
        {
            notify_config_update(NotifyConfigExplorer::Yes, data);
        }
    }

    // Special overlay for hotkey binding.
    if (data->ui_state->rebinding_hotkey)
    {
        auto font_size = Glyph::FontSize{ Config::widget_state().window_title_font_size };
        auto clip = CmdBuffer::current_clip(*data->core_draw_lst);
        Widgets::TextboxInput in{
            .text = "Rebinding hotkey | Press Esc to cancel",
            .padding = 2.f,
            .thickness = 2.f,
        };
        auto font_ctx = data->atlas->render_font_context(font_size);
        Vec2f size = Widgets::measure_textbox(&font_ctx, in);
        Vec2f center = center_clip(clip);
        in.pos.x = center.x - size.x / 2.f;
        in.pos.y = center.y - size.y / 2.f;
        constexpr auto flags = Widgets::BuildTextboxFlags::Fill | Widgets::BuildTextboxFlags::Strike;
        auto palette = *CmdBuffer::current_palette(*data->core_draw_lst);
        palette.fill = palette.background;
        CmdBuffer::push_color_palette(data->core_draw_lst, palette);
        Widgets::build_textbox(data->core_draw_lst, data->ui_state, &font_ctx, in, flags);
        CmdBuffer::pop_color_palette(data->core_draw_lst);
    }

    // Add the core draw list.
    // Note: We add this _after_ any draw lists added above because this needs to be
    // drawn on top of everything else.
    CmdBuffer::push_draw_list(data->cmd_lst, CmdBuffer::DrawListLayer::_0, data->core_draw_lst);

    // We want to draw tooltips on top of everything, so we push it to the top-most layer.
    if (data->ui_state->tooltip.enabled)
    {
        CmdBuffer::push_draw_list(data->cmd_lst, CmdBuffer::DrawListLayer::Top, data->ui_state->tooltip.lst);
    }

    CmdBuffer::pop_clip(data->core_draw_lst);
    CmdBuffer::pop_texture(data->core_draw_lst);
    CmdBuffer::pop_color_palette(data->core_draw_lst);

    CmdBuffer::end_frame(data->cmd_lst);
    ui_end_frame(data->ui_state);

    const uint32_t turnover_ticks_layout = rep(OS::get_ticks32());

    // Core draw to GPU.
    Render::draw_cmd_list(data->renderer, data->cmd_lst);

    CmdBuffer::cmd_list_consumed(data->cmd_lst);

    // Finished rendering.  Unbind the frame buffer and blit it for displaying.
    Render::apply_framebuffer(data->renderer, screen);

    const uint32_t turnover_ticks = rep(OS::get_ticks32());

    data->fps_data->last_update = start;
    data->fps_data->last_frame_dt = turnover_ticks - start;
    data->fps_data->last_frame_layout_dt = turnover_ticks_layout - start;

    data->ms_frame_counter->record_dt(turnover_ticks - start);
    data->fps_data->fps = 1.f / dt;

    // Swap the buffer.
    Render::window_end_frame(OS::core_window());

    // Populate window properties.
    process_window_state(OS::core_window());

    Render::consume_frame();

#if BUILD_DEBUG
    Arena::validate_scratch_arenas();
#endif // BUILD_DEBUG
}

struct UpdateRenderSizeData
{
    ScreenDimensions new_screen;
    RenderCoreData* render_data;
};

void update_render_size(UpdateRenderSizeData update_data)
{
    // No need to resize.
    if (update_data.new_screen == *update_data.render_data->screen)
        return;

#if 0 // DPI experiments.
    const float scale_factor = 1.f / get_platform_dpi_pixel_ratio();
    w = static_cast<int>(w * scale_factor);
    h = static_cast<int>(h * scale_factor);
#endif

    // Update the renderers.
    *update_data.render_data->screen = update_data.new_screen;

    Render::update_resolution(update_data.render_data->renderer,
                                Vec2f(  static_cast<float>(update_data.new_screen.width),
                                        static_cast<float>(update_data.new_screen.height)));
    Render::screen_resize(update_data.new_screen);

    // More?
}

void update_frame(RenderCoreData* data)
{
    auto screen = OS::client_size();
    UpdateRenderSizeData update_data{ .new_screen = screen,
                                        .render_data = data };
    // Note: Handles resizes internally.
    update_render_size(update_data);
    render_core(data);
}

void query_os_events(Arena::Arena* arena, OS::Events* events)
{
    PROF_SCOPE();
    // Only wait if there are no frames requested.
    OS::query_events(arena, events, make_yes_no<OS::Wait>(Render::frames_remaining() == 0));
}

void dequeue_os_event(OS::Events* events)
{
    assert(events->count > 0);
    SLLQueuePop(events->first, events->last);
    --events->count;
}

// The amount to shift by to ensure that we have at least 32 bits minimum.
static constexpr uint32_t bit_shift_amount = 5;
static constexpr uint32_t bit_mask = Constants::bitmask5;

// Largely cribbed from ImBitArray.
template<int Count>
struct BitArray
{
    uint32_t storage[(Count + bit_mask) >> bit_shift_amount];

    bool test_bit(int n) const
    {
        return (storage[n >> bit_shift_amount] & (1U << (n & bit_mask))) != 0;
    }

    void set_bit(int n)
    {
        storage[n >> bit_shift_amount] |= (1U << (n & bit_mask));
    }
};

uint32_t flatten_os_events(UI::UIState* state, OS::Events* events, const ScreenDimensions& screen, RenderCoreData* data)
{
    PROF_SCOPE();
    constexpr uint64_t text_buf_size = 128;
    // Initialize text input event buffers.
    state->in_mod_buf.str = Arena::push_array_no_zero<char>(state->post_frame_arena, text_buf_size);
    state->in_mod_buf.size = 0;
    state->in_buf.str = Arena::push_array_no_zero<char>(state->post_frame_arena, text_buf_size);
    state->in_buf.size = 0;

    BitArray<count_of<OS::Key>> key_changed{};
    uint32_t consumed = 0;
    uint32_t eat_text_event = 0;
    bool quit_loop = false;
    const bool noisy_events = Config::system_core().noisy_events;
    for (OS::Event* e = events->first, *next = nullptr;
        e != nullptr;
        e = next)
    {
        next = e->next;
        switch (e->sort)
        {
        case OS::EventSort::Quit:
            state->want_quit = true;
            Render::request_frames();
            break;
        case OS::EventSort::Release:
            {
                // We need to consume this next frame.
                if ((state->keys[rep(e->key)].down != false and key_changed.test_bit(rep(e->key)))
                    or state->hotkeys.hk != Hotkey::None)
                {
                    quit_loop = true;
                    break;
                }
                key_changed.set_bit(rep(e->key));
                state->keys[rep(e->key)].down = false;
                switch (e->key)
                {
                case OS::Key::LeftMouseButton:
                case OS::Key::MiddleMouseButton:
                case OS::Key::RightMouseButton:
                    ui_mouse_up(*e, state, screen);
                    break;
                default:
                    ui_key_up(*e, state);
                }
                Render::request_frames();
            }
            break;
        case OS::EventSort::Press:
            {
                // We need to consume this next frame.
                if ((state->keys[rep(e->key)].down != true and key_changed.test_bit(rep(e->key)))
                    or state->hotkeys.hk != Hotkey::None)
                {
                    quit_loop = true;
                    break;
                }
                key_changed.set_bit(rep(e->key));
                switch (e->key)
                {
                case OS::Key::LeftMouseButton:
                case OS::Key::MiddleMouseButton:
                case OS::Key::RightMouseButton:
                    ui_mouse_down(*e, state, screen, data);
                    break;
                default:
                    {
                        eat_text_event = 0;
                        KeyDownResult kd_result = ui_key_down(*e, state, data);
                        if (not kd_result.hotkey_matched)
                        {
                            if (kd_result.c != 0)
                            {
                                // Just truncate the buffer if we hit the size.
                                state->in_mod_buf.size = std::min(state->in_mod_buf.size + 1, text_buf_size);
                                state->in_mod_buf.str[state->in_mod_buf.size - 1] = kd_result.c;
                            }
                        }
                        // Otherwise, we matched a hotkey and we may need to eat a text event.
                        else
                        {
                            // Note: This only records alpha-numeric key events.
                            const bool upper = implies(state->mods, KeyMods::Shift) and kd_result.c >= 'a';
                            eat_text_event = kd_result.c - upper * ('a' - 'A');
                        }
                    }
                }
                Render::request_frames();
            }
            break;
        case OS::EventSort::MouseMove:
            {
                // If the mouse already changed, don't process this movement.
                if (key_changed.test_bit(rep(OS::Key::LeftMouseButton))
                    or key_changed.test_bit(rep(OS::Key::RightMouseButton))
                    or key_changed.test_bit(rep(OS::Key::MiddleMouseButton)))
                {
                    quit_loop = true;
                    break;
                }
                state->mouse.ui_mouse = ui_mouse_pos(*e, screen);
                Render::request_frames();
            }
            break;
        case OS::EventSort::Scroll:
            {
                state->mouse.wheel_delta = state->mouse.wheel_delta + e->wheel_delta;
                Render::request_frames();
            }
            break;
        case OS::EventSort::Text:
            // Test upper-case and lower-case.
            if (e->character != eat_text_event)
            {
                UTF8::EncodeInput in;
                std::string_view text = UTF8::utf8_encode(&in, e->character);
                char* append_to = state->in_buf.str + state->in_buf.size;
                uint64_t cpy_size = std::min(text_buf_size - state->in_buf.size, text.size());
                memcpy(append_to, text.data(), cpy_size);
                state->in_buf.size += cpy_size;
                Render::request_frames();
            }
            break;
        case OS::EventSort::FocusLost:
            // We don't want lingering UI state.
            ui_focus_lost(state);
            break;
        case OS::EventSort::FileDrop:
            state->dropped_files = events->drop_files;
            state->mouse.ui_mouse = ui_mouse_pos(*e, screen);
            Render::request_frames();
            break;
        // Special events that are populated at a higher level.
        case OS::EventSort::GapThreadWakeup:
            Render::request_frames();
            break;
        }

        if (quit_loop)
            break;
        dequeue_os_event(events);
        ++consumed;

        if (noisy_events)
        {
            char fmt_buf[512];
            auto msg = fmt_string(fmt_buf, "Consumed event: %S, key(%S), mouse_pos(%.2f,%.2f), wheel(%.2f,%.2f), repeat(%u), wnd(%llu)",
                                    OS::key_to_string(e->key),
                                    OS::event_sort_string(e->sort),
                                    e->pos.x, e->pos.y,
                                    e->wheel_delta.x, e->wheel_delta.y,
                                    e->repeat_count,
                                    rep(e->window));
            data->feed->queue_info(msg);
        }
    }

    // It kind of doesn't matter what the next frame hotkey was.  All that matters is
    // that our current hotkey is not set.  We tie this to the event system so that we
    // cannot start a hotkey from a UI frame alone.
    if (state->hotkeys.hk == Hotkey::None)
    {
        state->hotkeys.hk = state->hotkeys.next_frame_hk;
    }
    state->hotkeys.next_frame_hk = Hotkey::None;

    if (state->hotkeys.cust_hk.id == CustomHotkeyID::None)
    {
        state->hotkeys.cust_hk.id = state->hotkeys.cust_hk.next_frame_hk;
    }
    state->hotkeys.cust_hk.next_frame_hk = CustomHotkeyID::None;

    return consumed;
}

void show_flattened_events(const UIState& state, Feed::MessageFeed* feed)
{
    feed->queue_info("------ FRAME EVENTS BEGIN ------");
    // Mouse.
    {
        char fmt_buf[200];
        String8 mouse = fmt_string(fmt_buf, "Mouse: pos(%d,%d), wheel(%.2f,%.2f), Ldown(%d)Cl(%d), Rdown(%d)Cl(%d), Mdown(%d)Cl(%d)",
                                    state.mouse.ui_mouse.x,
                                    state.mouse.ui_mouse.y,
                                    state.mouse.wheel_delta.x,
                                    state.mouse.wheel_delta.y,
                                    state.mouse.down[rep(MouseButton::L)], state.mouse.click_count[rep(MouseButton::L)],
                                    state.mouse.down[rep(MouseButton::R)], state.mouse.click_count[rep(MouseButton::R)],
                                    state.mouse.down[rep(MouseButton::Mid)], state.mouse.click_count[rep(MouseButton::Mid)]);
        feed->queue_info(mouse);
    }
    feed->queue_info("------ FRAME EVENTS END ------");
}

void process_global_events(RenderCoreData* data)
{
    PROF_SCOPE();
    UIState* state = data->ui_state;

    // Process dropped files.
    if (state->dropped_files.count != 0)
    {
        char fmt_buf[512];
        if (state->dropped_files.count > 1)
        {
            data->feed->queue_warning("Only first file in drop list will be applied.");
        }

        String8 first_file = state->dropped_files.first->string;
        if (OS::regular_file_exists(first_file))
        {
            String8 msg = fmt_string(fmt_buf, "Dropped: %S.", first_file);
            data->feed->queue_info(msg);
            Diff::diff_panel_try_file_drop(data->diff_panel, first_file, state, data->feed);
        }
        else if (*data->cmd_mode == CommandMode::DiffDirPanel
                and OS::directory_exists(first_file))
        {
            String8 msg = fmt_string(fmt_buf, "Dropped: %S.", first_file);
            data->feed->queue_info(msg);
            Diff::diff_dir_panel_try_dir_drop(data->diff_dir_panel, first_file, state, data->feed);
        }
        else
        {
            String8 msg = fmt_string(fmt_buf, "Cannot drop: %S.", first_file);
            data->feed->queue_error(msg);
        }
        state->dropped_files = { };
    }

    if (hotkey(*state, Hotkey::GLB_ToggleFullscreen))
    {
        OS::OSWindow wind = OS::core_window();
        if (OS::window_fullscreened(wind))
        {
            OS::window_windowed(wind);
        }
        else
        {
            OS::window_fullscreen(wind);
        }
    }

    // Debug.
    if (down(*state, OS::Key::F11))
    {
        if (implies(state->mods, KeyMods::Ctrl))
        {
            state->special = toggle(state->special, SpecialModes::ShowGlyphs);
        }
    }

    if (hotkey(*state, Hotkey::GLB_OpenArenaTracker))
    {
#ifdef BUILD_TRACK_ARENA
            state->special = toggle(state->special, SpecialModes::ShowArenaReport);
            data->arena_report->start(*data->screen, state);
#else
            data->feed->queue_warning(str8_mut(str8_literal("Arena tracking disabled.  Build with 'track_arena'.")));
#endif // BUILD_TRACK_ARENA
    }

    if (hotkey(*state, Hotkey::GLB_ReloadConfig))
    {
        // We're going to open a new editor for the config.
        auto default_cfg_file = default_config_file(data->ui_state->frame_arena);
        if (Config::load_config(default_cfg_file, data->feed))
        {
            notify_config_update(NotifyConfigExplorer::Yes, data);
            data->feed->queue_info("Config reloaded.");
        }
    }

    if (hotkey(*state, Hotkey::GLB_LoadDefaultFont)
        and (*data->cmd_mode == CommandMode::None))
    {
        data->atlas->try_load_default_font_asset(data->feed);
        // Save in the current config.
        auto system_font_cfg = Config::system_fonts();
        system_font_cfg.current_font = str8_empty;
        Config::update(system_font_cfg);
        if (*data->cmd_mode != CommandMode::None)
        {
            clear_command_mode(data);
        }
    }

    if (hotkey(*state, Hotkey::GLB_ToggleShowFPS))
    {
        data->feed->queue_info("Toggle show FPS.");
        state->special = toggle(state->special, SpecialModes::ShowFPS);
    }

    if (hotkey(*state, Hotkey::GLB_ShowHelp)
        and *data->cmd_mode == CommandMode::None)
    {
        change_mode(data->cmd_mode, CommandMode::Help);
        data->help->start(*data->screen, state);
    }

    if (hotkey(*state, Hotkey::GLB_ConfigExplorer)
        and (*data->cmd_mode == CommandMode::None))
    {
        change_mode(data->cmd_mode, CommandMode::ConfigExplorer);
        data->config_explorer->start(*data->screen, data->ui_state);
    }

    if (hotkey(*state, Hotkey::GLB_ConfigExplorerPath))
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String8 cfg_dir = default_config_directory(scratch.arena);
        OS::open_path_in_explorer(cfg_dir);
        Arena::scratch_end(scratch);
    }

    if (hotkey(*state, Hotkey::GLB_OpenDiffDirPanel)
        and (*data->cmd_mode == CommandMode::None))
    {
        change_mode(data->cmd_mode, CommandMode::DiffDirPanel);
        Diff::diff_dir_panel_start(data->diff_dir_panel, *data->screen, data->ui_state);
        // Emit some helpful navigation info.
        {
            auto scratch = Arena::scratch_begin(Arena::no_conflicts);
            String8 msg = str8_fmt(scratch.arena, "%S to navigate to next diff.", Hotkeys::hotkey_as_string(scratch.arena, Hotkey::GLB_OpenDiffDirNext));
            data->feed->queue_info(msg);
            msg = str8_fmt(scratch.arena, "%S to navigate to previous diff.", Hotkeys::hotkey_as_string(scratch.arena, Hotkey::GLB_OpenDiffDirPrev));
            data->feed->queue_info(msg);
            Arena::scratch_end(scratch);
        }
    }

    if (hotkey(*state, Hotkey::GLB_OpenDiffDirNext)
        or hotkey(*state, Hotkey::GLB_OpenDiffDirPrev))
    {
        Diff::NextDiffOrder order = hotkey(*state, Hotkey::GLB_OpenDiffDirNext) ? Diff::NextDiffOrder::Next : Diff::NextDiffOrder::Previous;
        if (Diff::diff_dir_panel_nav_diff(data->diff_dir_panel, order, data->feed))
        {
            uint64_t idx = Diff::diff_dir_panel_selected_diff(data->diff_dir_panel);
            Diff::DiffDirDiffResults results = Diff::diff_dir_panel_cached_diffs(data->diff_dir_panel, idx);
            Diff::diff_panel_sink_cached_diffs(data->diff_panel, results);
        }
    }

    // An additional way to increase the font size is to wheel the mouse.
    const bool wheel_font_down = implies(state->mods, KeyMods::Ctrl) and vscroll(*state) == VScrollResult::Down;
    if (hotkey(*state, Hotkey::GLB_DecreaseFontSize)
        or wheel_font_down)
    {
        Config::add_delta_all_font_sizes(-2);
        data->atlas->rebuild_atlas();
        notify_config_update(NotifyConfigExplorer::Yes, data);

        // Eat the scroll value so widgets do not process it.
        if (wheel_font_down)
        {
            state->mouse.wheel_delta.y = 0.f;
        }
    }

    const bool wheel_font_up = implies(state->mods, KeyMods::Ctrl) and vscroll(*state) == VScrollResult::Up;
    if (hotkey(*state, Hotkey::GLB_IncreaseFontSize)
        or wheel_font_up)
    {
        Config::add_delta_all_font_sizes(2);
        data->atlas->rebuild_atlas();
        notify_config_update(NotifyConfigExplorer::Yes, data);

        // Eat the scroll value so widgets do not process it.
        if (wheel_font_up)
        {
            state->mouse.wheel_delta.y = 0.f;
        }
    }

    if (down(*state, OS::Key::Esc))
    {
        if (*data->cmd_mode != CommandMode::None)
        {
            clear_command_mode(data);
            // Eat the event so nobody else tries to process it.
            eat(data->ui_state, OS::Key::Esc);
        }
    }

    // Process this last.
    if (hotkey(*state, Hotkey::GLB_Close))
    {
        if (*data->cmd_mode == CommandMode::None)
        {
            // Close the app.
            state->want_quit = true;
        }
        else
        {
            clear_command_mode(data);
        }
    }
}

int gap_main_entry(int argc, char** argv)
{
    PROF_BEGIN(main_ctx, "Main entry");

    // This is the arena for any persistent data that lives in main.
    // I need to be careful about not putting too much junk in here, pretty much only stack-bound stuff.
    Arena::Arena* main_entry_arena = Arena::alloc(Arena::default_params);

    // Initialize the config very early on so any initialization below can use default values, if necessary.
    Config::init();

    Feed::MessageFeed message_feed;

    Feed::global_feed(&message_feed);

    PROF_BEGIN(cfg_load_ctx, "Load config");
    // Note: The config needs to be loaded before we load the file so that when the editor goes to build the model
    // it will have the correct colors, fonts, etc.
    const String8 default_cfg_file = default_config_file(main_entry_arena);
    if (not OS::file_or_path_exists(default_cfg_file))
    {
        message_feed.queue_info("No existing config, creating...");
        if (Config::save_config(default_cfg_file, &message_feed))
        {
            char fmt_buf[512];
            String8 msg = fmt_string(fmt_buf, "Config created at: %S", default_cfg_file);
            message_feed.queue_info(msg);
        }
    }
    else if (Config::load_config(default_cfg_file, &message_feed))
    {
        char fmt_buf[512];
        String8 msg = fmt_string(fmt_buf, "Config loaded at: %S", default_cfg_file);
        message_feed.queue_info(msg);
    }
    PROF_END(cfg_load_ctx);

    PROF_BEGIN(main_wnd_ctx, "Window init");
    OS::OSWindow window = OS::init_window(Config::system_core().core_window_rect, str8_mut(str8_literal("gap")));
    if (window == OS::OSWindow::Sentinel)
    {
        fprintf(stderr, "ERROR: Could not create core window.");
        return 1;
    }

    OS::window_minimum_size({ Width{ 300 }, Height{ 300 } });
    PROF_END(main_wnd_ctx);

    PROF_BEGIN(renderer_ctx, "Renderer init arenas");
    if (not Render::init_renderer_arenas())
    {
        fprintf(stderr, "ERROR: Could not initialize renderer arenas.");
        return 1;
    }
    PROF_END(renderer_ctx);

    // Initial window size.
    ScreenDimensions screen = OS::client_size();
    PROF_BEGIN(common_obj_ctx, "Common objects setup");

    // Core draw list.
    CmdBuffer::DrawListAllocator draw_lst_alloc{};
    CmdBuffer::CmdList cmd_lst{};

    // Setup the command list arenas.
    cmd_lst.cmd_list_arena = Arena::alloc(Arena::default_params);
    draw_lst_alloc.alloc_arena = Arena::alloc(Arena::default_params);

    // Setup the draw list allocator.
    CmdBuffer::init_alloc(&draw_lst_alloc);

    CmdBuffer::DrawList* core_draw_lst = CmdBuffer::alloc_draw_list();

    Render::FrameRenderer* renderer = Render::make_platform_renderer(main_entry_arena);
    Glyph::Atlas atlas;
    Clipboard::ClipboardManager clipboard;

    // Setup the thread pool.
    Thread::ThreadPool thread_pool;

    PROF_END(common_obj_ctx);

    thread_pool.startup(main_entry_arena);

    Thread::set_system_thread_pool(&thread_pool);

    PROF_BEGIN(hotkey_load_ctx, "Load hotkeys");
    // Setup hotkeys.
    Hotkeys::init();
    if (OS::regular_file_exists(Config::hotkey_state().hotkeys))
    {
        // Load.
        if (Hotkeys::load(Config::hotkey_state().hotkeys, &message_feed))
        {
            char fmt_buf[512];
            String8 msg = fmt_string(fmt_buf, "Hotkeys loaded at: %S", Config::hotkey_state().hotkeys);
            message_feed.queue_info(msg);
        }
        else
        {
            message_feed.queue_warning("Hotkeys failed to load.  Only defaults available.");
        }
    }
    else
    {
        char fmt_buf[512];
        if (Config::hotkey_state().hotkeys.size == 0)
        {
            auto scratch = Arena::scratch_begin(Arena::no_conflicts);
            String8 cfg_dir = default_config_directory(scratch.arena);
            String8 hotkey_cfg = OS::combine_paths(scratch.arena, cfg_dir, str8_mut(str8_literal("hotkeys.toml")));
            bool need_create = not OS::regular_file_exists(hotkey_cfg);
            // Try to load it.
            if (not need_create
                and Hotkeys::load(hotkey_cfg, &message_feed))
            {
                // Store the path.
                auto hotkey_state = Config::hotkey_state();
                Config::update_string(&hotkey_state.hotkeys, hotkey_cfg);
                Config::update(hotkey_state);
                String8 msg = fmt_string(fmt_buf, "Hotkeys loaded at: %S", Config::hotkey_state().hotkeys);
                message_feed.queue_info(msg);
            }
            else
            {
                need_create = true;
            }

            if (need_create)
            {
                message_feed.queue_info("Hotkey file does not exist.  Creating default...");
                if (Hotkeys::save(hotkey_cfg, &message_feed))
                {
                    String8 msg = fmt_string(fmt_buf, "Hotkey file created at: %S", hotkey_cfg);
                    auto hotkey_state = Config::hotkey_state();
                    Config::update_string(&hotkey_state.hotkeys, hotkey_cfg);
                    Config::update(hotkey_state);
                    message_feed.queue_info(msg);
                }
                else
                {
                    String8 msg = fmt_string(fmt_buf, "Unable to create hotkeys file '%S'", hotkey_cfg);
                    message_feed.queue_error(msg);
                }
            }
            Arena::scratch_end(scratch);
        }
        else
        {
            String8 msg = fmt_string(fmt_buf, "Hotkey file '%S' cannot be loaded or does not exist.", Config::hotkey_state().hotkeys);
            message_feed.queue_warning(msg);
        }
    }
    PROF_END(hotkey_load_ctx);

    Theme::apply_border_color(window, &message_feed);

    if (not atlas.init(&message_feed))
        return 1;

    if (not Render::init(screen))
        return 1;

#ifndef NDEBUG
    Render::display_renderer_version();
#endif // NDEBUG

    // Populate initial resolutions.
    Render::update_resolution(renderer, Vec2f(  static_cast<float>(rep(screen.width)),
                                                static_cast<float>(rep(screen.height))));

    // Now we can populate the atlas since the renderer set up the graphics context.
    if (not atlas.populate_atlas())
        return 1;

    // Main loop state.
    bool quit = false;

    UIState ui_state;
    CommandMode cmd_mode = CommandMode::None;
    MsFrameCounter ms_frame_counter;
    FPSData fps_data;

    // Set up frame arenas.
    ui_state.frame_arena = Arena::alloc(Arena::default_params);
    ui_state.post_frame_arena = Arena::alloc(Arena::default_params);

    // Setup last path buffer.  Allocate this on the main frame arena.
    ui_state.last_path_buf = Arena::push_array<UI::LastPathBuffer>(main_entry_arena, 1);

    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String8 msg = str8_fmt(scratch.arena, "Press '%S' for help.", Hotkeys::hotkey_as_string(scratch.arena, Hotkeys::Hotkey::GLB_ShowHelp));
        message_feed.queue_warning(msg);
        Arena::scratch_end(scratch);
    }

    PROF_BEGIN(ui_obj_ctx, "Setup UI objects");

    // Editor-like UI elements.
    Config::Explorer config_explorer{ &atlas };
    Help::Help help{ &atlas };
    Arena::Report::ArenaReport arena_report{ &atlas };
    Diff::DiffPanel* diff_panel = Diff::make_diff_panel(&atlas);
    Diff::DiffDirPanel* diff_dir_panel = Diff::make_diff_dir_panel(&atlas);

    // Setup various command palettes.
    atlas.sync_config();
    config_explorer.sync_config();
    help.sync_config();
    arena_report.sync_config();
    Diff::diff_panel_sync_config(diff_panel, &message_feed);
    Diff::diff_dir_panel_sync_config(diff_dir_panel);

    RenderCoreData render_core_data = {
        .screen = &screen,
        .cmd_mode = &cmd_mode,
        .renderer = renderer,
        .atlas = &atlas,
        .feed = &message_feed,
        .diff_panel = diff_panel,
        .diff_dir_panel = diff_dir_panel,
        .config_explorer = &config_explorer,
        .help = &help,
        .ms_frame_counter = &ms_frame_counter,
        .fps_data = &fps_data,
        .ui_state = &ui_state,
        .cmd_lst = &cmd_lst,
        .core_draw_lst = core_draw_lst,
        .clipboard = &clipboard,
        .arena_report = &arena_report,
    };

    OS::populate_core_render_data(&render_core_data);

    PROF_END(ui_obj_ctx);

    // Now that the atlas is populated, we can load the file(s).
    {
        bool loaded_dir_a = false;
        if (argc > 1)
        {
            auto scratch = Arena::scratch_begin(Arena::no_conflicts);
            bool loaded_a = false;
            for (int i = 1; i < argc; ++i)
            {
                String8 path = str8_cstr(argv[i]);
                if (OS::regular_file_exists(path))
                {
                    // Create a temp arena so we can do this lots of times.
                    auto tmp = Arena::temp_begin(scratch.arena);
                    Diff::TextFile file = Diff::text_file_read(tmp.arena, path);
                    if (not loaded_a)
                    {
                        Diff::diff_panel_file_A(diff_panel, file);
                        loaded_a = true;
                    }
                    else
                    {
                        Diff::diff_panel_file_B(diff_panel, file);
                    }
                    Arena::temp_end(tmp);
                }
                else if (OS::directory_exists(path))
                {
                    if (not loaded_dir_a)
                    {
                        Diff::diff_dir_panel_dir_A(diff_dir_panel, path, &message_feed);
                        loaded_dir_a = true;
                    }
                    else
                    {
                        Diff::diff_dir_panel_dir_B(diff_dir_panel, path, &message_feed);
                    }
                }
                else
                {
                    char fmt_buf[512];
                    String8 msg = fmt_string(fmt_buf, "Argument '%S' is not a regular file; ignoring", path);
                    message_feed.queue_error(msg);
                }
            }
            Arena::scratch_end(scratch);
        }
        Diff::diff_panel_apply_diff(diff_panel, &message_feed);
        // Open the directory comparison panel.
        if (loaded_dir_a)
        {
            // Apply the diff as well.
            Diff::diff_dir_panel_apply_diff(diff_dir_panel, &message_feed);
            ui_state.hotkeys.next_frame_hk = Hotkey::GLB_OpenDiffDirPanel;
        }
    }

    PROF_END(main_ctx);

    // Kickoff rendering.
    Render::request_frames();

    // State for async tasks.
    OS::MicroSec last_async_check = OS::now_microseconds();

    // Default cursor (because we're most likely on text).
    OS::set_cursor(OS::CursorStyle::IBeam);

    // Arena for events.
    Arena::Arena* os_event_arena = Arena::alloc(Arena::default_params);
    OS::Events events{};

    // Now that we have everything set up, let's see if we need to apply screen state.
    if (Config::system_core().fullscreen)
    {
        OS::window_fullscreen(OS::core_window());
    }
    else if (Config::system_core().maximized)
    {
        OS::window_maximize(OS::core_window());
    }

    while (not quit)
    {
        query_os_events(os_event_arena, &events);
        uint32_t consumed_count = flatten_os_events(&ui_state, &events, screen, &render_core_data);
        if (Config::system_core().noisy_flattened_events and consumed_count != 0)
        {
            show_flattened_events(ui_state, &message_feed);
        }
        process_global_events(&render_core_data);
        // It's possible to have processed an event but not actually have anything to render.
        if (Render::frames_remaining() <= 0)
            continue;

        // Core rendering.
        {
            render_core(&render_core_data);
        }

        if (Config::needs_save())
        {
            Config::save_config(default_cfg_file, &message_feed);
        }

        if (Hotkeys::needs_save())
        {
            Hotkeys::save(Config::hotkey_state().hotkeys, &message_feed);
        }

        // If all events are consumed, clear the arena.
        if (events.count == 0)
        {
            Arena::clear(os_event_arena);
        }

        // Check every 2 seconds.
        if (rep(OS::now_microseconds()) - rep(last_async_check) > static_cast<uint64_t>(Thousand(Config::system_core().async_update_frequency_ms)))
        {
            thread_pool.async_notify();
            last_async_check = OS::now_microseconds();
        }

        quit = ui_state.want_quit;
    }

    OS::destroy_window(window);

    thread_pool.shutdown();

    return 0;
}

namespace UI
{
    struct GapLogo
    {
        Render::BasicTexture tex;
        ScreenDimensions size;
    };

    constinit GapLogo logo = { .tex = Render::BasicTexture::Invalid,
                                .size = {} };

    LogoInfo gap_logo(Feed::MessageFeed* feed)
    {
        if (logo.tex == Render::BasicTexture::Invalid)
        {
            constexpr ScreenDimensions desired_size{ Width{ 640 }, Height{ 480 } };
            auto result = SVG::load_svg_asset(Assets::AssetID::Logo, feed, desired_size);
            logo.tex = result.tex;
            logo.size = result.size;
        }
        return { .tex = logo.tex, .dim = logo.size };
    }

    void update_last_path(UIState* state, String8 new_path)
    {
        // Cannot update.
        if (new_path.size > std::size(state->last_path_buf->array))
            return;
        new_path = directory_of_file(new_path);
        memcpy(state->last_path_buf->array, new_path.str, new_path.size * sizeof(char));
        state->last_path.str = state->last_path_buf->array;
        state->last_path.size = new_path.size;
    }
} // namespace UI

// Unity build.
#ifndef NO_UNITY
#ifdef WIN32
// Just suppress this.
#pragma warning(push)
#pragma warning(disable: 4005) // GL.h(107,9): warning C4005: 'GL_ALL_ATTRIB_BITS': macro redefinition
#endif

#include "arena-report.cpp"
#include "arena.cpp"
#include "assets.cpp"
#include "clipboard-manager.cpp"
#include "concurrent-queue.cpp"
#include "config-explorer.cpp"
#include "config.cpp"
#include "diff-core.cpp"
#include "diff-dir-panel.cpp"
#include "diff-dir-list.cpp"
#include "diff-panel.cpp"
#include "diff-text.cpp"
#include "feed.cpp"
#include "file-tracker.cpp"
#include "gap-strings.cpp"
#include "glyph-cache.cpp"
#include "help.cpp"
#include "hotkeys.cpp"
#include "os-common.cpp"
#include "renderer.cpp"
#include "svg.cpp"
#include "thread-ctx.cpp"
#include "thread.cpp"
#include "tiny-toml.cpp"
#include "utf-8.cpp"
#include "util.cpp"
#include "widgets.cpp"
#include "window-theming.cpp"

// Third-party.
#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>
// Undef so it can be included below.
#undef STB_SPRINTF_IMPLEMENTATION

// Platform-specific.
#ifdef BUILD_OPENGL_RENDERER
#include "renderer-opengl.cpp"
#endif // BUILD_OPENGL_RENDERER

#ifdef WIN32
// Windows goes last.
#define WIN32_GFX
#include "os-win32.cpp"

#ifdef BUILD_OPENGL_RENDERER
#include "renderer-opengl-win32.cpp"
#endif // BUILD_OPENGL_RENDERER

#ifdef BUILD_D3D11_RENDERER
#include "renderer-d3d11-win32.cpp"
#endif // BUILD_D3D11_RENDERER

#pragma warning(pop)
#else // ^^^ WIN32 ^^^ / vvv !WIN32 vvv
#define LINUX_GFX
#include "os-linux.cpp"
#endif // WIN32

#ifdef BUILD_PROFILED
#include "TracyClient.cpp"
#endif // BUILD_PROFILED

#endif // NO_UNITY