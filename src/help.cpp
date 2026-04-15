#include "help.h"

#include <algorithm>
#include <vector>

#include "basic-box.h"
#include "basic-button.h"
#include "basic-checkbox.h"
#include "basic-scrollbox.h"
#include "basic-textbox.h"
#include "basic-window.h"
#include "config.h"
#include "enum-utils.h"
#include "gap-core.h"
#include "fuzzy-search.h"
#include "hotkeys.h"
#include "tooltips.h"

namespace Help
{
    namespace
    {
        enum class Mode
        {
            Hotkeys,
            About
        };

        using Description = FuzzySearch::FuzzyName;

        struct HotkeyEntry
        {
            Description name;
            String8 assignment;
            UI::Widgets::ID id;
            Hotkeys::HotkeyEntry hk_entry;
            Hotkeys::HotkeyMatch conflict;
        };

        using HotkeyEntries = std::vector<HotkeyEntry>;

        struct CustomHotkeyEntry
        {
            Description name;
            String8 desc;
            String8 assignment;
            UI::Widgets::ID id;
            Hotkeys::CustomHotkeyEntry hk_entry;
            Hotkeys::HotkeyMatch conflict;
        };

        using CustomHotkeyEntries = std::vector<CustomHotkeyEntry>;

        struct HotkeyComponent
        {
            std::string_view component;
            HotkeyEntries hotkeys;
            CustomHotkeyEntries custom_hotkeys;
            Hotkeys::CustomHotkeyGroup group;
            UI::Widgets::ID add_custom_id;
        };

        using HotkeyComponents = std::vector<HotkeyComponent>;

        using RenderHotkeyEntries = FuzzySearch::FuzzySearchResults<HotkeyEntry>;
        using RenderCustomHotkeyEntries = FuzzySearch::FuzzySearchResults<CustomHotkeyEntry>;

        struct RenderComponent
        {
            std::string_view component;
            RenderHotkeyEntries hotkeys;
            RenderCustomHotkeyEntries custom_hotkeys;
            Hotkeys::CustomHotkeyGroup group;
            UI::Widgets::ID add_custom_id;
        };

        using RenderComponents = std::vector<RenderComponent>;

        void populate_custom_hotkeys(Arena::Arena* arena, HotkeyComponent* c, Hotkeys::CustomHotkeyGroup group)
        {
            auto scratch = Arena::scratch_begin({ &arena, 1 });
            c->group = group;
            Hotkeys::CustomHotkeyList lst = Hotkeys::custom_hotkeys_for_group(group);
            for EachNode(n, lst.first)
            {
                Hotkeys::CustomHotkeyInfo info = Hotkeys::custom_hotkey_info(n->id);
                c->custom_hotkeys.emplace_back(
                    FuzzySearch::make_fuzzy_name(sv_str8(info.name)),
                    str8_empty,
                    Hotkeys::hotkey_as_string(arena, n->key, n->mods),
                    UI::Widgets::make_id_seed(
                        UI::Widgets::make_id_seed(
                            UI::Widgets::ID::Help,
                            c->component),
                        sv_str8(info.fn_name)),
                    *n);
                if (Hotkeys::is_custom_hotkey_bound(n->id))
                {
                    c->custom_hotkeys.back().desc = str8_fmt(arena, "%S | bound function \"%S\"", info.desc, info.fn_name);
                }
                else
                {
                    c->custom_hotkeys.back().desc = str8_fmt(arena,
                        "Function binding \"%S\" has no bound function for group \"%S\".  Check plugin source.",
                        info.fn_name,
                        Hotkeys::hotkey_group_as_string(group));
                }

                // Get conflicts for this hotkey.
                bool try_matches = n->key != OS::Key::Null;
                Hotkeys::HotkeyMatchList matches = {};
                if (try_matches)
                {
                    matches = Hotkeys::bindings_for(scratch.arena, { .key = n->key, .mods = n->mods });
                }

                if (matches.count > 1)
                {
                    for EachNode(m, matches.first)
                    {
                        // Detect inner-conflicts.
                        if (m->group == group
                            and not Hotkeys::is_match(m->match, n->id))
                        {
                            c->custom_hotkeys.back().conflict = m->match;
                            // No need to find more.
                            break;
                        }

                        // Intersect with global conflicts.
                        if (m->group == Hotkeys::CustomHotkeyGroup::GLB
                            and not Hotkeys::is_match(m->match, n->id))
                        {
                            c->custom_hotkeys.back().conflict = m->match;
                            break;
                        }
                    }
                }
            }

            // Make the "add custom" ID.
            c->add_custom_id = UI::Widgets::make_id_seed(
                                UI::Widgets::make_id_seed(UI::Widgets::ID::Help, c->component),
                                "add-cust-btn");
            Arena::scratch_end(scratch);
        }

        void populate_hotkey_conflict(HotkeyEntry* entry, Hotkeys::CustomHotkeyGroup group)
        {
            auto scratch = Arena::scratch_begin(Arena::no_conflicts);
            bool try_match = entry->hk_entry.key != OS::Key::Null;
            Hotkeys::HotkeyMatchList matches = {};
            if (try_match)
            {
                matches = Hotkeys::bindings_for(scratch.arena,
                { .key = entry->hk_entry.key,
                    .mods = entry->hk_entry.mods });
            }

            if (matches.count > 1)
            {
                for EachNode(n, matches.first)
                {
                    // Detect inner-conflicts.
                    if (n->group == group
                        and not Hotkeys::is_match(n->match, entry->hk_entry.cmd))
                    {
                        entry->conflict = n->match;
                        // No need to find more.
                        break;
                    }

                    // Intersect with global conflicts.
                    if (n->group == Hotkeys::CustomHotkeyGroup::GLB
                        and not Hotkeys::is_match(n->match, entry->hk_entry.cmd))
                    {
                        entry->conflict = n->match;
                        break;
                    }
                }
            }
            Arena::scratch_end(scratch);
        }

#define DAT_CAT_START(e, grp, cat, desc) void fill_ ## cat ## _collection(Arena::Arena* arena, HotkeyComponent* c) { \
 c->component = desc; using T = Hotkeys::Hotkey; constexpr auto group = Hotkeys::CustomHotkeyGroup:: grp;            \
 populate_custom_hotkeys(arena, c, group);
#define DAT_CMD(e, k, m, desc)           \
 c->hotkeys.emplace_back(                \
  FuzzySearch::make_fuzzy_name(desc),    \
  str8_empty,                            \
  UI::Widgets::make_id_seed(             \
   UI::Widgets::make_id_seed(            \
    UI::Widgets::ID::Help,               \
    c->component),                       \
   #e),                                  \
   Hotkeys::entry_for(T::e));            \
  { c->hotkeys.back().assignment = Hotkeys::hotkey_as_string(arena, T:: e); \
    populate_hotkey_conflict(&c->hotkeys.back(), group);                    \
  }
#define DAT_CAT_END(e, grp) }
#include "hotkeys.dat"
#undef DAT_CAT_START
#undef DAT_CMD
#undef DAT_CAT_END

        struct UIData
        {
            UI::Widgets::ID hotkeys_id = UI::Widgets::ID::Zero;
            UI::Widgets::ID about_id = UI::Widgets::ID::Zero;
            UI::Widgets::ID show_conflicts_id = UI::Widgets::ID::Zero;
            UI::Widgets::ID reload_hotkeys_id = UI::Widgets::ID::Zero;
            int64_t sel_index = -1;
            Mode mode = Mode::Hotkeys;
            float wheel_offset_amount = 0.f;
            bool show_conflicts_only = false;
        };
    } // namespace [anon]

