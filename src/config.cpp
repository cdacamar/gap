#include "config.h"

#include <cassert>

#include <algorithm>

#include "feed.h"
#include "gap-bits.h"
#include "gap-strings.h"
#include "tiny-toml.h"
#include "util.h"

namespace Config
{
    namespace
    {
        using LoadValueFn = void(*)(Arena::Arena*, TinyToml::ParseMsg*, void*);

        struct ConfigMsgNode
        {
            ConfigMsgNode* next;
            String8 key;
            TinyToml::ParseMsg* msg;
            void* data;
            LoadValueFn loader_fn;
        };

        struct ConfigMsgSet
        {
            ConfigMsgNode** buckets;
            uint64_t capacity;
            uint64_t count;
            uint64_t nil_buckets;
            uint64_t load;
        };

        ConfigMsgSet make_config_msg_set(Arena::Arena* arena, uint64_t hint_size)
        {
            ConfigMsgSet set{};
            const uint64_t pow_2_aligned_size = up_pow2(hint_size);
            set.capacity = pow_2_aligned_size;
            set.nil_buckets = set.capacity;
            set.buckets = Arena::push_array<ConfigMsgNode*>(arena, set.capacity);
            return set;
        }

        ConfigMsgNode* fetch_config_msg(const ConfigMsgSet& set, String8 key)
        {
            HashResult hash;
            hash_str8(key, &hash);
            uint64_t idx = (set.capacity - 1) & hash.result[0];
            ConfigMsgNode* slot = set.buckets[idx];
            while (slot != nullptr)
            {
                if (str8_match_exact(slot->key, key))
                    break;
                slot = slot->next;
            }
            return slot;
        }

        void push_config_msg_set_msg(Arena::Arena* arena,
                                        ConfigMsgSet* set,
                                        String8 key,
                                        void* data,
                                        LoadValueFn loader_fn)
        {
            HashResult hash;
            hash_str8(key, &hash);
            uint64_t idx = (set->capacity - 1) & hash.result[0];
            ConfigMsgNode* slot = set->buckets[idx];
            if (slot == nullptr)
            {
                slot = Arena::push_array<ConfigMsgNode>(arena, 1);
                slot->data = data;
                slot->loader_fn = loader_fn;
                slot->key = str8_copy(arena, key);
                set->buckets[idx] = slot;
                ++set->count;
                --set->nil_buckets;
                set->load = std::max(set->load, uint64_t(1));
            }
            else
            {
                uint64_t elms = 0;
                bool insert = true;
                while (slot->next != nullptr)
                {
                    if (str8_match_exact(slot->key, key))
                    {
                        insert = false;
                        break;
                    }
                    ++elms;
                    slot = slot->next;
                }

                if (str8_match_exact(slot->key, key))
                {
                    insert = false;
                }

                if (insert)
                {
                    ConfigMsgNode* new_entry = Arena::push_array<ConfigMsgNode>(arena, 1);
                    new_entry->data = data;
                    new_entry->loader_fn = loader_fn;
                    new_entry->key = str8_copy(arena, key);
                    slot->next = new_entry;
                    ++set->count;
                    elms += 2; // +1 for current slot and +1 for new node.
                    set->load = std::max(set->load, elms);
                }
            }
        }

        struct ConfigTableNode
        {
            ConfigTableNode* next;
            String8 key;
            ConfigMsgSet pair_set;
        };

        struct ConfigTableSet
        {
            ConfigTableNode** buckets;
            uint64_t capacity;
            uint64_t count;
            uint64_t nil_buckets;
            uint64_t load;
        };

        ConfigTableSet make_config_table_set(Arena::Arena* arena, uint64_t hint_size)
        {
            ConfigTableSet set{};
            const uint64_t pow_2_aligned_size = up_pow2(hint_size);
            set.capacity = pow_2_aligned_size;
            set.nil_buckets = set.capacity;
            set.buckets = Arena::push_array<ConfigTableNode*>(arena, set.capacity);
            return set;
        }

        ConfigTableNode* fetch_config_table(const ConfigTableSet& set, String8 key)
        {
            HashResult hash;
            hash_str8(key, &hash);
            uint64_t idx = (set.capacity - 1) & hash.result[0];
            ConfigTableNode* slot = set.buckets[idx];
            while (slot != nullptr)
            {
                if (str8_match_exact(slot->key, key))
                    break;
                slot = slot->next;
            }
            return slot;
        }

