#include "basic-scrollbox.h"

#include <algorithm>

#include "config.h"
#include "os.h"
#include "util.h"

namespace UI::Widgets
{
    // ---- ScrollBox ----
    struct ScrollBox::Data
    {
        static constexpr int padding = 2;

        ID v_scroll_id = ID::Zero;
        ID h_scroll_id = ID::Zero;
        Vec2f content_size;
        Vec2f scroll_offset;
        Vec2f movement_offset;
        float scrollbar_width = 10.f;
        int scrollbar_min_size = 20;
    };

    namespace
    {
        Vec2f scroll_offset(const ScrollBox::Data& data)
        {
            return data.scroll_offset + data.movement_offset;
        }

        void populate_scroll_offset(ScrollBox::Data* data, const Vec2f& offset)
        {
            if (Config::system_effects().smooth_scroll)
            {
                data->movement_offset = data->scroll_offset + data->movement_offset - offset;
            }
            data->scroll_offset = offset;
        }

        void populate_scroll_offset_no_anim(ScrollBox::Data* data, const Vec2f& offset)
        {
            data->scroll_offset = offset;
        }

        struct ScrollbarRect
        {
            Vec2f pos;
            Vec2f size;
        };

        ScrollbarRect v_scrollbar_box(ScrollBox::Data* data, const Render::RenderViewport& viewport)
        {
            if (data->content_size.y == 0.f)
                return {};
            // Note: the viewport height is added to content size height because the viewport goes from y(0) to y(content.height).
            float height = rep(viewport.height) * (rep(viewport.height) / (data->content_size.y + rep(viewport.height)));
            height = std::max(height, static_cast<float>(data->scrollbar_min_size));
            const float offset_multiplier = scroll_offset(*data).y / data->content_size.y;
            Vec2f left{ rep(viewport.width) - data->scrollbar_width, 0.f };
            Vec2f size{ data->scrollbar_width, 0.f };
            size.y = height;
            left.y = rep(viewport.height) - height - (rep(viewport.height) - height) * offset_multiplier;
            return { .pos = left, .size = size };
        }

        ScrollbarRect h_scrollbar_box(ScrollBox::Data* data, const Render::RenderViewport& viewport)
        {
            if (data->content_size.x == 0.f)
                return {};
            // So we don't overlap with the vert scrollbar.
            const float vp_width = rep(viewport.width) - data->scrollbar_width;
            // Note: Due to the way content width is adjusted, we add an additional 'view_size_x' here to
            // ensure that we catch the 'end' of the viewable content.
            float width = vp_width * (vp_width / (data->content_size.x + vp_width));
            width = std::max(width, static_cast<float>(data->scrollbar_min_size));
            const float offset_multiplier = scroll_offset(*data).x / (data->content_size.x);
            Vec2f left{};
            Vec2f size{ 0.f, data->scrollbar_width };
            size.x = width;
            left.x = offset_multiplier * (vp_width - width);
            return { .pos = left, .size = size };
        }

        void drag_v_scroll(ScrollBox::Data* data, const UIState& state, const Render::RenderViewport& viewport)
        {
            const Vec2f initial_offset = scroll_offset(*data);
            float height = rep(viewport.height) * rep(viewport.height) / (data->content_size.y + rep(viewport.height));
            height = std::max(height, static_cast<float>(data->scrollbar_min_size));
            const float off = -static_cast<float>(mouse_move_drag(state).y);
            const float off_scaled = data->content_size.y * off / (rep(viewport.height) - height);
            float off_y = std::clamp(initial_offset.y + off_scaled, 0.f, data->content_size.y);
            float off_x = initial_offset.x;
            populate_scroll_offset_no_anim(data, { off_x, off_y });
        }

        void drag_h_scroll(ScrollBox::Data* data, const UIState& state, const Render::RenderViewport& viewport)
        {
            const Vec2f initial_offset = scroll_offset(*data);
            const float vp_width = rep(viewport.width) - data->scrollbar_width;
            float width = vp_width * (vp_width / (data->content_size.x + vp_width));
            width = std::max(width, static_cast<float>(data->scrollbar_min_size));
            const float off = static_cast<float>(mouse_move_drag(state).x);
            const float off_scaled = data->content_size.x * off / (vp_width - width);
            float off_x = std::clamp(initial_offset.x + off_scaled, 0.f, data->content_size.x);
            float off_y = initial_offset.y;
            populate_scroll_offset_no_anim(data, { off_x, off_y });
        }