    struct Help::Data
    {
        UI::Widgets::BasicWindow window;
        UI::Widgets::ScrollBox scrollbox;
        Glyph::Atlas* atlas;
        Arena::Arena* components_arena;
        Glyph::FontSize font_size = Glyph::FontSize{ 18 };
        HotkeyComponents components;
        RenderComponents render_components;
        UIData ui_data;

        static constexpr float padding = 2.f;
    };

    namespace
    {
        void init_collections(Help::Data* data)
        {
            Arena::clear(data->components_arena);
            data->components.clear();
#define DAT_CAT_START(e, grp, cat, desc) \
 data->components.push_back({});    \
 fill_ ## cat ## _collection(data->components_arena, &data->components.back());
#define DAT_CMD(e, k, m, desc)
#define DAT_CAT_END(e, grp)
#include "hotkeys.dat"
#undef DAT_CAT_START
#undef DAT_CMD
#undef DAT_CAT_END
        }

        void copy_as_render_entries(HotkeyComponents* components, RenderComponents* render_componenets)
        {
            render_componenets->clear();
            render_componenets->reserve(components->size());
            for (auto& c : *components)
            {
                render_componenets->push_back({});
                auto& new_c = render_componenets->back();
                new_c.component = c.component;
                new_c.group = c.group;
                // Hotkeys.
                new_c.hotkeys.reserve(c.hotkeys.size());
                FuzzySearch::reset_states(&c.hotkeys);
                for (auto& hk : c.hotkeys)
                {
                    new_c.hotkeys.push_back({ .entry = &hk, .score = 0 });
                }

                // Custom hotkeys.
                new_c.custom_hotkeys.reserve(c.custom_hotkeys.size());
                FuzzySearch::reset_states(&c.custom_hotkeys);
                for (auto& chk : c.custom_hotkeys)
                {
                    new_c.custom_hotkeys.push_back({ .entry = &chk, .score = 0 });
                }
                new_c.add_custom_id = c.add_custom_id;
            }
        }

        void setup_ui_data(Help::Data* data)
        {
            data->ui_data = {};
            data->ui_data.hotkeys_id = UI::Widgets::make_id_seed(UI::Widgets::ID::Help, "hotkeys");
            data->ui_data.about_id = UI::Widgets::make_id_seed(UI::Widgets::ID::Help, "about");
            data->ui_data.show_conflicts_id = UI::Widgets::make_id_seed(UI::Widgets::ID::Help, "conflicts");
            data->ui_data.reload_hotkeys_id = UI::Widgets::make_id_seed(UI::Widgets::ID::Help, "reload-hotkeys");
            data->ui_data.mode = Mode::Hotkeys;
            data->ui_data.wheel_offset_amount = UI::standard_font_padding(data->font_size);
            data->window.background_alpha(0.8f);
            data->scrollbox.scroll_to(0.f);
            copy_as_render_entries(&data->components, &data->render_components);
        }

        Render::RenderViewport initial_window_viewport(const ScreenDimensions& screen)
        {
            auto viewport = Render::RenderViewport::basic(screen);
            viewport.width = Width{ rep(viewport.width) / 2 };
            viewport.height = Height{ rep(viewport.height) / 2 };
            viewport.offset_x = Render::ViewportOffsetX{ (rep(screen.width) - rep(viewport.width)) / 2 };
            viewport.offset_y = Render::ViewportOffsetY{ rep(viewport.height) / 2 };
            return viewport;
        }

        float title_base_height(Glyph::RenderFontContext* font_ctx)
        {
            return font_ctx->current_font_size() + Help::Data::padding * 2 + 0.f;
        }

        float hotkey_base_height(Glyph::RenderFontContext* font_ctx)
        {
            return font_ctx->current_font_size() * 2 + Help::Data::padding * 2;
        }

        Vec2f content_size(Help::Data* data)
        {
            auto font_ctx = data->atlas->render_font_context(data->font_size);
            // Keep total_size 'x' as 0 so we don't spawn a horizontal scroll.
            Vec2f total_size{};
            float title_height = title_base_height(&font_ctx);
            float hotkey_height = hotkey_base_height(&font_ctx);
            for (auto& comp : data->render_components)
            {
                total_size.y += title_height + Help::Data::padding;
                total_size.y += static_cast<int>(comp.hotkeys.size()) * hotkey_height;
                total_size.y += static_cast<int>(comp.custom_hotkeys.size()) * hotkey_height;
            }
            return total_size;
        }

        bool box_for(Help::Data* data, UI::AABBData* box, Glyph::RenderFontContext* font_ctx, int64_t idx)
        {
            // Keep total_size 'x' as 0 so we don't spawn a horizontal scroll.
            Vec2f total_size{};
            float title_height = title_base_height(font_ctx);
            float hotkey_height = hotkey_base_height(font_ctx);
            // We're going to switch this to "elements to skip".
            idx += 1;
            for (auto& comp : data->render_components)
            {
                total_size.y += title_height + Help::Data::padding;
                if (static_cast<int64_t>(comp.hotkeys.size()) >= idx)
                {
                    total_size.y += idx * hotkey_height;
                    box->pos.x = 0;
                    // Remove a hotkey height since we want the 'top' of the box.
                    box->pos.y = total_size.y - hotkey_height;
                    box->size = hotkey_height;
                    return true;
                }
                idx -= static_cast<int64_t>(comp.hotkeys.size());
                total_size.y += static_cast<int>(comp.hotkeys.size()) * hotkey_height;
                // Try the custom hotkey list.
                if (static_cast<int64_t>(comp.custom_hotkeys.size()) >= idx)
                {
                    total_size.y += idx * hotkey_height;
                    box->pos.x = 0;
                    // Remove a hotkey height since we want the 'top' of the box.
                    box->pos.y = total_size.y - hotkey_height;
                    box->size = hotkey_height;
                    return true;
                }
                idx -= static_cast<int64_t>(comp.custom_hotkeys.size());
                total_size.y += static_cast<int>(comp.custom_hotkeys.size()) * hotkey_height;
            }
            return false;
        }

        void apply_search_filter(HotkeyComponents* components, RenderComponents* render_components, std::string_view filter)
        {
            render_components->clear();
            render_components->reserve(components->size());
            for (auto& c : *components)
            {
                render_components->push_back({});
                auto& new_c = render_components->back();
                new_c.component = c.component;
                new_c.group = c.group;
                FuzzySearch::populate_fuzzy_search_results(&c.hotkeys, &new_c.hotkeys, filter);
                FuzzySearch::populate_fuzzy_search_results(&c.custom_hotkeys, &new_c.custom_hotkeys, filter);

                // Component is empty.
                if (new_c.hotkeys.empty() and new_c.custom_hotkeys.empty())
                {
                    render_components->pop_back();
                }
                new_c.add_custom_id = c.add_custom_id;
            }
        }