        void push_config_table_set_cfg(Arena::Arena* arena,
                                        ConfigTableSet* set,
                                        String8 key,
                                        const ConfigMsgSet& pair_set)
        {
            HashResult hash;
            hash_str8(key, &hash);
            uint64_t idx = (set->capacity - 1) & hash.result[0];
            ConfigTableNode* slot = set->buckets[idx];
            if (slot == nullptr)
            {
                slot = Arena::push_array<ConfigTableNode>(arena, 1);
                slot->pair_set = pair_set;
                slot->key = str8_copy(arena, key);
                set->buckets[idx] = slot;
                ++set->count;
                --set->nil_buckets;
                set->load = std::max(set->load, uint64_t(1));
            }
            else
            {
                uint64_t elms = 0;
                bool insert = true;
                while (slot->next != nullptr)
                {
                    if (str8_match_exact(slot->key, key))
                    {
                        insert = false;
                        break;
                    }
                    ++elms;
                    slot = slot->next;
                }

                if (str8_match_exact(slot->key, key))
                {
                    insert = false;
                }

                if (insert)
                {
                    ConfigTableNode* new_entry = Arena::push_array<ConfigTableNode>(arena, 1);
                    new_entry->pair_set = pair_set;
                    new_entry->key = str8_copy(arena, key);
                    slot->next = new_entry;
                    ++set->count;
                    elms += 2; // +1 for current slot and +1 for new node.
                    set->load = std::max(set->load, elms);
                }
            }
        }

        // The computes the brightness based on the W3C formula.
        float weighted_w3c(Vec4f color)
        {
            // Source: https://mixable.blog/black-or-white-text-on-a-colour-background/
            float bright_r = color.x * 255.f * 0.299f;
            float bright_g = color.y * 255.f * 0.587f;
            float bright_b = color.z * 255.f * 0.114f;
            return bright_r + bright_g + bright_b;
        }

        // Sourced from: https://mixable.blog/adjust-text-color-to-be-readable-on-light-and-dark-backgrounds-of-user-interfaces/
        Vec4f readable_color_for_any_bg(Vec4f color)
        {
            auto hsv = rgb_to_hsv(color);

            constexpr float step = 0.01f;
            // Normally we're supposed to use 127.f but I have found that, since fred, by default, wants to be in the dark-mode
            // spectrum, it makes more sense to tune the colors biased towards light backgrounds when 'light_mode' is active,
            // meaning we're going to tend towards darkening all colors.
            //constexpr float bright_cap = 127.f;
            constexpr float bright_cap = 115.f;

            float brightness = weighted_w3c(color);
            if (brightness < bright_cap)
            {
                while (brightness < bright_cap and hsv.z >= 0.f and hsv.z <= 1.f)
                {
                    hsv.z += step;
                    brightness = weighted_w3c(hsv_to_rgb(hsv));
                }
            }
            else
            {
                while (brightness > bright_cap and hsv.z >= 0.f and hsv.z <= 1.f)
                {
                    hsv.z -= step;
                    brightness = weighted_w3c(hsv_to_rgb(hsv));
                }
            }

            return hsv_to_rgb(hsv);
        }

        // Some sane defaults.
        HotkeyState hotkey_state_instance =
        {
            .hotkeys = str8_empty
        };

        DiffColors diff_colors_instance =
        {
            .background                 = hex_to_vec4f(0x1F1F1FFF),
            .line_numbers               = hex_to_vec4f(0x8A8592FF),
            .whitespace                 = hex_to_vec4f(0xE3E4E229),
            .del_line                   = hex_to_vec4f(0xFF808040),
            .ins_line                   = hex_to_vec4f(0x7EFF7E40),
            .eq_line                    = hex_to_vec4f(0xFFFFFF40),
            .gap_line                   = hex_to_vec4f(0xD3D4D419),
            .del_txt                    = hex_to_vec4f(0xFF80807E),
            .ins_txt                    = hex_to_vec4f(0x7EFF7E7E),
            .eq_txt                     = hex_to_vec4f(0xFFFFFFFF),
            .trimmed_text               = hex_to_vec4f(0xE3811C7E),
            .del_mark                   = hex_to_vec4f(0xFF0000FF),
            .ins_mark                   = hex_to_vec4f(0x00FF00FF),
            .selected_dir_file          = hex_to_vec4f(0x00ADFF80),
            .diff_computing             = hex_to_vec4f(0x00ADFFFF),
        };

        DiffColors diff_colors_inverse_instance;

        // For supporting color inversion mode toggling.
        DiffColors* current_diff_colors_instance = &diff_colors_instance;

        DiffState diff_state_instance =
        {
            .diff_font_size = 14,
            .show_line_numbers = true,
            .word_based_diff = true,
            .context_window = 3, // This is the default for Linux diff... so...
        };

        FeedColors feed_colors_instance =
        {
            .info    = hex_to_vec4f(0xD4D4D4FF),
            .warning = hex_to_vec4f(0xE3811CFF),
            .error   = hex_to_vec4f(0xFF0000FF),
        };

        FeedColors feed_colors_inverse_instance;

        // For supporting color inversion mode toggling.
        FeedColors* current_feed_colors_instance = &feed_colors_instance;