        bool horiz_scrollbar_needed(ScrollBox::Data* data, const Render::RenderViewport& viewport)
        {
            float view_size_x = rep(viewport.width) + 0.f;
            // Remove the vert scrollbar width.
            view_size_x -= data->scrollbar_width;
            // Note: Due to the way content width is adjusted, we add an additional 'view_size_x' here to
            // ensure that we catch the 'end' of the viewable content.
            return (data->content_size.x + view_size_x) > view_size_x;
        }

        struct TranslatedScroll
        {
            VScrollResult vscr;
            HScrollResult hscr;
            Vec2f wheel_delta;
        };

        TranslatedScroll translate_mouse_wheel(const UI::UIState& state)
        {
            TranslatedScroll result = {};
            result.vscr = vscroll(state);
            result.hscr = hscroll(state);
            result.wheel_delta = state.mouse.wheel_delta;
            // There's a scenario here where we want to uniformly support holing SHIFT and moving
            // the mouse wheel to horizontal scroll.  We do this by detecting if SHIFT is down,
            // adding the y-offset to the x-offset delta and removing the vertical scroll result.
            if (implies(state.mods, UI::KeyMods::Shift))
            {
                // Note: Wheeling 'down' will produce a negative value, but we want this to mean
                // "move left" in our horizontal offset value so we invert it here.
                result.wheel_delta.x += -result.wheel_delta.y;
                // We need to recompute the hscroll.
                if (result.wheel_delta.x < 0.f)
                {
                    result.hscr = HScrollResult::Left;
                }

                if (result.wheel_delta.x > 0.f)
                {
                    result.hscr = HScrollResult::Right;
                }
                result.vscr = VScrollResult::None;
                result.wheel_delta.y = 0.f;
            }
            return result;
        }
    } // namespace [anon]

    ScrollBox::ScrollBox(ID content_id):
        data{ new Data{ .v_scroll_id = make_id_seed(content_id, "vscroll"),
                        .h_scroll_id = make_id_seed(content_id, "hscroll") } } { }

    ScrollBox::~ScrollBox() = default;

    void ScrollBox::content_size(const Vec2f& size)
    {
        data->content_size = size;
        Vec2f off;
        off.y = std::clamp(data->scroll_offset.y, 0.f, data->content_size.y);
        off.x = std::clamp(data->scroll_offset.x, 0.f, data->content_size.x);
        populate_scroll_offset_no_anim(data.get(), off);
    }

    bool ScrollBox::at_end_y() const
    {
        return data->scroll_offset.y == data->content_size.y;
    }

    bool ScrollBox::at_end_x() const
    {
        return data->scroll_offset.x == data->content_size.x;
    }

    void ScrollBox::content_size(const Vec2f& size, const Render::RenderViewport& viewport)
    {
        data->content_size = size;
        // Clamp the content viewport such that we do not add an extra viewport height.
        data->content_size.y = std::clamp(size.y - rep(viewport.height), 0.f, size.y);
        data->content_size.x = std::clamp(size.x - rep(viewport.width), 0.f, size.x);
        Vec2f off;
        off.y = std::clamp(data->scroll_offset.y, 0.f, data->content_size.y);
        off.x = std::clamp(data->scroll_offset.x, 0.f, data->content_size.x);
        populate_scroll_offset_no_anim(data.get(), off);
    }

    void ScrollBox::sync_config()
    {
        data->scrollbar_width = Config::widget_state().scrollbar_width + 0.f;
        data->scrollbar_min_size = Config::widget_state().scrollbar_min_size;
    }

    Vec2f ScrollBox::position() const
    {
        return scroll_offset(*data);
    }

    Render::RenderViewport ScrollBox::content_viewport(const Render::RenderViewport& viewport) const
    {
        const int scrollbar_width = static_cast<int>(data->scrollbar_width);

        // reduce the margins a bit.
        auto new_viewport = viewport;
        pad_viewport(&new_viewport, { Data::padding });
        new_viewport.width = retract(new_viewport.width, scrollbar_width);
        // Make room for the horizontal scrollbar if necessary.
        if (horiz_scrollbar_needed(data.get(), viewport))
        {
            new_viewport.height = retract(new_viewport.height, scrollbar_width);
            new_viewport.offset_y = offset_from(new_viewport.offset_y, scrollbar_width);
        }
        return new_viewport;
    }

    const Vec2f& ScrollBox::content_size() const
    {
        return data->content_size;
    }

