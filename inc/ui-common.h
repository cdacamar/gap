#pragma once

#include <stdint.h>
#include <string.h>

#include <string>
#include <bit>

#include "arena.h"
#include "gap-strings.h"
#include "glyph-cache.h"
#include "os.h"
#include "renderer.h"
#include "types.h"
#include "vec.h"
#include "widgets.h"

namespace Feed
{
    class MessageFeed;
} // namespace Feed

namespace UI
{
    using KeyMods = OS::KeyMods;
    using namespace Hotkeys;

    enum class MouseButton
    {
        L,
        R,
        Mid,
        Count
    };

    struct Mouse
    {
        Vec2i ui_mouse{};
        Vec2i ui_prev_mouse{};
        Vec2i initial_down_mouse{};  // If the mouse is down, this tells us where that click started.
        Vec2f wheel_delta{};
        OS::Ticks32 last_click[count_of<MouseButton>]{};
        uint8_t click_count[count_of<MouseButton>]{};
        bool down[count_of<MouseButton>]{};
    };

    enum class SpecialModes : uint8_t
    {
        None             = 0,
        ShowGlyphs       = 1u << 0,
        SuspendRendering = 1u << 1,
        ShowFPS          = 1u << 2,
        ShowArenaReport  = 1u << 3,
    };

    struct KeyState
    {
        bool down;
    };

    using KeyStates = KeyState[count_of<OS::Key>];

    struct DragItemPayload
    {
        std::string data;
        Vec2i start_point{};
    };

    struct DragItem
    {
        Widgets::ID id = Widgets::ID::Zero;
        DragItemPayload payload{};
    };

    struct CustomHotkey
    {
        CustomHotkeyID id = CustomHotkeyID::None;
        CustomHotkeyID rebind_target = CustomHotkeyID::None;
        CustomHotkeyID next_frame_hk = CustomHotkeyID::None;
        CustomHotkeyGroup group = CustomHotkeyGroup::Count;
    };

    struct HotkeyState
    {
        Hotkey hk = Hotkey::None;
        Hotkey rebind_target = Hotkey::None;
        Hotkey next_frame_hk = Hotkey::None;
        CustomHotkey cust_hk = {};
        Widgets::ID rebind_widget = Widgets::ID::Zero;
    };

    struct TooltipState
    {
        CmdBuffer::DrawList* lst = nullptr;
        float expand_offset = 0.f; // Note: This tries to converge to 1.f.
        bool enabled = false;
    };

    struct LastPathBuffer
    {
        char array[KB(1)];
    };

    enum class FrameIndex : uint64_t { };

    struct UIState
    {
        FrameIndex frame_index{};
        Mouse mouse{};
        KeyMods mods{};
        SpecialModes special{};
        String8 in_buf{};
        String8 in_mod_buf{}; // A buffer for alpha-numeric keys even when modifiers are engaged (useful for vim inputs).
        KeyStates keys{};
        HotkeyState hotkeys{};
        Widgets::ID hot_widget = Widgets::ID::Zero;
        Widgets::ID focus_widget = Widgets::ID::Zero;
        Widgets::ID focus_keyboard = Widgets::ID::Zero;
        Widgets::ID focus_window = Widgets::ID::Zero;
        Widgets::ID context_widget = Widgets::ID::Zero; // Context-based widgets, like pop-ups, tooltips, etc.
        Widgets::ID highlight_widget = Widgets::ID::Zero;
        Arena::Arena* frame_arena{};      // Cleared on frame entry.
        Arena::Arena* post_frame_arena{}; // Cleared at the end of the frame.
        DragItem drag{};
        OS::CursorStyle cursor = OS::CursorStyle::Default;
        OS::DropFileList dropped_files{};
        LastPathBuffer* last_path_buf{};
        String8 last_path{};
        TooltipState tooltip{};
        float anim_fast_rate{};
        float anim_faster_rate{};
        bool want_quit = false;
        bool new_suggestions = false;
        bool rebinding_hotkey = false;
    };

    constexpr void change_cursor(UIState* s, OS::CursorStyle c)
    {
        s->cursor = c;
    }

    constexpr bool dragging(const UIState& s, Widgets::ID id)
    {
        return s.drag.id == id;
    }