        FeedState feed_state_instance =
        {
            .feed_font_size = 14,
        };

        WidgetColors widget_colors_instance =
        {
            .window_border               = hex_to_vec4f(0xADD6FF26),
            .window_title_background     = hex_to_vec4f(0xADD6FF26),
            .window_title_font_color     = hex_to_vec4f(0xD4D4D4FF),
            .window_close_button_hover   = hex_to_vec4f(0xF44747FF),
            .window_close_button_pressed = hex_to_vec4f(0xCD1010FF),
            .scrollbar_inactive          = hex_to_vec4f(0xADD6FF26),
            .scrollbar_active            = hex_to_vec4f(0xADD6FFFF),
            .scrollbar_track_outline     = hex_to_vec4f(0xADD6FF26),
            .custom_hotkey_fill          = hex_to_vec4f(0x0049FF31),
            .drag_drop_zone              = hex_to_vec4f(0xADD6FFFF),
            .outline_selection           = hex_to_vec4f(0x009FFFFF),
            .warning                     = hex_to_vec4f(0xE3811CFF),
            .error                       = hex_to_vec4f(0xFF0000FF),
            .active_button               = hex_to_vec4f(0x0098FFFF),
            .loading_bar_progress        = hex_to_vec4f(0x0098FF80),
            .loading_bar_complete        = hex_to_vec4f(0x009A0280),
            .B_color                     = hex_to_vec4f(0xFFFFFFFF),
            .KB_color                    = hex_to_vec4f(0x64DBEAFF),
            .MB_color                    = hex_to_vec4f(0x9627D8FF),
            .GB_color                    = hex_to_vec4f(0xCD3256FF),
            .TB_color                    = hex_to_vec4f(0xD9B026FF),
            .PB_color                    = hex_to_vec4f(0xFF0A00FF),
            .EB_color                    = hex_to_vec4f(0xFFFFFFFF),
        };

        WidgetColors widget_colors_inverse_instance;

        // For supporting color inversion mode toggling.
        WidgetColors* current_widget_colors_instance = &widget_colors_instance;

        WidgetState widget_state_instance =
        {
            .window_title_font_size = 14,
            .scrollbar_width = 10,
            .scrollbar_min_size = 20,
        };

        SystemCore system_core_instance =
        {
            .core_window_rect = { OS::default_window_pos.x, OS::default_window_pos.y, rep(Constants::screen.width), rep(Constants::screen.height) },
            .async_update_frequency_ms = 1000,
            .noisy_events = false,
            .noisy_flattened_events = false,
            .noisy_diff_timing = false,
            .fullscreen = false,
            .maximized = false,
        };

        SystemFonts system_fonts_instance;

        SystemEffects system_effects_instance;

        // --- SAVING -----------------------------------------------
        struct TomlSaveInput
        {
            Arena::Arena* arena;
            String8List serial_lst;
            char fmt_buf[512];
        };

        void toml_save_color(TomlSaveInput* in, String8 name, const Vec4f& color)
        {
            uint32_t x = vec4f_to_hex(color);
            String8 entry = fmt_string(in->fmt_buf, "%S = %#x\n", name, x);
            str8_serial_push_str8(in->arena, &in->serial_lst, entry);
        }

        void toml_save_toggle(TomlSaveInput* in, String8 name, bool b)
        {
            String8View vals[] =
            {
                str8_literal("false"),
                str8_literal("true")
            };
            String8 entry = fmt_string(in->fmt_buf, "%S = %S\n", name, str8_mut(vals[b]));
            str8_serial_push_str8(in->arena, &in->serial_lst, entry);
        }

        void toml_save_font_size(TomlSaveInput* in, String8 name, int size)
        {
            String8 entry = fmt_string(in->fmt_buf, "%S = %d\n", name, size);
            str8_serial_push_str8(in->arena, &in->serial_lst, entry);
        }

        void toml_save_px_size(TomlSaveInput* in, String8 name, int size)
        {
            String8 entry = fmt_string(in->fmt_buf, "%S = %d\n", name, size);
            str8_serial_push_str8(in->arena, &in->serial_lst, entry);
        }

        void toml_save_int(TomlSaveInput* in, String8 name, int i)
        {
            String8 entry = fmt_string(in->fmt_buf, "%S = %d\n", name, i);
            str8_serial_push_str8(in->arena, &in->serial_lst, entry);
        }

        void toml_save_path(TomlSaveInput* in, String8 name, String8 path)
        {
            String8 entry = fmt_string(in->fmt_buf, "%S = '%S'\n", name, path);
            str8_serial_push_str8(in->arena, &in->serial_lst, entry);
        }

        void toml_save_file_path(TomlSaveInput* in, String8 name, String8 file)
        {
            String8 entry = fmt_string(in->fmt_buf, "%S = '%S'\n", name, file);
            str8_serial_push_str8(in->arena, &in->serial_lst, entry);
        }