    // Direct interaction.
    void ScrollBox::scroll_to(float offset)
    {
        auto off = scroll_offset(*data);
        off.y = std::clamp(offset, 0.f, data->content_size.y);
        populate_scroll_offset(data.get(), off);
    }

    void ScrollBox::scroll_to_no_smooth_scroll(float offset)
    {
        auto off = scroll_offset(*data);
        off.y = std::clamp(offset, 0.f, data->content_size.y);
        populate_scroll_offset_no_anim(data.get(), off);
    }

    void ScrollBox::make_box_viewable(const AABBData& box, const Render::RenderViewport& viewport)
    {
        // Make a box specifically for the viewport.
        auto content_vp = content_viewport(viewport);
        Vec2f content_size{ rep(content_vp.width) + 0.f, rep(content_vp.height) + 0.f };
        Vec2f content_top = scroll_offset(*data);

        // Hit tests.
        if (box.pos.y < content_top.y)
        {
            // Shift top of window up.
            content_top.y += box.pos.y - content_top.y;
        }

        if (box.pos.y + box.size.y > content_top.y + content_size.y)
        {
            // Shift bottom of window down.
            content_top.y -= (content_top.y + content_size.y) - (box.pos.y + box.size.y);
        }

        if (box.pos.x < content_top.x)
        {
            // Shift window left.
            content_top.x = box.pos.x;
        }

        if (box.pos.x + box.size.x > content_top.x + content_size.x)
        {
            // Shift right of window to accommodate the box minimally.
            content_top.x += std::abs((content_top.x + content_size.x) - box.pos.x);
        }

        // Compute the new scroll offset.
        float off_y = std::clamp(content_top.y, 0.f, data->content_size.y);
        float off_x = std::clamp(content_top.x, 0.f, data->content_size.x);
        populate_scroll_offset(data.get(), { off_x, off_y });
    }

    void ScrollBox::scroll_to_end_y()
    {
        float off_y = data->content_size.y;
        populate_scroll_offset(data.get(), { data->scroll_offset.x, off_y });
    }

    void ScrollBox::scroll_to_end_x()
    {
        float off_x = data->content_size.x;
        populate_scroll_offset(data.get(), { off_x, data->scroll_offset.y });
    }