    constexpr bool drag_threshold_met(const UIState& s)
    {
        constexpr float drag_thresh = 12.f;
        Vec2i dx = s.mouse.ui_mouse - s.mouse.initial_down_mouse;
        return dx.mag2() > drag_thresh;
    }

    template <typename T>
    constexpr void start_drag(UIState* state, Widgets::ID id, Vec2i start, const T& payload)
    {
        state->drag.id = id;
        state->drag.payload.start_point = start;

        struct Payload
        {
            char blob[sizeof(T)];
        };

        Payload p = std::bit_cast<Payload>(payload);
        state->drag.payload.data.resize(sizeof(Payload));
        memcpy(state->drag.payload.data.data(), &p, sizeof(Payload));
    }

    template <typename T>
    constexpr T* drag_payload(UIState* state)
    {
        return reinterpret_cast<T*>(state->drag.payload.data.data());
    }

    constexpr Vec2i mouse_move_drag(const UIState& state)
    {
        auto offset = state.mouse.ui_mouse - state.mouse.ui_prev_mouse;
        return offset;
    }

    constexpr uint8_t clicked_count(const UIState& s, MouseButton b)
    {
        return s.mouse.click_count[rep(b)];
    }

    constexpr bool down(const UIState& s, MouseButton b)
    {
        return s.mouse.down[rep(b)];
    }

    constexpr bool down(const UIState& s, OS::Key k)
    {
        return s.keys[rep(k)].down;
    }

    constexpr void eat(UIState* s, OS::Key k)
    {
        s->keys[rep(k)] = {};
    }

    constexpr bool any_mouse_down(const UIState& s)
    {
        return s.mouse.down[rep(MouseButton::L)]
            or s.mouse.down[rep(MouseButton::R)]
            or s.mouse.down[rep(MouseButton::Mid)];
    }

    enum class VScrollResult
    {
        None,
        Up,
        Down
    };

    constexpr VScrollResult vscroll(const UIState& s)
    {
        if (s.mouse.wheel_delta.y < 0.f)
            return VScrollResult::Down;
        if (s.mouse.wheel_delta.y > 0.f)
            return VScrollResult::Up;
        return VScrollResult::None;
    }

    enum class HScrollResult
    {
        None,
        Left,
        Right
    };

    constexpr HScrollResult hscroll(const UIState& s)
    {
        if (s.mouse.wheel_delta.x < 0.f)
            return HScrollResult::Left;
        if (s.mouse.wheel_delta.x > 0.f)
            return HScrollResult::Right;
        return HScrollResult::None;
    }

    constexpr bool any_scroll_dir(const UIState& s)
    {
        return s.mouse.wheel_delta != 0.f;
    }

    constexpr void clear_drag(UIState* s)
    {
        s->drag.id = Widgets::ID::Zero;
        s->drag.payload = {};
    }

    constexpr void try_set_hot_widget(UIState* s, Widgets::ID id)
    {
        if (s->hot_widget == Widgets::ID::Zero)
        {
            s->hot_widget = id;
        }
    }

    constexpr void try_set_focus_widget(UIState* s, Widgets::ID id)
    {
        if (s->focus_widget == Widgets::ID::Zero)
        {
            s->focus_widget = id;
        }
    }

    constexpr void force_set_focus_widget(UIState* s, Widgets::ID id)
    {
        s->focus_widget = id;
    }

    constexpr bool empty_focus_widget(const UIState& s)
    {
        return s.focus_widget == Widgets::ID::Zero or s.focus_widget == Widgets::ID::Sentinel;
    }

    constexpr bool self_or_empty_focus_widget(const UIState& s, Widgets::ID self)
    {
        return s.focus_widget == self or empty_focus_widget(s);
    }

    constexpr bool hot_widget_set(const UIState& s, Widgets::ID hot)
    {
        return s.hot_widget == hot and hot != Widgets::ID::Zero;
    }

    // A basic heuristic for a widget being clicked.
    constexpr bool std_click_trigger(const UIState& s, Widgets::ID id)
    {
        return hot_widget_set(s, id)
                and s.focus_widget == id
                and not down(s, MouseButton::L);
    }

    constexpr void set_keyboard_widget(UIState* s, Widgets::ID id)
    {
        s->focus_keyboard = id;
    }