        void toml_save_string(TomlSaveInput* in, String8 name, String8 str)
        {
            auto scratch = Arena::scratch_begin({ &in->arena, 1 });
            String8 escaped_str = TinyToml::escape_string(scratch.arena, str);
            String8 entry = fmt_string(in->fmt_buf, "%S = \"%S\"\n", name, escaped_str);
            str8_serial_push_str8(in->arena, &in->serial_lst, entry);
            Arena::scratch_end(scratch);
        }

        void toml_save_rect_i(TomlSaveInput* in, String8 name, const Vec4i& rect)
        {
            String8 entry = fmt_string(in->fmt_buf, "%S = [%d, %d, %d, %d]\n", name, rect.xyza[0], rect.xyza[1], rect.xyza[2], rect.xyza[3]);
            str8_serial_push_str8(in->arena, &in->serial_lst, entry);
        }

        void toml_save_linebreak(TomlSaveInput* in)
        {
            str8_serial_push_char(in->arena, &in->serial_lst, '\n');
        }

        void toml_save_start_table(TomlSaveInput* in, String8 name)
        {
            str8_serial_push_str8(in->arena, &in->serial_lst, str8_mut(str8_literal("[")));
            str8_serial_push_str8(in->arena, &in->serial_lst, name);
            str8_serial_push_str8(in->arena, &in->serial_lst, str8_mut(str8_literal("]\n")));
        }

#define DAT_CAT_START(T, access, component, path) void toml_save(TomlSaveInput* in, const T& x) { \
    toml_save_start_table(in, str8_mut(str8_literal(path)));
#define DAT_CAT_END() }
#define DAT_COLOR(name, T, desc) toml_save_color(in, str8_mut(str8_literal(#name)), x.name);
#define DAT_TOGGLE(name, T, desc) toml_save_toggle(in, str8_mut(str8_literal(#name)), x.name);
#define DAT_FONTSIZE(name, T, desc) toml_save_font_size(in, str8_mut(str8_literal(#name)), x.name);
#define DAT_PX(name, T, desc) toml_save_px_size(in, str8_mut(str8_literal(#name)), x.name);
#define DAT_INT(name, T, desc) toml_save_int(in, str8_mut(str8_literal(#name)), x.name);
#define DAT_PATH(name, T, desc) toml_save_path(in, str8_mut(str8_literal(#name)), x.name);
#define DAT_FILE(name, T, desc) toml_save_file_path(in, str8_mut(str8_literal(#name)), x.name);
#define DAT_STRING(name, T, desc) toml_save_string(in, str8_mut(str8_literal(#name)), x.name);
#define DAT_RECT_I(name, T, desc) toml_save_rect_i(in, str8_mut(str8_literal(#name)), x.name);
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
#undef DAT_RECT_I

        // --- LOADING ----------------------------------------------
        void toml_load_color(Arena::Arena*, TinyToml::ParseMsg* msg, void* color)
        {
            if (TinyToml::any_integral(msg))
            {
                *reinterpret_cast<Vec4f*>(color) = hex_to_vec4f(static_cast<uint32_t>(TinyToml::integral_val(msg)));
            }
        }

        void toml_load_toggle(Arena::Arena*, TinyToml::ParseMsg* msg, void* b)
        {
            if (TinyToml::any_boolean(msg))
            {
                *reinterpret_cast<bool*>(b) = TinyToml::boolean_val(msg);
            }
        }

        void toml_load_font_size(Arena::Arena*, TinyToml::ParseMsg* msg, void* size)
        {
            if (TinyToml::any_integral(msg))
            {
                *reinterpret_cast<int*>(size) = static_cast<int>(TinyToml::integral_val(msg));
            }
        }

        void toml_load_px_size(Arena::Arena*, TinyToml::ParseMsg* msg, void* value)
        {
            if (TinyToml::any_integral(msg))
            {
                *reinterpret_cast<int*>(value) = static_cast<int>(TinyToml::integral_val(msg));
            }
        }

        void toml_load_int(Arena::Arena*, TinyToml::ParseMsg* msg, void* i)
        {
            if (TinyToml::any_integral(msg))
            {
                *reinterpret_cast<int*>(i) = static_cast<int>(TinyToml::signed_integral_val(msg));
            }
        }

        void toml_load_path(Arena::Arena* arena, TinyToml::ParseMsg* msg, void* path)
        {
            if (TinyToml::any_string(msg))
            {
                *reinterpret_cast<String8*>(path) = str8_copy(arena, TinyToml::string_val(msg));
            }
        }

        void toml_load_file_path(Arena::Arena* arena, TinyToml::ParseMsg* msg, void* file)
        {
            if (TinyToml::any_string(msg))
            {
                *reinterpret_cast<String8*>(file) = str8_copy(arena, TinyToml::string_val(msg));
            }
        }