    BuildScrollBoxResponse ScrollBox::build(CmdBuffer::DrawList* lst,
                                            UIState* state,
                                            float scroll_amount,
                                            BuildScrollBoxFlags flags)
    {
        BuildScrollBoxResponse resp{};
        // Process input.
        const auto clip = CmdBuffer::current_clip(*lst);
        {
            if (mouse_in_clip(state->mouse.ui_mouse, clip))
            {
                ID hot_widget = ID::Zero;
                auto adjusted_mouse = adjusted_mouse_for_clip(state->mouse.ui_mouse, clip);
                auto v_scroll = v_scrollbar_box(data.get(), convert(clip));
                auto h_scroll = h_scrollbar_box(data.get(), convert(clip));
                if (basic_aabb({ .pos = v_scroll.pos, .size = v_scroll.size }, adjusted_mouse))
                {
                    hot_widget = data->v_scroll_id;
                }
                else if (basic_aabb({ .pos = h_scroll.pos, .size = h_scroll.size }, adjusted_mouse))
                {
                    hot_widget = data->h_scroll_id;
                }

                try_set_hot_widget(state, hot_widget);
                if (down(*state, MouseButton::L))
                {
                    try_set_focus_widget(state, hot_widget);
                }

                if (empty_focus_widget(*state))
                {
                    if (state->hot_widget == hot_widget and hot_widget != ID::Zero)
                    {
                        change_cursor(state, CursorStyle::Default);
                    }
                    TranslatedScroll scroll = translate_mouse_wheel(*state);
                    // Note: Use the real scroll offset without the visual offset.
                    auto off = data->scroll_offset;
                    switch (scroll.vscr)
                    {
                    case VScrollResult::Up:
                        {
                            off.y = std::clamp(off.y - std::abs(scroll_amount * scroll.wheel_delta.y), 0.f, data->content_size.y);
                            populate_scroll_offset(data.get(), off);
                            resp.scroll_changed = true;
                        }
                        break;
                    case VScrollResult::Down:
                        {
                            off.y = std::clamp(off.y + std::abs(scroll_amount * scroll.wheel_delta.y), 0.f, data->content_size.y);
                            populate_scroll_offset(data.get(), off);
                            resp.scroll_changed = true;
                        }
                        break;
                    }

                    switch (scroll.hscr)
                    {
                    case HScrollResult::Left:
                        off.x = std::clamp(off.x - std::abs(scroll_amount * scroll.wheel_delta.x), 0.f, data->content_size.x);
                        populate_scroll_offset(data.get(), off);
                        resp.scroll_changed = true;
                        break;
                    case HScrollResult::Right:
                        off.x = std::clamp(off.x + std::abs(scroll_amount * scroll.wheel_delta.x), 0.f, data->content_size.x);
                        populate_scroll_offset(data.get(), off);
                        resp.scroll_changed = true;
                        break;
                    }
                }
            }

            // Drag scroll.
            if (state->focus_widget == data->v_scroll_id)
            {
                drag_v_scroll(data.get(), *state, convert(clip));
                resp.scroll_changed = true;
                resp.scroll_dragged = true;
            }
            else if (state->focus_widget == data->h_scroll_id)
            {
                drag_h_scroll(data.get(), *state, convert(clip));
                resp.scroll_changed = true;
                resp.scroll_dragged = true;
            }
        }
        const auto& colors = Config::widget_colors();

        CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
        // Border rect for viewport.
        if (implies(flags, BuildScrollBoxFlags::DrawBorder))
        {
            Vec2f left{ 0.f, 0.f };
            Vec2f size{ rep(clip.width) + 0.f, rep(clip.height) + 0.f };
            CmdBuffer::strike_rect(lst, Render::FragShader::BasicColor, left, size, 2.f, colors.scrollbar_track_outline);
        }

        // Scroll bar(s).
        {
            // Outline for track(s).

            // Vert.
            Vec2f left{ rep(clip.width) - data->scrollbar_width, 0.f };
            Vec2f size{ data->scrollbar_width, rep(clip.height) + 0.f };
            CmdBuffer::strike_rect(lst, Render::FragShader::BasicColor, left, size, 2.f, colors.scrollbar_track_outline);

            const bool need_horiz = horiz_scrollbar_needed(data.get(), convert(clip));

            if (need_horiz)
            {
                left.x = 0.f;
                left.y = 0.f;
                // So we don't overloap with the vert scrollbar.
                size.x = rep(clip.width) - data->scrollbar_width;
                size.y = data->scrollbar_width;
                CmdBuffer::strike_rect(lst, Render::FragShader::BasicColor, left, size, 2.f, colors.scrollbar_track_outline);
            }

            const Vec4f scrollbar_color[] =
            {
                colors.scrollbar_inactive, // Neutral.
                colors.scrollbar_active    // Hovered.
            };

            // Vert scrollbar rect.
            {
                auto [rect_pos, rect_size] = v_scrollbar_box(data.get(), convert(clip));
                const bool active = (empty_focus_widget(*state) and state->hot_widget == data->v_scroll_id) or state->focus_widget == data->v_scroll_id;
                CmdBuffer::solid_rect(lst, Render::FragShader::BasicColor, rect_pos, rect_size, scrollbar_color[active]);
            }

            // Horiz scrollbar rect.
            if (need_horiz)
            {
                auto [rect_pos, rect_size] = h_scrollbar_box(data.get(), convert(clip));
                const bool active = (empty_focus_widget(*state) and state->hot_widget == data->h_scroll_id) or state->focus_widget == data->h_scroll_id;
                CmdBuffer::solid_rect(lst, Render::FragShader::BasicColor, rect_pos, rect_size, scrollbar_color[active]);
            }
        }

        // Advance the movement offset.
        data->movement_offset = apply_anim(data->movement_offset, state->anim_fast_rate);

        // Snap at last pixel to avoid jumping text.
        data->movement_offset.x = data->movement_offset.x * (std::abs(data->movement_offset.x) > 1.0f);
        data->movement_offset.y = data->movement_offset.y * (std::abs(data->movement_offset.y) > 1.0f);

        if (data->movement_offset != 0.f)
        {
            Render::request_frames();
        }
        return resp;
    }

    // ---- IndexedScrollBox ----
    struct IndexedScrollBox::Data
    {
        static constexpr int padding = 2;

        ID v_scroll_id = ID::Zero;
        ID h_scroll_id = ID::Zero;
        IndexedScrollContentSize content_size{};
        IndexedScrollOffset scroll_offset{};
        Vec2f movement_offset{};
        float scrollbar_width = 10.f;
        int scrollbar_min_size = 20;
    };