    constexpr void set_focus_window(UIState* s, Widgets::ID id)
    {
        s->focus_window = id;
    }

    constexpr void set_context_widget(UIState* s, Widgets::ID id)
    {
        s->context_widget = id;
    }

    constexpr void try_set_highlight_widget(UIState* s, Widgets::ID id)
    {
        if (s->highlight_widget == Widgets::ID::Zero)
        {
            s->highlight_widget = id;
        }
    }

    constexpr bool highlight_widget_set(const UIState& s, Widgets::ID id)
    {
        return s.highlight_widget == id and id != Widgets::ID::Zero;
    }

    constexpr bool hotkey(const UIState& s, Hotkey hk)
    {
        return s.hotkeys.hk == hk;
    }

    constexpr bool custom_hotkey(const UIState& s, CustomHotkeyGroup group)
    {
        return s.hotkeys.cust_hk.id != CustomHotkeyID::None
                and s.hotkeys.cust_hk.group == group;
    }

    constexpr void clear_hotkey_rebind_state(UIState* s)
    {
        s->rebinding_hotkey = false;
        s->hotkeys.rebind_target = Hotkey::None;
        s->hotkeys.cust_hk.rebind_target = CustomHotkeyID::None;
    }

    using CursorStyle = OS::CursorStyle;

    struct AABBData
    {
        Vec2f pos;  // Top-left.
        Vec2f size;
    };

    constexpr bool basic_aabb(const AABBData& box, const Vec2i& point)
    {
        return box.pos.x + box.size.x > point.x
            and box.pos.x <= point.x
            and box.pos.y + box.size.y > point.y
            and box.pos.y <= point.y;
    }

    constexpr bool boxed_aabb(const Vec4f& p_a, const Vec4f& p_b)
    {
        // Same as AABB computation in basic_aabb, just taking 'b's width into account.
        return p_a.p0[0] < p_b.p1[0] and p_a.p1[0] > p_b.p0[0]
            and p_a.p0[1] < p_b.p1[1] and p_a.p1[1] > p_b.p0[1];
    }

    constexpr Vec2i adjusted_mouse_for_viewport(const Vec2i& mouse_pos, const Render::RenderViewport& viewport)
    {
        auto adjusted_mouse = mouse_pos;
        adjusted_mouse.x -= rep(viewport.offset_x);
        adjusted_mouse.y -= rep(viewport.offset_y);
        return adjusted_mouse;
    }

    constexpr Vec2i adjusted_mouse_for_clip(const Vec2i& mouse_pos, const CmdBuffer::ClipRect& clip)
    {
        auto adjusted_mouse = mouse_pos;
        adjusted_mouse.x -= rep(clip.offset_x);
        adjusted_mouse.y -= rep(clip.offset_y);
        return adjusted_mouse;
    }

    constexpr bool mouse_in_viewport(const Vec2i& mouse_pos, const Render::RenderViewport& viewport)
    {
        auto adjusted_mouse = adjusted_mouse_for_viewport(mouse_pos, viewport);
        Vec2f pos{ 0.f, 0.f };
        Vec2f size{ rep(viewport.width) + 0.f, rep(viewport.height) + 0.f };
        return basic_aabb({ .pos = pos, .size = size }, adjusted_mouse);
    }

    constexpr void pad_viewport(Render::RenderViewport* viewport, const Vec2i& padding)
    {
        viewport->width = Width{ rep(viewport->width) - padding.x * 2 };
        viewport->offset_x = Render::ViewportOffsetX{ rep(viewport->offset_x) + padding.x };

        viewport->height = Height{ rep(viewport->height) - padding.y * 2 };
        viewport->offset_y = Render::ViewportOffsetY{ rep(viewport->offset_y) + padding.y };
    }

    constexpr CmdBuffer::ClipRect pad_clip(const CmdBuffer::ClipRect& clip, const Vec2i& padding)
    {
        auto new_clip = clip;
        new_clip.width = Width{ rep(clip.width) - padding.x * 2 };
        new_clip.offset_x = CmdBuffer::OffsetX{ rep(clip.offset_x) + padding.x };

        new_clip.height = Height{ rep(clip.height) - padding.y * 2 };
        new_clip.offset_y = CmdBuffer::OffsetY{ rep(clip.offset_y) + padding.y };
        return new_clip;
    }