        void toml_load_string(Arena::Arena* arena, TinyToml::ParseMsg* msg, void* str)
        {
            if (TinyToml::any_string(msg))
            {
                // Process any escapes.
                auto scratch = Arena::scratch_begin({ &arena, 1 });
                String8 escaped_str = TinyToml::process_escapes(scratch.arena, TinyToml::string_val(msg));
                *reinterpret_cast<String8*>(str) = str8_copy(arena, escaped_str);
                Arena::scratch_end(scratch);
            }
        }

        void toml_load_rect_i(Arena::Arena*, TinyToml::ParseMsg* msg, void* rect)
        {
            if (msg->kind == TinyToml::ParseMsg::Kind::Array)
            {
                Vec4i* result = reinterpret_cast<Vec4i*>(rect);
                *result = {};
                uint64_t idx = 0;
                for EachNode(n, msg->array.first)
                {
                    if (TinyToml::any_integral(n->data))
                    {
                        result->xyza[idx++] = static_cast<int>(TinyToml::signed_integral_val(n->data));
                    }
                }
            }
        }

        void process_table_msg(Arena::Arena* arena, ConfigTableSet* cfg_tables, TinyToml::ParseMsg* msg, Feed::MessageFeed* feed)
        {
            auto scratch = Arena::scratch_begin({ &arena, 1 });
            // Fetch the table.
            String8 key = TinyToml::key_val(scratch.arena, msg->table.key);
            ConfigTableNode* table = fetch_config_table(*cfg_tables, key);
            if (table != nullptr)
            {
                // Skip the table.
                msg = msg->next;
                while (msg != nullptr and msg->kind == TinyToml::ParseMsg::Kind::Pair)
                {
                    key = TinyToml::key_val(scratch.arena, msg->pair.key);
                    ConfigMsgNode* val = fetch_config_msg(table->pair_set, key);
                    if (val != nullptr)
                    {
                        if (val->msg == nullptr)
                        {
                            val->msg = msg->pair.val;
                            val->loader_fn(arena, val->msg, val->data);
                        }
                        else
                        {
                            String8 err_msg = str8_fmt(scratch.arena, "Duplicate value for key '%S' at (%u:%u); ignored.", key, unsigned(msg->locus.line), unsigned(msg->locus.col));
                            feed->queue_warning(err_msg);
                        }
                    }
                    else
                    {
                        String8 err_msg = str8_fmt(scratch.arena, "Unknown key '%S' for table '%S' at (%u:%u); ignored.", key, table->key, unsigned(msg->locus.line), unsigned(msg->locus.col));
                        feed->queue_warning(err_msg);
                    }
                    msg = msg->next;
                }
            }
            else
            {
                String8 err_msg = str8_fmt(scratch.arena, "Unknown table '%S' at (%u:%u); ignored.", key, unsigned(msg->locus.line), unsigned(msg->locus.col));
                feed->queue_warning(err_msg);
            }
            Arena::scratch_end(scratch);
        }

        TinyToml::ParseMsg* skip_table(TinyToml::ParseMsg* msg)
        {
            msg = msg->next;
            while (msg != nullptr and msg->kind == TinyToml::ParseMsg::Kind::Pair)
            {
                msg = msg->next;
            }
            return msg;
        }

#define DAT_CAT_START(T, access, component, path) void populate_loader(Arena::Arena* arena, ConfigTableSet* cfg_table, T* x) { \
    ConfigMsgSet pair_set = make_config_msg_set(arena, 32); String8 path_str = str8_mut(str8_literal(path));
#define DAT_CAT_END() push_config_table_set_cfg(arena, cfg_table, path_str, pair_set); }
#define DAT_COLOR(name, T, desc) push_config_msg_set_msg(arena, &pair_set, str8_mut(str8_literal(#name)), &x->name, toml_load_color);
#define DAT_TOGGLE(name, T, desc) push_config_msg_set_msg(arena, &pair_set, str8_mut(str8_literal(#name)), &x->name, toml_load_toggle);
#define DAT_FONTSIZE(name, T, desc) push_config_msg_set_msg(arena, &pair_set, str8_mut(str8_literal(#name)), &x->name, toml_load_font_size);
#define DAT_PX(name, T, desc) push_config_msg_set_msg(arena, &pair_set, str8_mut(str8_literal(#name)), &x->name, toml_load_px_size);
#define DAT_INT(name, T, desc) push_config_msg_set_msg(arena, &pair_set, str8_mut(str8_literal(#name)), &x->name, toml_load_int);
#define DAT_PATH(name, T, desc) push_config_msg_set_msg(arena, &pair_set, str8_mut(str8_literal(#name)), &x->name, toml_load_path);
#define DAT_FILE(name, T, desc) push_config_msg_set_msg(arena, &pair_set, str8_mut(str8_literal(#name)), &x->name, toml_load_file_path);
#define DAT_STRING(name, T, desc) push_config_msg_set_msg(arena, &pair_set, str8_mut(str8_literal(#name)), &x->name, toml_load_string);
#define DAT_RECT_I(name, T, desc) push_config_msg_set_msg(arena, &pair_set, str8_mut(str8_literal(#name)), &x->name, toml_load_rect_i);
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
#undef DAT_RECT_I

// Helpers.

// Color inversions.
#define DAT_CAT_START(T, access, component, path) void populate_inversion([[maybe_unused]] const T& src, [[maybe_unused]] T* inverse) {
#define DAT_CAT_END() }
#define DAT_COLOR(name, T, desc) inverse->name = readable_color_for_any_bg(src.name);
#define DAT_TOGGLE(name, T, desc)
#define DAT_FONTSIZE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_RECT_I(name, T, desc)
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
#undef DAT_RECT_I

#define DAT_CAT_START(T, access, component, path) void move_owned_values_to([[maybe_unused]] Arena::Arena* arena, [[maybe_unused]] T* x) {
#define DAT_CAT_END() }
#define DAT_COLOR(name, T, desc)
#define DAT_TOGGLE(name, T, desc)
#define DAT_FONTSIZE(name, T, desc)
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc) x->name = str8_copy(arena, x->name);
#define DAT_FILE(name, T, desc) x->name = str8_copy(arena, x->name);
#define DAT_STRING(name, T, desc) x->name = str8_copy(arena, x->name);
#define DAT_RECT_I(name, T, desc)
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
#undef DAT_RECT_I