    namespace
    {
        int64_t idx_scroll_max_idx(const IndexedScrollBox::Data& data)
        {
            return std::max(int64_t(0ll), data.content_size.v_size - 1);
        }

        IndexedScrollOffset add_offset_idx_scroll_v(const IndexedScrollBox::Data& data, IndexedScrollOffset scroll_off, float off_y)
        {
            float scroll_off_low = (scroll_off.offset.y + off_y);
            int64_t idx = static_cast<int64_t>(scroll_off_low / data.content_size.entry_size.y);
            float low_part = fmodf(scroll_off_low, data.content_size.entry_size.y);
            // Keep the low part positive.
            if (low_part < 0.f)
            {
                idx -= 1;
                low_part = data.content_size.entry_size.y + low_part;
            }
            scroll_off.idx += idx;
            scroll_off.offset.y = low_part;
            // Clamping.
            if (scroll_off.idx < 0)
            {
                scroll_off.idx = 0;
                scroll_off.offset.y = 0.f;
            }
            else if (scroll_off.idx >= idx_scroll_max_idx(data))
            {
                scroll_off.idx = idx_scroll_max_idx(data);
                scroll_off.offset.y = 0.f;
            }
            return scroll_off;
        }

        IndexedScrollOffset idx_scroll_offset(const IndexedScrollBox::Data& data)
        {
            IndexedScrollOffset off = add_offset_idx_scroll_v(data, data.scroll_offset, data.movement_offset.y);
            off.offset.x += data.movement_offset.x;
            return off;
        }

        IndexedScrollOffset idx_scroll_no_offset(const IndexedScrollBox::Data& data)
        {
            return data.scroll_offset;
        }

        void populate_idx_scroll_offset(IndexedScrollBox::Data* data, const IndexedScrollOffset& offset)
        {
            if (Config::system_effects().smooth_scroll)
            {
                int64_t delta_idx = data->scroll_offset.idx - offset.idx;
                float delta_y = delta_idx * data->content_size.entry_size.y + data->scroll_offset.offset.y - offset.offset.y;
                float delta_x = data->scroll_offset.offset.x - offset.offset.x;
                data->movement_offset = data->movement_offset + Vec2f{ delta_x, delta_y };
            }
            data->scroll_offset = offset;
        }

        void populate_idx_scroll_offset_no_anim(IndexedScrollBox::Data* data, const IndexedScrollOffset& offset)
        {
            data->scroll_offset = offset;
        }

        ScrollbarRect v_idx_scrollbar_box(IndexedScrollBox::Data* data, const Render::RenderViewport& viewport)
        {
            if (data->content_size.v_size <= 1)
                return {};
            const double pixels_per_view = rep(viewport.height) / data->content_size.entry_size.y;
            const double total_size = data->content_size.v_size - 1.;
            const double win_size = total_size + pixels_per_view;
            const float target_height = rep(viewport.height) * static_cast<float>(pixels_per_view / win_size);
            const float height = std::clamp(target_height, static_cast<float>(data->scrollbar_min_size), rep(viewport.height) + 0.f);
            const auto off = idx_scroll_offset(*data);
            const double idx_v = static_cast<double>(off.idx + off.offset.y / data->content_size.entry_size.y);
            const float scroll_off = static_cast<float>((idx_v / total_size) * (rep(viewport.height) - height));
            Vec2f left{ rep(viewport.width) - data->scrollbar_width, 0.f };
            Vec2f size{ data->scrollbar_width, 0.f };
            size.y = height;
            left.y = rep(viewport.height) - height - scroll_off;
            return { .pos = left, .size = size };
        }

        ScrollbarRect h_idx_scrollbar_box(IndexedScrollBox::Data* data, const Render::RenderViewport& viewport)
        {
            if (data->content_size.entry_size.x == 0.f)
                return {};
            // So we don't overlap with the vert scrollbar.
            const float vp_width = rep(viewport.width) - data->scrollbar_width;
            // Note: Due to the way content width is adjusted, we add an additional 'view_size_x' here to
            // ensure that we catch the 'end' of the viewable content.
            float width = vp_width * (vp_width / (data->content_size.entry_size.x + vp_width));
            width = std::max(width, static_cast<float>(data->scrollbar_min_size));
            const auto off = idx_scroll_offset(*data);
            const float offset_multiplier = off.offset.x / (data->content_size.entry_size.x);
            Vec2f left{};
            Vec2f size{ 0.f, data->scrollbar_width };
            size.x = width;
            left.x = offset_multiplier * (vp_width - width);
            return { .pos = left, .size = size };
        }