    constexpr void offset_viewport(Render::RenderViewport* viewport, const Vec2i& offset)
    {
        viewport->offset_x = Render::ViewportOffsetX{ rep(viewport->offset_x) + offset.x };
        viewport->offset_y = Render::ViewportOffsetY{ rep(viewport->offset_y) + offset.y };
    }

    constexpr Render::ViewportOffsetX offset_from(Render::ViewportOffsetX x, int off)
    {
        return Render::ViewportOffsetX{ rep(x) + off };
    }

    constexpr Render::ViewportOffsetY offset_from(Render::ViewportOffsetY y, int off)
    {
        return Render::ViewportOffsetY{ rep(y) + off };
    }

    constexpr CmdBuffer::OffsetX offset_from(CmdBuffer::OffsetX x, int off)
    {
        return CmdBuffer::OffsetX{ rep(x) + off };
    }

    constexpr CmdBuffer::OffsetY offset_from(CmdBuffer::OffsetY y, int off)
    {
        return CmdBuffer::OffsetY{ rep(y) + off };
    }

    constexpr void move_clip_to_absolute(CmdBuffer::ClipRect* clip, const Vec2i& pos)
    {
        clip->offset_x = offset_from(CmdBuffer::OffsetX{}, pos.x);
        clip->offset_y = offset_from(CmdBuffer::OffsetY{}, pos.y);
    }

    constexpr void size_offset_viewport(Render::RenderViewport* viewport, const Vec2i& offset)
    {
        viewport->width = Width{ rep(viewport->width) + offset.x };
        viewport->height = Height{ rep(viewport->height) + offset.y };
    }

    constexpr float standard_font_padding(Glyph::FontSize font_size)
    {
        return rep(font_size) * 1.60f;
    }

    constexpr CmdBuffer::ClipRect convert(const Render::RenderViewport& vp)
    {
        return {
            .offset_x = CmdBuffer::OffsetX{ rep(vp.offset_x) },
            .offset_y = CmdBuffer::OffsetY{ rep(vp.offset_y) },
            .width = vp.width,
            .height = vp.height,
        };
    }

    constexpr Render::RenderViewport convert(const CmdBuffer::ClipRect& vp)
    {
        return {
            .offset_x = Render::ViewportOffsetX{ rep(vp.offset_x) },
            .offset_y = Render::ViewportOffsetY{ rep(vp.offset_y) },
            .width = vp.width,
            .height = vp.height,
        };
    }

    constexpr bool mouse_in_clip(const Vec2i& mouse_pos, const CmdBuffer::ClipRect& clip)
    {
        auto adjusted_mouse = adjusted_mouse_for_clip(mouse_pos, clip);
        Vec2f pos{ 0.f, 0.f };
        Vec2f size{ rep(clip.width) + 0.f, rep(clip.height) + 0.f };
        return basic_aabb({ .pos = pos, .size = size }, adjusted_mouse);
    }

    // Don't take clip offsets into account.  This is useful for intersecting child content.
    constexpr Vec4f unadjusted_clip_as_vec(const CmdBuffer::ClipRect& clip)
    {
        return Vec4f{
            // p0
            0.f,
            0.f,
            // p1
            rep(clip.width) + 0.f,
            rep(clip.height) + 0.f,
        };
    }

    constexpr Vec4f clip_as_vec(const CmdBuffer::ClipRect& clip)
    {
        return Vec4f{
            // p0
            rep(clip.offset_x) + 0.f,
            rep(clip.offset_y) + 0.f,
            // p1
            rep(clip.offset_x) + rep(clip.width) + 0.f,
            rep(clip.offset_y) + rep(clip.height) + 0.f,
        };
    }

    constexpr CmdBuffer::ClipRect vec_as_clip(const Vec4f& vec)
    {
        return CmdBuffer::ClipRect{
            .offset_x = CmdBuffer::OffsetX(vec.p0[0]),
            .offset_y = CmdBuffer::OffsetY(vec.p0[1]),
            .width = Width(vec.p1[0] - vec.p0[0]),
            .height = Height(vec.p1[1] - vec.p0[1]),
        };
    }