        enum class UseComputedColors : bool { No, Yes };

        void setup_colors(UseComputedColors use_computed)
        {
            if (is_yes(use_computed))
            {
                current_feed_colors_instance = &feed_colors_inverse_instance;
                current_widget_colors_instance = &widget_colors_inverse_instance;
                current_diff_colors_instance = &diff_colors_inverse_instance;
            }
            else
            {
                current_feed_colors_instance = &feed_colors_instance;
                current_widget_colors_instance = &widget_colors_instance;
                current_diff_colors_instance = &diff_colors_instance;
            }
        }

        void populate_inverse_color_states()
        {
            populate_inversion(feed_colors_instance, &feed_colors_inverse_instance);
            populate_inversion(widget_colors_instance, &widget_colors_inverse_instance);
            populate_inversion(diff_colors_instance, &diff_colors_inverse_instance);
            // We really do want to invert the background to get a true inverted light-mode color.
            diff_colors_inverse_instance.background = invert_color(diff_colors_instance.background);
            setup_colors(make_yes_no<UseComputedColors>(system_effects().light_mode));
        }

        SystemFonts default_system_fonts(Arena::Arena* arena)
        {
            SystemFonts fonts{};
            // TODO: We should probably use a default that isn't so... Windows-y.
#if WIN32
            fonts.fallback_fonts_folder = str8_copy(arena, str8_mut(str8_literal("C:\\Windows\\Fonts")));
#elif OS_MAC
            fonts.fallback_fonts_folder = str8_copy(arena, str8_mut(str8_literal("/System/Library/Fonts")));
#elif OS_LINUX
            fonts.fallback_fonts_folder = str8_copy(arena, str8_mut(str8_literal("/usr/share/fonts")));
#endif // WIN32
            fonts.current_font = str8_empty;
            return fonts;
        }

        SystemEffects default_system_effects()
        {
            SystemEffects effects =
            {
                .light_mode = false,
                .smooth_scroll = true,
#if OS_MAC
                .subpixel_font_aa = false,
                .font_hinting = false,
#else
                .subpixel_font_aa = true,
                .font_hinting = true,
#endif
                .render_whitespace = true,
            };
            return effects;
        }