        void drag_v_idx_scroll(IndexedScrollBox::Data* data, const UIState& state, const Render::RenderViewport& viewport)
        {
            const double content_size = data->content_size.v_size * static_cast<double>(data->content_size.entry_size.y);
            double height = rep(viewport.height) * rep(viewport.height) / (content_size + rep(viewport.height));
            height = std::max(height, static_cast<double>(data->scrollbar_min_size));
            const double off = -static_cast<double>(mouse_move_drag(state).y);
            const double off_scaled = content_size * off / (rep(viewport.height) - height);
            auto added_off = add_offset_idx_scroll_v(*data, data->scroll_offset, static_cast<float>(off_scaled));
            populate_idx_scroll_offset_no_anim(data, added_off);
        }

        void drag_h_idx_scroll(IndexedScrollBox::Data* data, const UIState& state, const Render::RenderViewport& viewport)
        {
            auto initial_offset = idx_scroll_offset(*data);
            const float vp_width = rep(viewport.width) - data->scrollbar_width;
            float width = vp_width * (vp_width / (data->content_size.entry_size.x + vp_width));
            width = std::max(width, static_cast<float>(data->scrollbar_min_size));
            const float off = static_cast<float>(mouse_move_drag(state).x);
            const float off_scaled = data->content_size.entry_size.x * off / (vp_width - width);
            float off_x = std::clamp(initial_offset.offset.x + off_scaled, 0.f, data->content_size.entry_size.x);
            initial_offset.offset.x = off_x;
            populate_idx_scroll_offset_no_anim(data, initial_offset);
        }

        bool horiz_scrollbar_needed(IndexedScrollBox::Data* data, const Render::RenderViewport& viewport)
        {
            float view_size_x = rep(viewport.width) + 0.f;
            // Remove the vert scrollbar width.
            view_size_x -= data->scrollbar_width;
            // Note: Due to the way content width is adjusted, we add an additional 'view_size_x' here to
            // ensure that we catch the 'end' of the viewable content.
            return (data->content_size.entry_size.x + view_size_x) > view_size_x;
        }
    } // namespace [anon]

    IndexedScrollBox::IndexedScrollBox(ID content_id):
        data{ new Data{ .v_scroll_id = make_id_seed(content_id, "vscroll"),
                        .h_scroll_id = make_id_seed(content_id, "hscroll") } } { }

    IndexedScrollBox::~IndexedScrollBox() = default;

    // Setup.
    void IndexedScrollBox::content_size(const IndexedScrollContentSize& size)
    {
        data->content_size = size;
        // This clamps 'y' for us.
        IndexedScrollOffset off = add_offset_idx_scroll_v(*data, data->scroll_offset, 0.f);
        off.offset.x = std::clamp(data->scroll_offset.offset.x, 0.f, data->content_size.entry_size.x);
        populate_idx_scroll_offset_no_anim(data.get(), off);
    }

    void IndexedScrollBox::sync_config()
    {
        data->scrollbar_width = Config::widget_state().scrollbar_width + 0.f;
        data->scrollbar_min_size = Config::widget_state().scrollbar_min_size;
    }

    // Queries for enclosed content.
    IndexedScrollOffset IndexedScrollBox::position() const
    {
        return idx_scroll_offset(*data);
    }

    IndexedScrollOffset IndexedScrollBox::position_no_offset() const
    {
        return idx_scroll_no_offset(*data);
    }

    Render::RenderViewport IndexedScrollBox::content_viewport(const Render::RenderViewport& viewport) const
    {
        const int scrollbar_width = static_cast<int>(data->scrollbar_width);

        // reduce the margins a bit.
        auto new_viewport = viewport;
        pad_viewport(&new_viewport, { Data::padding });
        new_viewport.width = retract(new_viewport.width, scrollbar_width);
        // Make room for the horizontal scrollbar if necessary.
        if (horiz_scrollbar_needed(data.get(), viewport))
        {
            new_viewport.height = retract(new_viewport.height, scrollbar_width);
            new_viewport.offset_y = offset_from(new_viewport.offset_y, scrollbar_width);
        }
        return new_viewport;
    }

    const IndexedScrollContentSize& IndexedScrollBox::content_size() const
    {
        return data->content_size;
    }

    uint64_t IndexedScrollBox::indices_per_view(const Render::RenderViewport& viewport) const
    {
        auto content_vp = content_viewport(viewport);
        return static_cast<uint64_t>(rep(content_vp.height) / data->content_size.entry_size.y) - 1;
    }