        void filter_by_conflicts(RenderComponents* render_components)
        {
            for (auto& c : *render_components)
            {
                auto last = std::remove_if(begin(c.hotkeys),
                                        end(c.hotkeys),
                                        [](auto e)
                                        {
                                            return not Hotkeys::valid_match(e.entry->conflict);
                                        });
                c.hotkeys.erase(last, end(c.hotkeys));
                // Custom hotkeys.
                auto last_c = std::remove_if(begin(c.custom_hotkeys),
                                        end(c.custom_hotkeys),
                                        [](auto e)
                                        {
                                            return not Hotkeys::valid_match(e.entry->conflict);
                                        });
                c.custom_hotkeys.erase(last_c, end(c.custom_hotkeys));
            }

            auto last = std::remove_if(begin(*render_components),
                                        end(*render_components),
                                        [](const RenderComponent& c)
                                        {
                                            return c.hotkeys.empty() and c.custom_hotkeys.empty();
                                        });
            render_components->erase(last, end(*render_components));
        }

        struct WindowContentViewports
        {
            Render::RenderViewport scroll_vp;
        };

        int buttons_height(Help::Data* data, const Render::RenderViewport& viewport)
        {
            int buttons_vp_height = std::min(static_cast<int>(UI::standard_font_padding(data->font_size) + Help::Data::padding * 3), rep(viewport.height));
            return buttons_vp_height;
        }