    constexpr bool clips_overlap(const CmdBuffer::ClipRect& a, const CmdBuffer::ClipRect& b)
    {
        // Same as AABB computation in basic_aabb, just taking 'b's width into account.
        Vec4f p_a = clip_as_vec(a);
        Vec4f p_b = clip_as_vec(b);
        return boxed_aabb(p_a, p_b);
    }

    constexpr Vec2f center_clip(const CmdBuffer::ClipRect& clip)
    {
        float x = rep(clip.offset_x) + (rep(clip.width) / 2.f);
        float y = rep(clip.offset_y) + (rep(clip.height) / 2.f);
        return { x, y };
    }

    struct PosSize
    {
        Vec2f pos;
        Vec2f size;
    };

    constexpr PosSize pos_size_clip(const CmdBuffer::ClipRect& clip)
    {
        Vec2f pos{ rep(clip.offset_x) + 0.f, rep(clip.offset_y) + 0.f };
        Vec2f size{ rep(clip.width) + 0.f,
                    rep(clip.height) + 0.f };
        return { .pos = pos, .size = size };
    }

    constexpr CmdBuffer::ClipRect expand_clip_center(const CmdBuffer::ClipRect& clip, float off)
    {
        Vec4f clip_v = clip_as_vec(clip);
        clip_v.p0[0] += (rep(clip.width) / 2.f) * off;
        clip_v.p0[1] += (rep(clip.height) / 2.f) * off;
        clip_v.p1[0] -= (rep(clip.width) / 2.f) * off;
        clip_v.p1[1] -= (rep(clip.height) / 2.f) * off;
        return vec_as_clip(clip_v);
    }

    constexpr CmdBuffer::ClipRect intersect(const CmdBuffer::ClipRect& enclosing, const CmdBuffer::ClipRect& inner)
    {
        auto enclosing_v = clip_as_vec(enclosing);
        auto inner_v = clip_as_vec(inner);
        Vec4f r;
        r.p0[0] = std::max(enclosing_v.p0[0], inner_v.p0[0]);
        r.p0[1] = std::max(enclosing_v.p0[1], inner_v.p0[1]);
        r.p1[0] = std::min(enclosing_v.p1[0], inner_v.p1[0]);
        r.p1[1] = std::min(enclosing_v.p1[1], inner_v.p1[1]);
        return vec_as_clip(r);
    }

    constexpr void clamp_clip_to(CmdBuffer::ClipRect* clip, ScreenDimensions screen)
    {
        auto enclosing_v = clip_as_vec(CmdBuffer::ClipRect::basic(screen));
        auto inner_v = clip_as_vec(*clip);
        float delta_x = std::min(0.f, enclosing_v.p1[0] - inner_v.p1[0]);
        float delta_y = std::min(0.f, enclosing_v.p1[1] - inner_v.p1[1]);
        inner_v.p0[0] += delta_x;
        inner_v.p1[0] += delta_x;
        inner_v.p0[1] += delta_y;
        inner_v.p1[1] += delta_y;
        *clip = vec_as_clip(inner_v);
    }

    constexpr Vec2f resize_to_maintain_aspect_ratio(Vec2f img_size, ScreenDimensions fit_to)
    {
        // If we need to adjust the size given the viewport, do that now.
        if ((img_size.x > rep(fit_to.width))
            or (img_size.y > rep(fit_to.height)))
        {
            float aspect_ratio = static_cast<float>(rep(fit_to.width)) / (rep(fit_to.height));
            float logo_ratio = img_size.x / img_size.y;
            float w = 0.f;
            float h = 0.f;
            if (aspect_ratio < logo_ratio)
            {
                w = rep(fit_to.width) + 0.f;
                h = w / logo_ratio;
            }
            else
            {
                h = rep(fit_to.height) + 0.f;
                w = h * logo_ratio;
            }
            img_size.x = w;
            img_size.y = h;
        }
        return img_size;
    }

    struct LogoInfo
    {
        Render::BasicTexture tex;
        ScreenDimensions dim;
    };

    LogoInfo gap_logo(Feed::MessageFeed* feed);

    void update_last_path(UIState* state, String8 new_path);
} // namespace UI