        // Globals.
        bool need_save = false;
        Arena::Arena* cfg_arena;
    } // namespace [anon]

    // Initialization.
    void init()
    {
        cfg_arena = Arena::alloc(Arena::default_params);
        system_fonts_instance = default_system_fonts(cfg_arena);
        system_effects_instance = default_system_effects();
    }

    const DiffColors& diff_colors()
    {
        return *current_diff_colors_instance;
    }

    const DiffState& diff_state()
    {
        return diff_state_instance;
    }

    const FeedColors& feed_colors()
    {
        return *current_feed_colors_instance;
    }

    const FeedState& feed_state()
    {
        return feed_state_instance;
    }

    const WidgetColors& widget_colors()
    {
        return *current_widget_colors_instance;
    }

    const WidgetState& widget_state()
    {
        return widget_state_instance;
    }

    const SystemCore& system_core()
    {
        return system_core_instance;
    }

    const SystemFonts& system_fonts()
    {
        return system_fonts_instance;
    }

    const SystemEffects& system_effects()
    {
        return system_effects_instance;
    }

    const HotkeyState& hotkey_state()
    {
        return hotkey_state_instance;
    }

    bool needs_save()
    {
        return need_save;
    }

    void update(const DiffColors& new_state)
    {
        diff_colors_instance = new_state;
        populate_inverse_color_states();
        need_save = true;
    }

    void update(const DiffState& new_state)
    {
        diff_state_instance = new_state;
        need_save = true;
    }

    void update(const SystemCore& new_state)
    {
        system_core_instance = new_state;
        populate_inverse_color_states();
        need_save = true;
    }

    void update(const FeedColors& new_state)
    {
        feed_colors_instance = new_state;
        populate_inverse_color_states();
        need_save = true;
    }

    void update(const FeedState& new_state)
    {
        feed_state_instance = new_state;
        need_save = true;
    }

    void update(const WidgetColors& new_state)
    {
        widget_colors_instance = new_state;
        populate_inverse_color_states();
        need_save = true;
    }

    void update(const WidgetState& new_state)
    {
        widget_state_instance = new_state;
        need_save = true;
    }

    void update(const SystemFonts& new_state)
    {
        system_fonts_instance = new_state;
        need_save = true;
    }

    void update(const SystemEffects& new_state)
    {
        system_effects_instance = new_state;
        populate_inverse_color_states();
        need_save = true;
    }

    void update(const HotkeyState& new_state)
    {
        hotkey_state_instance = new_state;
        need_save = true;
    }

    void add_delta_all_font_sizes(int delta)
    {
#define DAT_CAT_START(T, access, component, path) { [[maybe_unused]] T* inst = &(access ## _instance);
#define DAT_CAT_END() }
#define DAT_COLOR(name, T, desc)
#define DAT_TOGGLE(name, T, desc)
#define DAT_FONTSIZE(name, T, desc) inst->name += delta;
#define DAT_PX(name, T, desc)
#define DAT_INT(name, T, desc)
#define DAT_PATH(name, T, desc)
#define DAT_FILE(name, T, desc)
#define DAT_STRING(name, T, desc)
#define DAT_RECT_I(name, T, desc)
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
#undef DAT_RECT_I
        need_save = true;
    }

    void update_string(String8* cfg_value, String8 value)
    {
        *cfg_value = str8_copy(cfg_arena, value);
    }

    bool load_config(String8 path, Feed::MessageFeed* feed)
    {
        PROF_SCOPE();
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String8 file_contents = str8_empty;
        TinyToml::ParseMsgList result_lst{};
        bool file_good = read_entire_file(scratch.arena, &file_contents, path);
        if (file_good)
        {
            TinyToml::Parser parser = TinyToml::make_parser(scratch.arena, file_contents);
            TinyToml::parser_parse(&parser);
            if (TinyToml::has_errors(parser))
            {
                // Emit errors.
                char buf[512];
                String8 msg = fmt_string(buf, "Failed to parse config file '%S'.", path);
                feed->queue_error(msg);
                for EachNode(n, parser.err_msgs.first)
                {
                    TinyToml::ErrorInternal* err = n->error.err;
                    String8View k = TinyToml::err_kind_string(err->kind);
                    msg = fmt_string(buf, "TOML ERROR: %.*s(%u:%u)", int(k.size), k.str, unsigned(err->locus.line), unsigned(err->locus.col));
                    feed->queue_error(msg);
                    file_good = false;
                }
            }
            else
            {
                result_lst = parser.parse_msgs;
            }
        }
        else
        {
            auto os_err = OS::last_os_error_code();
            String8 os_err_str = OS::format_error(scratch.arena, os_err);
            String8 msg = str8_fmt(scratch.arena, "Unable to read file '%S': %S", path, os_err_str);
            feed->queue_error(msg);
        }

        // Process messages.
        TinyToml::ParseMsg* msg = result_lst.first;
        if (msg != nullptr)
        {
            // Before we clear the config arena, we want to temporarily move all the owned allocations into the scratch arena
            // and then move them back to the config arena once we're done parsing.  The reason we do this is so that we can
            // retain default values of strings without having the written config entry (or if the user changes it through the
            // UI).
            move_owned_values_to(scratch.arena, &diff_colors_instance);
            move_owned_values_to(scratch.arena, &diff_state_instance);
            move_owned_values_to(scratch.arena, &feed_colors_instance);
            move_owned_values_to(scratch.arena, &feed_state_instance);
            move_owned_values_to(scratch.arena, &widget_colors_instance);
            move_owned_values_to(scratch.arena, &widget_state_instance);
            move_owned_values_to(scratch.arena, &system_core_instance);
            move_owned_values_to(scratch.arena, &system_fonts_instance);
            move_owned_values_to(scratch.arena, &system_effects_instance);
            move_owned_values_to(scratch.arena, &hotkey_state_instance);
            // Clear the config arena so we can start adding to it again.
            Arena::clear(cfg_arena);
            ConfigTableSet cfg_table = make_config_table_set(scratch.arena, 32);
            populate_loader(scratch.arena, &cfg_table, &diff_colors_instance);
            populate_loader(scratch.arena, &cfg_table, &diff_state_instance);
            populate_loader(scratch.arena, &cfg_table, &feed_colors_instance);
            populate_loader(scratch.arena, &cfg_table, &feed_state_instance);
            populate_loader(scratch.arena, &cfg_table, &widget_colors_instance);
            populate_loader(scratch.arena, &cfg_table, &widget_state_instance);
            populate_loader(scratch.arena, &cfg_table, &system_core_instance);
            populate_loader(scratch.arena, &cfg_table, &system_fonts_instance);
            populate_loader(scratch.arena, &cfg_table, &system_effects_instance);
            populate_loader(scratch.arena, &cfg_table, &hotkey_state_instance);
            do
            {
                using Kind = TinyToml::ParseMsg::Kind;
                switch (msg->kind)
                {
                case Kind::Table:
                    process_table_msg(scratch.arena, &cfg_table, msg, feed);
                    msg = skip_table(msg);
                    break;
                case Kind::ArrayOfTable:
                    msg = skip_table(msg);
                    break;
                case Kind::Fin:
                    msg = msg->next;
                    break;
                default:
                    {
                        String8 err_msg = str8_fmt(scratch.arena,
                            "Unexpected element '%S' at (%u:%u); ignored.",
                            TinyToml::msg_kind_string(msg->kind),
                            unsigned(msg->locus.line),
                            unsigned(msg->locus.col));
                        feed->queue_warning(err_msg);
                        msg = msg->next;
                    }
                    break;
                }
            } while (msg != nullptr);

            // Promote values into the config arena.
            move_owned_values_to(cfg_arena, &diff_colors_instance);
            move_owned_values_to(cfg_arena, &diff_state_instance);
            move_owned_values_to(cfg_arena, &feed_colors_instance);
            move_owned_values_to(cfg_arena, &feed_state_instance);
            move_owned_values_to(cfg_arena, &widget_colors_instance);
            move_owned_values_to(cfg_arena, &widget_state_instance);
            move_owned_values_to(cfg_arena, &system_core_instance);
            move_owned_values_to(cfg_arena, &system_fonts_instance);
            move_owned_values_to(cfg_arena, &system_effects_instance);
            move_owned_values_to(cfg_arena, &hotkey_state_instance);
        }
        Arena::scratch_end(scratch);

        populate_inverse_color_states();
        return file_good;
    }

    bool save_config(String8 path, Feed::MessageFeed* feed)
    {
        PROF_SCOPE();
        // Regardless of if the save is successful or not, we should tell the rest of the app that we do not need to save anymore.
        need_save = false;

        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        TomlSaveInput save_in{};
        save_in.arena = scratch.arena;
        str8_serial_begin(scratch.arena, &save_in.serial_lst);

        toml_save(&save_in, diff_colors_instance);
        toml_save_linebreak(&save_in);
        toml_save(&save_in, diff_state_instance);
        toml_save_linebreak(&save_in);
        toml_save(&save_in, feed_colors_instance);
        toml_save_linebreak(&save_in);
        toml_save(&save_in, feed_state_instance);
        toml_save_linebreak(&save_in);
        toml_save(&save_in, widget_colors_instance);
        toml_save_linebreak(&save_in);
        toml_save(&save_in, widget_state_instance);
        toml_save_linebreak(&save_in);
        toml_save(&save_in, system_core_instance);
        toml_save_linebreak(&save_in);
        toml_save(&save_in, system_fonts_instance);
        toml_save_linebreak(&save_in);
        toml_save(&save_in, system_effects_instance);
        toml_save_linebreak(&save_in);
        toml_save(&save_in, hotkey_state_instance);

        String8 result = str8_serial_end(scratch.arena, save_in.serial_lst);
        bool file_good = save_file(path, result);
        if (not file_good)
        {
            auto os_err = OS::last_os_error_code();
            String8 os_err_str = OS::format_error(scratch.arena, os_err);
            char fmt_buf[512];
            String8 txt = fmt_string(fmt_buf, "Failed to save to '%S': %S", path, os_err_str);
            feed->queue_info(txt);
            file_good = false;
        }
        Arena::scratch_end(scratch);
        return file_good;
    }

    void allow_computed_colors(bool allow)
    {
        if (not allow)
        {
            setup_colors(UseComputedColors::No);
        }
        else
        {
            setup_colors(make_yes_no<UseComputedColors>(system_effects().light_mode));
        }
    }
} // namespace Config