        WindowContentViewports window_content_viewports(Help::Data* data, const Render::RenderViewport& viewport)
        {
            // Mode buttons first.
            auto buttons_vp_height = buttons_height(data, viewport);

            auto scroll_vp = viewport;
            auto scroll_height = std::max(0, std::min(rep(viewport.height) - static_cast<int>(Help::Data::padding) - buttons_vp_height, rep(viewport.height)));
            scroll_vp.height = Height{ scroll_height };

            return { .scroll_vp = scroll_vp };
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

        void apply_filters(Help::Data* data)
        {
            auto scratch = Arena::scratch_begin(Arena::no_conflicts);
            String8 filter = str8_empty;
            if (filter.size == 0)
            {
                // Clear selection until the user does something.
                data->ui_data.sel_index = -1;
                copy_as_render_entries(&data->components, &data->render_components);
            }
            else
            {
                // If a search filter is applied, the selected index should start with the first element.
                data->ui_data.sel_index = 0;
                apply_search_filter(&data->components, &data->render_components, sv_str8(filter));
            }

            if (data->ui_data.show_conflicts_only)
            {
                filter_by_conflicts(&data->render_components);
            }
            Arena::scratch_end(scratch);
        }

        struct BuildHotkeysResponse
        {
            Hotkeys::Hotkey reset_hk = Hotkeys::Hotkey::None;
            Hotkeys::Hotkey next_frame_hk = Hotkeys::Hotkey::None;
            Hotkeys::Hotkey clear_hk = Hotkeys::Hotkey::None;
            Hotkeys::CustomHotkeyID next_frame_chk = Hotkeys::CustomHotkeyID::None;
            Hotkeys::CustomHotkeyID builder_id = Hotkeys::CustomHotkeyID::None;
            Hotkeys::CustomHotkeyID remove_id = Hotkeys::CustomHotkeyID::None;
            Hotkeys::CustomHotkeyID delete_target_id = Hotkeys::CustomHotkeyID::None;
            Hotkeys::CustomHotkeyID clear_id = Hotkeys::CustomHotkeyID::None;
            Hotkeys::CustomHotkeyGroup group = {};
        };

        BuildHotkeysResponse build_hotkeys(Help::Data* data,
                            CmdBuffer::DrawList* lst,
                            UI::UIState* state)
        {
            BuildHotkeysResponse resp{};
            const auto& colors = Config::widget_colors();
            auto [scroll_vp] = window_content_viewports(data, data->window.content_viewport(data->window.window_viewport()));
            data->scrollbox.content_size(content_size(data), scroll_vp);
            auto content_vp = data->scrollbox.content_viewport(scroll_vp);
            CmdBuffer::push_clip(lst, UI::convert(scroll_vp));
            {
                auto scroll_resp = data->scrollbox.build(lst,
                                                            state,
                                                            data->ui_data.wheel_offset_amount,
                                                            UI::Widgets::BuildScrollBoxFlags::None);
                // If the scroll changed set the focus so no other widgets try to process the event.
                if (scroll_resp.scroll_changed)
                {
                    UI::try_set_focus_widget(state, UI::Widgets::ID::Help);
                }
            }
            CmdBuffer::push_clip(lst, UI::convert(content_vp));

            // Process input.
            auto font_ctx = data->atlas->render_font_context(data->font_size);
            {
                if (UI::empty_focus_widget(*state))
                {
                    if (UI::down(*state, OS::Key::Up))
                    {
                        data->ui_data.sel_index = std::max(int64_t(0), data->ui_data.sel_index - 1);
                        UI::AABBData box;
                        if (box_for(data, &box, &font_ctx, data->ui_data.sel_index))
                        {
                            data->scrollbox.make_box_viewable(box, content_vp);
                        }
                    }

                    if (UI::down(*state, OS::Key::Down))
                    {
                        int64_t count_values = 0;
                        for (auto& entry : data->render_components)
                        {
                            count_values += static_cast<int64_t>(entry.hotkeys.size());
                            count_values += static_cast<int64_t>(entry.custom_hotkeys.size());
                        }
                        // Even if this ends up being -1, this is fine because there's nothing to render anyway.
                        data->ui_data.sel_index = std::min(count_values - 1, data->ui_data.sel_index + 1);
                        UI::AABBData box;
                        if (box_for(data, &box, &font_ctx, data->ui_data.sel_index))
                        {
                            data->scrollbox.make_box_viewable(box, content_vp);
                        }
                    }
                }
            }
            Vec2f pos{};
            pos.y = 0.f + rep(content_vp.height);
            pos.y += data->scrollbox.position().y;

            RenderDescriptionData desc_data =
            {
                .pos = pos,
                .unmatched_color = colors.window_title_font_color,
                .match_color = hex_to_vec4f(0xffffffff),
                .desc = nullptr,
            };

            UI::Widgets::BuildBoxInput box_in{
                .thickness = Help::Data::padding
            };

            UI::Widgets::BuildButtonInput btn_in{
                .padding = { Help::Data::padding, 0.f },
                .thickness = Help::Data::padding
            };
            const float title_h = title_base_height(&font_ctx);
            const float entry_height = hotkey_base_height(&font_ctx);
            int64_t idx = 0;
            for (auto& cmp : data->render_components)
            {
                pos.y -= title_h;

                // Title.
                if (pos.y <= rep(content_vp.height))
                {
                    auto text_pos = pos;
                    // Center the component title.
                    float text_w = font_ctx.measure_text(cmp.component).x;
                    text_pos.x += (rep(content_vp.width) - text_w) / 2.f;
                    text_pos.y += (title_h - rep(data->font_size)) / 2.f;
                    CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
                    text_pos.x = font_ctx.render_text(lst, cmp.component, text_pos, colors.window_title_font_color).x;

                    // Add the custom hotkey button to end.
                    UI::Widgets::BuildIconicButtonInput btn_ico_in = {
                        .id = cmp.add_custom_id,
                        .icon = Glyph::SpecialGlyph::Plus,
                        .pos = pos,
                        .padding = { Help::Data::padding, Help::Data::padding },
                        .thickness = Help::Data::padding
                    };
                    btn_ico_in.pos.x += text_pos.x + Help::Data::padding;
                    auto btn_size = UI::Widgets::measure_iconic_button(&font_ctx, btn_ico_in);
                    btn_ico_in.pos.y += (title_h - btn_size.y) / 2.f;
                    auto btn_resp = UI::Widgets::basic_iconic_button(lst, state, &font_ctx, btn_ico_in, UI::Widgets::BuildButtonFlags::None);
                    if (btn_resp.clicked)
                    {
                        resp.builder_id = Hotkeys::make_custom_hotkey(cmp.group, {});
                        resp.group = cmp.group;
                    }

                    // Create tooltip.
                    if (not state->tooltip.enabled and UI::hot_widget_set(*state, btn_ico_in.id))
                    {
                        auto temp = Arena::temp_begin(state->frame_arena);
                        String8 tip = str8_fmt(temp.arena, "Add custom binding to '%S'", Hotkeys::hotkey_group_as_string(cmp.group));
                        UI::Widgets::TextTooltipInput tip_in{
                            .text = sv_str8(tip),
                            .padding = Help::Data::padding,
                            .screen_pos = state->mouse.ui_mouse
                        };
                        UI::Widgets::basic_text_tooltip(state, lst, &font_ctx, tip_in);
                        Arena::temp_end(temp);
                    }
                }

                pos.y -= Help::Data::padding;

                // Hotkeys.
                for (auto& hk : cmp.hotkeys)
                {
                    const bool currently_selected = idx == data->ui_data.sel_index;
                    pos.y -= entry_height;
                    if (pos.y <= rep(content_vp.height))
                    {
                        using Flg = UI::Widgets::BuildBoxFlags;
                        // Hotkey button.
                        {
                            btn_in.id = UI::Widgets::make_id_seed(hk.entry->id, "hotkey");
                            btn_in.pos = pos;
                            if (state->rebinding_hotkey and state->hotkeys.rebind_widget == btn_in.id)
                            {
                                // Make a special button for the rebind target.
                                UI::Widgets::BuildIconicButtonInput btn_ico_in = {
                                    .id = btn_in.id,
                                    .icon = Glyph::SpecialGlyph::X,
                                    .padding = { Help::Data::padding, Help::Data::padding },
                                    .thickness = Help::Data::padding
                                };
                                Vec2f size = UI::Widgets::measure_iconic_button(&font_ctx, btn_ico_in);
                                btn_ico_in.pos = btn_in.pos;
                                btn_ico_in.pos.x += rep(content_vp.width) - (Help::Data::padding + size.x);
                                btn_ico_in.pos.y += (entry_height - size.y) / 2.f;
                                auto palette = *CmdBuffer::current_palette(*lst);
                                palette.fill = colors.window_close_button_hover;
                                CmdBuffer::push_color_palette(lst, palette);
                                auto btn_resp = UI::Widgets::basic_iconic_button(lst, state, &font_ctx, btn_ico_in, UI::Widgets::BuildButtonFlags::Strike);
                                CmdBuffer::pop_color_palette(lst);
                                // Create tooltip.
                                if (not state->tooltip.enabled and UI::hot_widget_set(*state, btn_ico_in.id))
                                {
                                    std::string_view txt = "Clear binding";
                                    UI::Widgets::TextTooltipInput tip_in{
                                        .text = txt,
                                        .padding = Help::Data::padding,
                                        .screen_pos = state->mouse.ui_mouse
                                    };
                                    UI::Widgets::basic_text_tooltip(state, lst, &font_ctx, tip_in);
                                }

                                if (btn_resp.clicked)
                                {
                                    resp.clear_hk = hk.entry->hk_entry.cmd;
                                }
                            }
                            else
                            {
                                btn_in.label = sv_str8(hk.entry->assignment);
                                Vec2f size = UI::Widgets::measure_button(&font_ctx, btn_in);
                                btn_in.pos.x += rep(content_vp.width) - (Help::Data::padding + size.x);
                                btn_in.pos.y += (entry_height - size.y) / 2.f;
                                auto btn_resp = UI::Widgets::basic_button(lst, state, &font_ctx, btn_in, UI::Widgets::BuildButtonFlags::Strike);
                                if (btn_resp.clicked)
                                {
                                    state->hotkeys.rebind_target = hk.entry->hk_entry.cmd;
                                    state->hotkeys.rebind_widget = btn_in.id;
                                    state->rebinding_hotkey = true;
                                }
                                // If this isn't the default, offer a button to reset it.
                                if (not Hotkeys::is_default_binding(hk.entry->hk_entry.cmd))
                                {
                                    // Move the button box.
                                    UI::Widgets::BuildIconicButtonInput reset_btn = {
                                        .id = UI::Widgets::make_id_seed(hk.entry->id, "reset"),
                                        .icon = Glyph::SpecialGlyph::Reset,
                                        .padding = { Help::Data::padding },
                                        .thickness = Help::Data::padding
                                    };
                                    Vec2f reset_size = UI::Widgets::measure_iconic_button(&font_ctx, reset_btn);
                                    btn_in.pos.x -= reset_size.x + Help::Data::padding;
                                    btn_in.pos.y = pos.y + (entry_height - reset_size.y) / 2.f;

                                    reset_btn.pos = btn_in.pos;
                                    btn_resp = UI::Widgets::basic_iconic_button(lst, state, &font_ctx, reset_btn, UI::Widgets::BuildButtonFlags::Strike);
                                    // Create tooltip.
                                    if (not state->tooltip.enabled and UI::hot_widget_set(*state, reset_btn.id))
                                    {
                                        std::string_view txt = "Reset to default";
                                        UI::Widgets::TextTooltipInput tip_in{
                                            .text = txt,
                                            .padding = Help::Data::padding,
                                            .screen_pos = state->mouse.ui_mouse
                                        };
                                        UI::Widgets::basic_text_tooltip(state, lst, &font_ctx, tip_in);
                                    }

                                    if (btn_resp.clicked)
                                    {
                                        resp.reset_hk = hk.entry->hk_entry.cmd;
                                    }
                                }
                            }
                        }
                        // Transparent box for capturing hover/selection.
                        {
                            // Conflict notification, relative to button above.
                            Hotkeys::HotkeyMatch conflict = hk.entry->conflict;
                            if (Hotkeys::valid_match(conflict))
                            {
                                Vec2f warn_pos = btn_in.pos;
                                auto warn_size = font_ctx.icon_glyph_size(Glyph::SpecialGlyph::Warning);
                                warn_pos.x -= warn_size.x + Help::Data::padding;
                                warn_pos.y = pos.y + (entry_height + warn_size.y) / 5.f;

                                box_in.pos = warn_pos;
                                box_in.size = warn_size;
                                box_in.id = UI::Widgets::make_id_seed(hk.entry->id, "conflict");
                                UI::Widgets::basic_box(lst, state, box_in, Flg::Clickable);

                                CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
                                font_ctx.render_icon_glyph(lst, Glyph::SpecialGlyph::Warning, warn_pos, colors.warning);

                                if (UI::hot_widget_set(*state, box_in.id) and UI::self_or_empty_focus_widget(*state, box_in.id))
                                {
                                    // Create tooltip.
                                    if (not state->tooltip.enabled)
                                    {
                                        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
                                        String8 msg = str8_fmt(scratch.arena, "Conflict with: '%S' : %S",
                                            Hotkeys::describe_match(conflict),
                                            hk.entry->assignment);

                                        auto palette = *CmdBuffer::current_palette(*lst);
                                        palette.text = colors.warning;
                                        CmdBuffer::push_color_palette(lst, palette);
                                        UI::Widgets::TextTooltipInput tip_in{
                                            .text = sv_str8(msg),
                                            .padding = Help::Data::padding,
                                            .screen_pos = state->mouse.ui_mouse
                                        };
                                        UI::Widgets::basic_text_tooltip(state, lst, &font_ctx, tip_in);
                                        CmdBuffer::pop_color_palette(lst);
                                        Arena::scratch_end(scratch);
                                    }
                                }
                            }

                            box_in.pos = pos;
                            box_in.size.x = rep(content_vp.width) + 0.f;
                            box_in.size.y = entry_height;
                            box_in.id = hk.entry->id;
                            UI::Widgets::basic_box(lst, state, box_in, Flg::Clickable);
                            if (currently_selected
                                or (UI::hot_widget_set(*state, box_in.id) and UI::self_or_empty_focus_widget(*state, box_in.id)))
                            {
                                UI::Widgets::basic_box(lst, state, box_in, Flg::Fill);
                                // Now we can make the decision if this should signal a hotkey trigger or not.
                                // Either the hot widget is double-clicked, or return was hit and there is a filter applied.
                                if ((state->focus_widget == box_in.id and UI::clicked_count(*state, UI::MouseButton::L) == 2)
                                    or (currently_selected and UI::down(*state, OS::Key::Return)))
                                {
                                    resp.next_frame_hk = hk.entry->hk_entry.cmd;
                                    // Let's also eat the return event, because what will happen is that we will close this dialog and return will still
                                    // be in the input queue, so the next focus widget after this command mode will try to process it.
                                    UI::eat(state, OS::Key::Return);
                                }
                            }
                        }
                        desc_data.desc = &hk.entry->name;
                        desc_data.pos = pos;
                        desc_data.pos.x += Help::Data::padding;
                        desc_data.pos.y += (entry_height + font_ctx.current_font_line_height()) / 5.f;
                        build_description(lst, &font_ctx, desc_data);
                    }

                    if (pos.y < -font_ctx.current_font_line_height())
                        break;
                    ++idx;
                }

                if (pos.y < -font_ctx.current_font_line_height())
                    break;

                // Custom hotkeys.
                for (auto& chk : cmp.custom_hotkeys)
                {
                    const bool currently_selected = idx == data->ui_data.sel_index;
                    pos.y -= entry_height;
                    if (pos.y <= rep(content_vp.height))
                    {
                        using Flg = UI::Widgets::BuildBoxFlags;
                        // Hotkey button.
                        {
                            btn_in.id = UI::Widgets::make_id_seed(chk.entry->id, "hotkey");
                            if (state->rebinding_hotkey and state->hotkeys.rebind_widget == btn_in.id)
                            {
                                // Make a special button for the rebind target.
                                UI::Widgets::BuildIconicButtonInput btn_ico_in{
                                    .id = btn_in.id,
                                    .icon = Glyph::SpecialGlyph::X,
                                    .padding = { Help::Data::padding, Help::Data::padding },
                                    .thickness = Help::Data::padding
                                };
                                Vec2f size = UI::Widgets::measure_iconic_button(&font_ctx, btn_ico_in);
                                btn_ico_in.pos = pos;
                                btn_ico_in.pos.x += rep(content_vp.width) - (Help::Data::padding + size.x);
                                btn_ico_in.pos.y += (entry_height - size.y) / 2.f;
                                auto palette = *CmdBuffer::current_palette(*lst);
                                palette.fill = colors.window_close_button_hover;
                                CmdBuffer::push_color_palette(lst, palette);
                                auto btn_resp = UI::Widgets::basic_iconic_button(lst, state, &font_ctx, btn_ico_in, UI::Widgets::BuildButtonFlags::Strike);
                                CmdBuffer::pop_color_palette(lst);
                                // Create tooltip.
                                if (not state->tooltip.enabled and UI::hot_widget_set(*state, btn_ico_in.id))
                                {
                                    std::string_view txt = "Clear binding";
                                    UI::Widgets::TextTooltipInput tip_in{
                                        .text = txt,
                                        .padding = Help::Data::padding,
                                        .screen_pos = state->mouse.ui_mouse
                                    };
                                    UI::Widgets::basic_text_tooltip(state, lst, &font_ctx, tip_in);
                                }

                                if (btn_resp.clicked)
                                {
                                    resp.clear_id = chk.entry->hk_entry.id;
                                }
                            }
                            else
                            {
                                btn_in.label = sv_str8(chk.entry->assignment);
                                Vec2f size = UI::Widgets::measure_button(&font_ctx, btn_in);
                                btn_in.pos = pos;
                                btn_in.pos.x += rep(content_vp.width) - (Help::Data::padding + size.x);
                                btn_in.pos.y += (entry_height - size.y) / 2.f;
                                auto btn_resp = UI::Widgets::basic_button(lst, state, &font_ctx, btn_in, UI::Widgets::BuildButtonFlags::Strike);
                                if (btn_resp.clicked)
                                {
                                    state->hotkeys.cust_hk.rebind_target = chk.entry->hk_entry.id;
                                    state->hotkeys.rebind_widget = btn_in.id;
                                    state->rebinding_hotkey = true;
                                }
                            }
                        }

                        // Build the iconic button for 'help' first so we can absorb mouse events first.
                        desc_data.desc = &chk.entry->name;
                        desc_data.pos = pos;
                        desc_data.pos.x += Help::Data::padding;
                        desc_data.pos.y += (entry_height + font_ctx.current_font_line_height()) / 5.f;
                        if (Hotkeys::is_custom_hotkey_bound(chk.entry->hk_entry.id))
                        {
                            // Build an info button to display the description with a tooltip.
                            UI::Widgets::BuildIconicButtonInput in = {
                                .id = UI::Widgets::make_id_seed(chk.entry->id, "help-cust-info"),
                                .icon = Glyph::SpecialGlyph::Wrench,
                                .pos = pos,
                                .padding = Help::Data::padding,
                                .thickness = Help::Data::padding
                            };
                            Vec2f btn_size = UI::Widgets::measure_iconic_button(&font_ctx, in);

                            // Position the button centered vertically.
                            in.pos.y += (entry_height - btn_size.y) / 2.f;

                            // Shift the short name.
                            desc_data.pos.x += btn_size.x + Help::Data::padding;
                            auto btn_resp = UI::Widgets::basic_iconic_button(lst, state, &font_ctx, in, UI::Widgets::BuildButtonFlags::None);

                            if (btn_resp.clicked)
                            {
                                resp.builder_id = chk.entry->hk_entry.id;
                                resp.group = cmp.group;
                            }

                            if (not state->tooltip.enabled and UI::hot_widget_set(*state, in.id))
                            {
                                UI::Widgets::TextTooltipInput tip_in = {
                                    .text = sv_str8(chk.entry->desc),
                                    .padding = Help::Data::padding,
                                    .screen_pos = state->mouse.ui_mouse
                                };
                                UI::Widgets::basic_text_tooltip(state, lst, &font_ctx, tip_in);
                            }
                        }
                        else
                        {
                            // Tell the user there are some problems with this hotkey entry.
                            UI::Widgets::BuildIconicButtonInput in = {
                                .id = UI::Widgets::make_id_seed(chk.entry->id, "help-cust-err"),
                                .icon = Glyph::SpecialGlyph::Warning,
                                .pos = pos,
                                .padding = Help::Data::padding,
                                .thickness = Help::Data::padding
                            };
                            Vec2f btn_size = UI::Widgets::measure_iconic_button(&font_ctx, in);

                            // Position the button centered vertically.
                            in.pos.y += (entry_height - btn_size.y) / 2.f;

                            // Shift the short name.
                            desc_data.pos.x += btn_size.x + Help::Data::padding;

                            auto palette = *CmdBuffer::current_palette(*lst);
                            palette.text = colors.warning;
                            CmdBuffer::push_color_palette(lst, palette);
                            auto btn_resp = UI::Widgets::basic_iconic_button(lst, state, &font_ctx, in, UI::Widgets::BuildButtonFlags::None);
                            if (btn_resp.clicked)
                            {
                                resp.builder_id = chk.entry->hk_entry.id;
                                resp.group = cmp.group;
                            }
                            CmdBuffer::pop_color_palette(lst);

                            if (not state->tooltip.enabled and UI::hot_widget_set(*state, in.id))
                            {
                                UI::Widgets::TextTooltipInput tip_in = {
                                    .text = sv_str8(chk.entry->desc),
                                    .padding = Help::Data::padding,
                                    .screen_pos = state->mouse.ui_mouse
                                };
                                UI::Widgets::basic_text_tooltip(state, lst, &font_ctx, tip_in);
                            }
                        }
                        // Trash can icon for deletion.
                        {
                            // Tell the user there are some problems with this hotkey entry.
                            UI::Widgets::BuildIconicButtonInput in = {
                                .id = UI::Widgets::make_id_seed(chk.entry->id, "delete-cust-hk"),
                                .icon = Glyph::SpecialGlyph::Trash,
                                .pos = pos,
                                .padding = Help::Data::padding,
                                .thickness = Help::Data::padding
                            };
                            Vec2f btn_size = UI::Widgets::measure_iconic_button(&font_ctx, in);

                            // Shift button based on button above.
                            in.pos.x = desc_data.pos.x;

                            // Position the button centered vertically.
                            in.pos.y += (entry_height - btn_size.y) / 2.f;

                            // Shift the short name.
                            desc_data.pos.x += btn_size.x + Help::Data::padding;

                            auto palette = *CmdBuffer::current_palette(*lst);
                            palette.text = colors.error;
                            CmdBuffer::push_color_palette(lst, palette);
                            auto btn_resp = UI::Widgets::basic_iconic_button(lst, state, &font_ctx, in, UI::Widgets::BuildButtonFlags::None);
                            if (btn_resp.clicked)
                            {
                                resp.remove_id = chk.entry->hk_entry.id;
                                resp.group = cmp.group;
                            }
                            CmdBuffer::pop_color_palette(lst);

                            if (not state->tooltip.enabled and UI::hot_widget_set(*state, in.id))
                            {
                                UI::Widgets::TextTooltipInput tip_in = {
                                    .text = "Delete custom hotkey",
                                    .padding = Help::Data::padding,
                                    .screen_pos = state->mouse.ui_mouse
                                };
                                UI::Widgets::basic_text_tooltip(state, lst, &font_ctx, tip_in);
                            }
                        }
                        // Transparent box for capturing hover/selection.
                        {
                            // Conflict notification, relative to button above.
                            Hotkeys::HotkeyMatch conflict = chk.entry->conflict;
                            if (Hotkeys::valid_match(conflict))
                            {
                                Vec2f warn_pos = btn_in.pos;
                                auto warn_size = font_ctx.icon_glyph_size(Glyph::SpecialGlyph::Warning);
                                warn_pos.x -= warn_size.x + Help::Data::padding;
                                warn_pos.y = pos.y + (entry_height + warn_size.y) / 5.f;

                                box_in.pos = warn_pos;
                                box_in.size = warn_size;
                                box_in.id = UI::Widgets::make_id_seed(chk.entry->id, "conflict");
                                UI::Widgets::basic_box(lst, state, box_in, Flg::Clickable);

                                CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
                                font_ctx.render_icon_glyph(lst, Glyph::SpecialGlyph::Warning, warn_pos, colors.warning);

                                if (UI::hot_widget_set(*state, box_in.id) and UI::self_or_empty_focus_widget(*state, box_in.id))
                                {
                                    // Create tooltip.
                                    if (not state->tooltip.enabled)
                                    {
                                        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
                                        String8 msg = str8_fmt(scratch.arena, "Conflict with: '%S' : %S",
                                            Hotkeys::describe_match(conflict),
                                            chk.entry->assignment);

                                        auto palette = *CmdBuffer::current_palette(*lst);
                                        palette.text = colors.warning;
                                        CmdBuffer::push_color_palette(lst, palette);
                                        UI::Widgets::TextTooltipInput tip_in{
                                            .text = sv_str8(msg),
                                            .padding = Help::Data::padding,
                                            .screen_pos = state->mouse.ui_mouse
                                        };
                                        UI::Widgets::basic_text_tooltip(state, lst, &font_ctx, tip_in);
                                        CmdBuffer::pop_color_palette(lst);
                                        Arena::scratch_end(scratch);
                                    }
                                }
                            }
                            box_in.pos = pos;
                            box_in.size.x = rep(content_vp.width) + 0.f;
                            box_in.size.y = entry_height;
                            box_in.id = chk.entry->id;
                            // We're going to fill this box with its own color to help users delineate between
                            // custom hotkeys and builtins.
                            {
                                auto palette = *CmdBuffer::current_palette(*lst);
                                palette.fill = colors.custom_hotkey_fill;
                                CmdBuffer::push_color_palette(lst, palette);
                                UI::Widgets::basic_box(lst, state, box_in, Flg::Clickable | Flg::Fill);
                                CmdBuffer::pop_color_palette(lst);
                            }
                            if (currently_selected
                                or (UI::hot_widget_set(*state, box_in.id) and UI::self_or_empty_focus_widget(*state, box_in.id)))
                            {
                                UI::Widgets::basic_box(lst, state, box_in, Flg::Fill);
                                // Now we can make the decision if this should signal a hotkey trigger or not.
                                // Either the hot widget is double-clicked, or return was hit and there is a filter applied.
                                if ((state->focus_widget == box_in.id and UI::clicked_count(*state, UI::MouseButton::L) == 2)
                                    or (currently_selected and UI::down(*state, OS::Key::Return)))
                                {
                                    resp.next_frame_chk = chk.entry->hk_entry.id;
                                    // Let's also eat the return event, because what will happen is that we will close this dialog and return will still
                                    // be in the input queue, so the next focus widget after this command mode will try to process it.
                                    UI::eat(state, OS::Key::Return);
                                }
                            }
                        }
                        build_description(lst, &font_ctx, desc_data);
                    }

                    if (pos.y < -font_ctx.current_font_line_height())
                        break;
                    ++idx;
                }

                if (pos.y < -font_ctx.current_font_line_height())
                    break;
            }

            // Content viewport.
            CmdBuffer::pop_clip(lst);
            // Scroll viewport.
            CmdBuffer::pop_clip(lst);

            return resp;
        }

        void build_about(Help::Data* data,
                            CmdBuffer::DrawList* lst,
                            UI::UIState* state,
                            Clipboard::ClipboardManager* clipboard,
                            Feed::MessageFeed* feed)
        {
            auto window_vp = data->window.content_viewport(data->window.window_viewport());
            window_vp.height = Height{ rep(window_vp.height) - buttons_height(data, window_vp) };
            CmdBuffer::push_clip(lst, UI::convert(window_vp));
            // We want to show the logo aligned to the left and 'about' text to the right.
            // For optimal viewing of the logo, let's not stretch it.
            UI::LogoInfo lgo = UI::gap_logo(feed);

            // Measure ideal text placement.
            auto font_ctx = data->atlas->render_font_context(data->font_size);
            const int line_height = font_ctx.current_font_line_height();
            float max_line_x = font_ctx.measure_text(BUILD_TITLE).x;
            max_line_x = std::max(font_ctx.measure_text(HUMAN_VERSION_STRING).x, max_line_x);
            max_line_x = std::max(font_ctx.measure_text(HUMAN_BUILD_STRING).x, max_line_x);
            max_line_x = std::max(font_ctx.measure_text(HUMAN_CONFIG_STRING).x, max_line_x);
            // Max for alpha note.
            const std::string_view alpha_note = ALPHA_NOTE;
            UI::Widgets::MultiLineTextboxInput txt_in{
                .text = alpha_note,
                .pos = {},
                .padding = 0.f,
                .align = UI::Widgets::TextAlign::Left
            };
            Vec2f alpha_note_measure = UI::Widgets::measure_multiline_textbox(&font_ctx, txt_in);
            max_line_x = std::max(alpha_note_measure.x, max_line_x);
            float ideal_text_x = std::max(0.f, rep(window_vp.width) - (max_line_x + Help::Data::padding * 2));

            // Half the viewport so we can create an ideal viewport for it.
            auto logo_vp = window_vp;
            logo_vp.width = Width{ static_cast<int>(ideal_text_x) };
            auto [pos, size] = UI::pos_size_clip(CmdBuffer::ClipRect::basic(lgo.dim));
            size = UI::resize_to_maintain_aspect_ratio(size, { .width = logo_vp.width, .height = logo_vp.height });
            CmdBuffer::start_images(lst, Render::VertShader::OneOneTransform);
            CmdBuffer::push_texture(lst, lgo.tex);
            pos.y = rep(window_vp.height) - size.y;
            pos.y -= (rep(window_vp.height) - size.y) / 2;

            // Flip the image so it will be presented properly.
            pos.y += size.y;
            size.y = -size.y;
            CmdBuffer::render_image(lst, Render::FragShader::BasicColor, pos, size, {}, { 1.f }, hex_to_vec4f(0xFFFFFFFF));
            CmdBuffer::pop_texture(lst);

            // About text.
            CmdBuffer::start_glyph_run(lst, Render::VertShader::OneOneTransform);
            pos.x = size.x + Help::Data::padding * 2;
            pos.y = rep(logo_vp.height) - line_height + 0.f;
            const auto* palette = CmdBuffer::current_palette(*lst);
            font_ctx.render_text(lst, BUILD_TITLE, pos, palette->text);
            pos.y -= line_height;
            font_ctx.render_text(lst, HUMAN_VERSION_STRING, pos, palette->text);
            pos.y -= line_height;
            font_ctx.render_text(lst, HUMAN_BUILD_STRING, pos, palette->text);
            pos.y -= line_height;
            font_ctx.render_text(lst, HUMAN_CONFIG_STRING, pos, palette->text);

            // Alpha note.
            pos.y -= line_height * 2;
            pos.x = size.x + Help::Data::padding * 2;
            txt_in.pos = pos;
            UI::Widgets::build_multiline_textbox(lst, &font_ctx, txt_in);

            // Copy button.
            UI::Widgets::BuildIconicButtonInput in{
                .id = UI::Widgets::make_id_seed(UI::Widgets::ID::Help, "copy-about"),
                .icon = Glyph::SpecialGlyph::Copy,
                .pos = pos,
                .padding = Help::Data::padding,
            };
            auto btn_measure = UI::Widgets::measure_iconic_button(&font_ctx, in);
            in.pos.x = rep(window_vp.width) - btn_measure.x - Help::Data::padding;
            in.pos.y = rep(window_vp.height) - btn_measure.y - Help::Data::padding;
            {
                auto resp = UI::Widgets::basic_iconic_button(lst, state, &font_ctx, in, UI::Widgets::BuildButtonFlags::None);
                if (not state->tooltip.enabled and UI::hot_widget_set(*state, in.id))
                {
                    UI::Widgets::TextTooltipInput tip_in = {
                        .text = "Copy to clipboard",
                        .padding = Help::Data::padding,
                        .screen_pos = state->mouse.ui_mouse
                    };
                    UI::Widgets::basic_text_tooltip(state, lst, &font_ctx, tip_in);
                }

                if (resp.clicked)
                {
                    if (clipboard->set_clipboard(str8_mut(str8_literal(BUILD_TITLE "\n" HUMAN_VERSION_STRING "\n" HUMAN_BUILD_STRING "\n" HUMAN_CONFIG_STRING)), feed))
                    {
                        feed->queue_info("Copied.");
                    }
                }
            }

            CmdBuffer::pop_clip(lst);
        }
    } // namespace [anon]