    // Direct interaction.
    void IndexedScrollBox::scroll_to(IndexedScrollOffset offset)
    {
        populate_idx_scroll_offset(data.get(), offset);
    }

    void IndexedScrollBox::scroll_to_no_smooth_scroll(IndexedScrollOffset offset)
    {
        populate_idx_scroll_offset_no_anim(data.get(), offset);
    }

    void IndexedScrollBox::make_index_viewable(int64_t idx, Vec2f x_pos_size, const Render::RenderViewport& viewport)
    {
        auto content_vp = content_viewport(viewport);
        Vec2f content_size{ rep(content_vp.width) + 0.f, rep(content_vp.height) + 0.f };
        auto scroll_off = data->scroll_offset;
        int partial_idx_add = 0;
        if (scroll_off.offset.y < 0.f)
        {
            partial_idx_add = -1;
        }
        else if (scroll_off.offset.y > 0.f)
        {
            partial_idx_add = 1;
        }
        int64_t min_idx = scroll_off.idx + partial_idx_add;
        int64_t idx_per_view = static_cast<int64_t>(content_size.y / data->content_size.entry_size.y) - 1;
        int64_t max_idx = min_idx + idx_per_view;
        int64_t max_possible_idx = idx_scroll_max_idx(*data);
        max_idx = std::clamp(max_idx, int64_t(0ll), max_possible_idx);

        // Hit tests.
        if (idx < min_idx)
        {
            // Shift top of window up.
            scroll_off.idx = idx;
            scroll_off.offset.y = 0.f;
        }

        if (max_idx < idx)
        {
            // Shift bottom of window down.
            scroll_off.idx = std::max(int64_t(0ll), idx - idx_per_view);
            scroll_off.offset.y = 0.f;
        }

        if (x_pos_size.x < scroll_off.offset.x)
        {
            // Shift window left.
            scroll_off.offset.x = x_pos_size.x;
        }

        if (x_pos_size.x + x_pos_size.y > scroll_off.offset.x + content_size.x)
        {
            // Shift right of window to accommodate the box minimally.
            scroll_off.offset.x += std::abs((scroll_off.offset.x + content_size.x) - (x_pos_size.x + x_pos_size.y));
        }

        // Compute scroll offset.
        scroll_off.offset.x = std::clamp(scroll_off.offset.x, 0.f, data->content_size.entry_size.x);
        populate_idx_scroll_offset(data.get(), scroll_off);
    }