    Help::Help(Glyph::Atlas* atlas):
        data{ new Data{
            .window = { UI::Widgets::ID::Help },
            .scrollbox = { UI::Widgets::ID::Help },
            .atlas = atlas
        } }
    {
        data->components_arena = Arena::alloc(Arena::default_params);
        data->window.title("Help");
    }

    Help::~Help() = default;

    // Interaction.
    void Help::sync_config()
    {
        init_collections(data.get());
        // TODO: Update bindings, if necessary.
        data->font_size = Glyph::FontSize{ Config::widget_state().window_title_font_size };
        data->window.sync_config(data->atlas);
        data->scrollbox.sync_config();
        apply_filters(data.get());
    }

    void Help::hotkeys_updated()
    {
        init_collections(data.get());
        apply_filters(data.get());
    }

    void Help::start(const ScreenDimensions& screen, UI::UIState* state)
    {
        UI::Widgets::ShowWindowData show_data{
            .initial_viewport = initial_window_viewport(screen),
            .expand_point = { 0.5f, 0.5f }
        };
        data->window.show(show_data);
        UI::set_focus_window(state, UI::Widgets::ID::Help);
        setup_ui_data(data.get());
    }

    BuildHelpResponse Help::build(CmdBuffer::DrawList* lst,
                                    UI::UIState* state,
                                    Clipboard::ClipboardManager* clipboard,
                                    Feed::MessageFeed* feed)
    {
        BuildHelpResponse resp{};
        // Render widgets.
        {
            auto window_resp = data->window.build(lst, data->atlas, state);
            resp.close = window_resp.close;
        }

        // Build mode buttons first so we can decide what to show.
        {
            auto content_vp = data->window.content_viewport(data->window.window_viewport());
            CmdBuffer::push_clip(lst, UI::convert(content_vp));
            auto font_ctx = data->atlas->render_font_context(data->font_size);
            UI::Widgets::BuildButtonInput in{
                .id = data->ui_data.hotkeys_id,
                .label = "Hotkeys",
                .padding = { Data::padding, 0.f },
                .thickness = Data::padding
            };
            in.pos.x = 0;
            in.pos.y = rep(content_vp.height) - (UI::standard_font_padding(data->font_size) + in.thickness * 2);
            auto btn_resp = UI::Widgets::basic_button(lst, state, &font_ctx, in, UI::Widgets::BuildButtonFlags::Strike);
            if (btn_resp.clicked)
            {
                data->ui_data.mode = Mode::Hotkeys;
            }
            in.id = data->ui_data.about_id;
            in.label = "About";
            in.pos.x += btn_resp.btn_size.x + Data::padding;
            btn_resp = UI::Widgets::basic_button(lst, state, &font_ctx, in, UI::Widgets::BuildButtonFlags::Strike);
            if (btn_resp.clicked)
            {
                data->ui_data.mode = Mode::About;
            }

            // Filter for hotkeys.
            if (data->ui_data.mode == Mode::Hotkeys)
            {
                UI::Widgets::CheckboxInput cb_in = {
                    .label = "Show conflicts",
                    .checked = {}
                };
                Vec2f cb_size = UI::Widgets::measure_checkbox(&font_ctx, cb_in);
                in.forced_size = cb_size;
                in.id = data->ui_data.show_conflicts_id;
                in.pos.x = rep(content_vp.width) - in.forced_size.x;
                in.label = "";
                btn_resp = UI::Widgets::basic_button(lst, state, &font_ctx, in, UI::Widgets::BuildButtonFlags::None);
                if (btn_resp.clicked)
                {
                    data->ui_data.show_conflicts_only = not data->ui_data.show_conflicts_only;

                    if (data->ui_data.show_conflicts_only)
                    {
                        filter_by_conflicts(&data->render_components);
                    }
                    else
                    {
                        copy_as_render_entries(&data->components, &data->render_components);
                    }
                }
                cb_in.checked = make_yes_no<UI::Widgets::Checked>(data->ui_data.show_conflicts_only);
                UI::Widgets::build_checkbox(lst, &font_ctx, in.pos, cb_in);

                // Build button for reloading hotkeys.
                const auto* palette = CmdBuffer::current_palette(*lst);
                in.id = data->ui_data.reload_hotkeys_id;
                in.label = "Reload Hotkeys";
                UI::Widgets::BuildIconicTextButtonInput ico_btn = {
                    .btn_in = in,
                    .icon = Glyph::SpecialGlyph::Reset,
                    .icon_color = palette->text,
                };
                Vec2f btn_size = UI::Widgets::measure_left_iconic_text_button(&font_ctx, ico_btn);
                ico_btn.btn_in.forced_size = btn_size;
                ico_btn.btn_in.pos.x = (rep(content_vp.width) - btn_size.x) / 2.f;
                btn_resp = UI::Widgets::basic_left_iconic_text_button(lst, state, &font_ctx, ico_btn, UI::Widgets::BuildButtonFlags::Strike);
                if (btn_resp.clicked)
                {
                    resp.reload_hotkeys = true;
                }
            }
            CmdBuffer::pop_clip(lst);
        }

        switch (data->ui_data.mode)
        {
        case Mode::Hotkeys:
            {
                auto hk_resp = build_hotkeys(data.get(), lst, state);
                if (hk_resp.reset_hk != Hotkeys::Hotkey::None)
                {
                    Hotkeys::reset_to_default(hk_resp.reset_hk);
                    hotkeys_updated();
                }

                if (hk_resp.clear_hk != Hotkeys::Hotkey::None)
                {
                    UI::clear_hotkey_rebind_state(state);
                    Hotkeys::clear_binding(hk_resp.clear_hk);
                    hotkeys_updated();
                }

                if (hk_resp.clear_id != Hotkeys::CustomHotkeyID::None)
                {
                    UI::clear_hotkey_rebind_state(state);
                    Hotkeys::clear_binding(hk_resp.clear_id);
                    hotkeys_updated();
                }

                if (hk_resp.next_frame_hk != Hotkeys::Hotkey::None
                    or hk_resp.next_frame_chk != Hotkeys::CustomHotkeyID::None)
                {
                    state->hotkeys.next_frame_hk = hk_resp.next_frame_hk;
                    state->hotkeys.cust_hk.next_frame_hk = hk_resp.next_frame_chk;
                    // Let's also clear the focus widget so the next frame can properly respond
                    // to the hotkey without believing there's a widget set.
                    UI::force_set_focus_widget(state, UI::Widgets::ID::Sentinel);
                    resp.close = true;
                }

                if (hk_resp.builder_id != Hotkeys::CustomHotkeyID::None)
                {
                    resp.builder_id = hk_resp.builder_id;
                }

                if (hk_resp.remove_id != Hotkeys::CustomHotkeyID::None)
                {
                    resp.remove_id = hk_resp.remove_id;
                }
                resp.group = hk_resp.group;
            }
            break;
        case Mode::About:
            build_about(data.get(), lst, state, clipboard, feed);
            break;
        }

        // Finish the window.
        data->window.end(state);

        return resp;
    }
} // namespace Help