    BuildScrollBoxResponse IndexedScrollBox::build(CmdBuffer::DrawList* lst,
                                            UIState* state,
                                            float scroll_amount,
                                            BuildScrollBoxFlags flags)
    {
        BuildScrollBoxResponse resp{};
        // Process input.
        const auto clip = CmdBuffer::current_clip(*lst);
        {
            if (mouse_in_clip(state->mouse.ui_mouse, clip))
            {
                ID hot_widget = ID::Zero;
                auto adjusted_mouse = adjusted_mouse_for_clip(state->mouse.ui_mouse, clip);
                auto v_scroll = v_idx_scrollbar_box(data.get(), convert(clip));
                auto h_scroll = h_idx_scrollbar_box(data.get(), convert(clip));
                if (basic_aabb({ .pos = v_scroll.pos, .size = v_scroll.size }, adjusted_mouse))
                {
                    hot_widget = data->v_scroll_id;
                }
                else if (basic_aabb({ .pos = h_scroll.pos, .size = h_scroll.size }, adjusted_mouse))
                {
                    hot_widget = data->h_scroll_id;
                }

                try_set_hot_widget(state, hot_widget);
                if (down(*state, MouseButton::L))
                {
                    try_set_focus_widget(state, hot_widget);
                }

                if (empty_focus_widget(*state))
                {
                    if (state->hot_widget == hot_widget and hot_widget != ID::Zero)
                    {
                        change_cursor(state, CursorStyle::Default);
                    }
                    TranslatedScroll scroll = translate_mouse_wheel(*state);
                    // Note: Use the real scroll offset without the visual offset.
                    auto off = data->scroll_offset;
                    switch (scroll.vscr)
                    {
                    case VScrollResult::Up:
                        {
                            off = add_offset_idx_scroll_v(*data, off, -std::abs(scroll_amount * scroll.wheel_delta.y));
                            populate_idx_scroll_offset(data.get(), off);
                            resp.scroll_changed = true;
                        }
                        break;
                    case VScrollResult::Down:
                        {
                            off = add_offset_idx_scroll_v(*data, off, std::abs(scroll_amount * scroll.wheel_delta.y));
                            populate_idx_scroll_offset(data.get(), off);
                            resp.scroll_changed = true;
                        }
                        break;
                    }

                    switch (scroll.hscr)
                    {
                    case HScrollResult::Left:
                        off.offset.x = std::clamp(off.offset.x - std::abs(scroll_amount * scroll.wheel_delta.x), 0.f, data->content_size.entry_size.x);
                        populate_idx_scroll_offset(data.get(), off);
                        resp.scroll_changed = true;
                        break;
                    case HScrollResult::Right:
                        off.offset.x = std::clamp(off.offset.x + std::abs(scroll_amount * scroll.wheel_delta.x), 0.f, data->content_size.entry_size.x);
                        populate_idx_scroll_offset(data.get(), off);
                        resp.scroll_changed = true;
                        break;
                    }
                }
            }

            // Drag scroll.
            if (state->focus_widget == data->v_scroll_id)
            {
                drag_v_idx_scroll(data.get(), *state, convert(clip));
                resp.scroll_changed = true;
                resp.scroll_dragged = true;
            }
            else if (state->focus_widget == data->h_scroll_id)
            {
                drag_h_idx_scroll(data.get(), *state, convert(clip));
                resp.scroll_changed = true;
                resp.scroll_dragged = true;
            }
        }
        const auto& colors = Config::widget_colors();

        CmdBuffer::start_shapes(lst, Render::VertShader::OneOneTransform);
        // Border rect for viewport.
        if (implies(flags, BuildScrollBoxFlags::DrawBorder))
        {
            Vec2f left{ 0.f, 0.f };
            Vec2f size{ rep(clip.width) + 0.f, rep(clip.height) + 0.f };
            CmdBuffer::strike_rect(lst, Render::FragShader::BasicColor, left, size, 2.f, colors.scrollbar_track_outline);
        }

        // Scroll bar(s).
        {
            // Outline for track(s).

            // Vert.
            Vec2f left{ rep(clip.width) - data->scrollbar_width, 0.f };
            Vec2f size{ data->scrollbar_width, rep(clip.height) + 0.f };
            CmdBuffer::strike_rect(lst, Render::FragShader::BasicColor, left, size, 2.f, colors.scrollbar_track_outline);

            const bool need_horiz = horiz_scrollbar_needed(data.get(), convert(clip));

            if (need_horiz)
            {
                left.x = 0.f;
                left.y = 0.f;
                // So we don't overloap with the vert scrollbar.
                size.x = rep(clip.width) - data->scrollbar_width;
                size.y = data->scrollbar_width;
                CmdBuffer::strike_rect(lst, Render::FragShader::BasicColor, left, size, 2.f, colors.scrollbar_track_outline);
            }

            const Vec4f scrollbar_color[] =
            {
                colors.scrollbar_inactive, // Neutral.
                colors.scrollbar_active    // Hovered.
            };

            // Vert scrollbar rect.
            {
                auto [rect_pos, rect_size] = v_idx_scrollbar_box(data.get(), convert(clip));
                const bool active = (empty_focus_widget(*state) and state->hot_widget == data->v_scroll_id) or state->focus_widget == data->v_scroll_id;
                CmdBuffer::solid_rect(lst, Render::FragShader::BasicColor, rect_pos, rect_size, scrollbar_color[active]);
            }

            // Horiz scrollbar rect.
            if (need_horiz)
            {
                auto [rect_pos, rect_size] = h_idx_scrollbar_box(data.get(), convert(clip));
                const bool active = (empty_focus_widget(*state) and state->hot_widget == data->h_scroll_id) or state->focus_widget == data->h_scroll_id;
                CmdBuffer::solid_rect(lst, Render::FragShader::BasicColor, rect_pos, rect_size, scrollbar_color[active]);
            }
        }

        // Advance the movement offset.
        data->movement_offset = apply_anim(data->movement_offset, state->anim_fast_rate);

        // Snap at last pixel to avoid jumping text.
        data->movement_offset.x = data->movement_offset.x * (std::abs(data->movement_offset.x) > 1.0f);
        data->movement_offset.y = data->movement_offset.y * (std::abs(data->movement_offset.y) > 1.0f);

        if (data->movement_offset != 0.f)
        {
            Render::request_frames();
        }
        return resp;
    }
} // namespace UI::Widgets
