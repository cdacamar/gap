#include "hotkeys.h"

#include <algorithm>

#include "feed.h"
#include "gap-bits.h"
#include "gap-strings.h"
#include "os.h"
#include "tiny-toml.h"
#include "util.h"

namespace Hotkeys
{
    namespace
    {
        // There's an invariant here that if custom hotkey is active, the other is 0
        // and vice-versa.
        struct HotkeyBindingNode
        {
            HotkeyBindingNode* next;
            CustomHotkeyID cust_hk_id;
            Hotkey hk;
            HotkeyCombo binding;
        };

        struct HotkeyBindingEntry
        {
            HotkeyBindingNode* group[count_of<CustomHotkeyGroup>];
        };

        struct HotkeyBindingMap
        {
            HotkeyBindingEntry** buckets;
            HotkeyBindingNode* free_list;
            uint64_t capacity;
            uint64_t count;
            uint64_t nil_buckets;
            uint64_t load;
        };

        // Globals.
        Arena::Arena* hk_arena = nullptr;
        Arena::Arena* hk_cust_strings_arena = nullptr;
        HotkeyEntry hotkeys[count_of<Hotkey>];
        HotkeyEntry defaults[count_of<Hotkey>];
        CustomHotkeyList custom_hotkeys[count_of<CustomHotkeyGroup>];
        HotkeyBindingMap binding_map;
        bool need_save = false;

        CustomHotkeyList* custom_list_for_group_ref(CustomHotkeyGroup g)
        {
            return &custom_hotkeys[rep(g)];
        }

        uint64_t combine_binding(HotkeyCombo combo)
        {
            constexpr uint64_t key_width = std::bit_width(count_of<OS::Key>);
            uint64_t result = rep(combo.key) | (static_cast<uint64_t>(rep(combo.mods)) << key_width);
            return result;
        }

        HotkeyBindingMap make_hk_binding_map(Arena::Arena* arena)
        {
            HotkeyBindingMap result = {};
            const uint64_t pow_2_aligned_size = up_pow2(KB(1));
            result.capacity = pow_2_aligned_size;
            result.nil_buckets = result.capacity;
            result.buckets = Arena::push_array<HotkeyBindingEntry*>(arena, result.capacity);
            return result;
        }

        HotkeyBindingNode* map_hk_binding(Arena::Arena* arena, HotkeyBindingMap* map, CustomHotkeyGroup group, HotkeyBindingNode entry)
        {
            uint64_t idx = (map->capacity - 1) & combine_binding(entry.binding);
            HotkeyBindingEntry* group_slot = map->buckets[idx];

            if (group_slot == nullptr)
            {
                // Create the group.
                group_slot = Arena::push_array<HotkeyBindingEntry>(arena, 1);
                map->buckets[idx] = group_slot;
            }
            HotkeyBindingNode* slot = group_slot->group[rep(group)];

            if (slot == nullptr)
            {
                if (map->free_list != nullptr)
                {
                    slot = map->free_list;
                    SLLStackPop(map->free_list);
                    zero_bytes(slot);
                }
                else
                {
                    slot = Arena::push_array<HotkeyBindingNode>(arena, 1);
                }
                *slot = entry;
                // Insert into group.
                group_slot->group[rep(group)] = slot;
                ++map->count;
                --map->nil_buckets;
                map->load = std::max(map->load, uint64_t(1));
            }
            else
            {
                uint64_t elms = 0;
                for EachNode(n, slot)
                {
                    ++elms;
                }
                HotkeyBindingNode* new_entry = nullptr;
                if (map->free_list != nullptr)
                {
                    new_entry = map->free_list;
                    SLLStackPop(map->free_list);
                    zero_bytes(new_entry);
                }
                else
                {
                    new_entry = Arena::push_array<HotkeyBindingNode>(arena, 1);
                }
                *new_entry = entry;

                // Add the new entry to the front and append the rest.
                group_slot->group[rep(group)] = new_entry;
                new_entry->next = slot;
                ++map->count;
                elms += 1;
                map->load = std::max(map->load, elms);

                slot = new_entry;
            }
            return slot;
        }

        HotkeyBindingNode* fetch_hk_binding(HotkeyBindingMap* map, CustomHotkeyGroup group, HotkeyCombo binding)
        {
            HotkeyBindingNode* result = nullptr;
            uint64_t idx = (map->capacity - 1) & combine_binding(binding);
            HotkeyBindingEntry* group_slot = map->buckets[idx];

            HotkeyBindingNode* slot = nullptr;
            if (group_slot != nullptr)
            {
                slot = group_slot->group[rep(group)];
            }

            for EachNode(n, slot)
            {
                if (n->binding == binding)
                {
                    result = n;
                    break;
                }
            }
            return result;
        }

        void fill_binding_matches_hk_binding(Arena::Arena* arena, HotkeyBindingMap* map, HotkeyMatchList* lst, HotkeyCombo binding)
        {
            uint64_t idx = (map->capacity - 1) & combine_binding(binding);
            HotkeyBindingEntry* group_slot = map->buckets[idx];

            if (group_slot != nullptr)
            {
                for EachIndex(g, count_of<CustomHotkeyGroup>)
                {
                    for EachNode(n, group_slot->group[g])
                    {
                        if (n->binding == binding)
                        {
                            HotkeyMatchNode* node = Arena::push_array<HotkeyMatchNode>(arena, 1);
                            node->group = static_cast<CustomHotkeyGroup>(g);
                            node->match = { .hk = n->hk, .cust_hk_id = n->cust_hk_id };
                            SLLQueuePush(lst->first, lst->last, node);
                            ++lst->count;
                        }
                    }
                }
            }
        }

        void unmap_hk_binding(HotkeyBindingMap* map, CustomHotkeyGroup group, HotkeyBindingNode target)
        {
            uint64_t idx = (map->capacity - 1) & combine_binding(target.binding);
            HotkeyBindingEntry* group_slot = map->buckets[idx];

            HotkeyBindingNode* slot = nullptr;
            if (group_slot != nullptr)
            {
                slot = group_slot->group[rep(group)];
            }

            HotkeyBindingNode* prev_slot = nullptr;
            while (slot != nullptr)
            {
                if (slot->binding == target.binding
                    and (slot->cust_hk_id == target.cust_hk_id
                        and slot->hk == target.hk))
                {
                    break;
                }
                prev_slot = slot;
                slot = slot->next;
            }

            if (slot != nullptr)
            {
                HotkeyBindingNode* removed = slot;
                SLLStackPop(slot);
                if (prev_slot != nullptr)
                {
                    prev_slot->next = slot;
                }
                else
                {
                    group_slot->group[rep(group)] = slot;
                }
                --map->count;
                removed->next = nullptr;
                SLLStackPush(map->free_list, removed);
            }
        }

        constexpr CustomHotkeyGroup group_for_hotkey(Hotkey hk)
        {
            switch (hk)
            {
#define DAT_CAT_START(e, grp, cat, desc)
#define DAT_CMD(c, k, m, desc) case Hotkey::c:
#define DAT_CAT_END(e, grp) return CustomHotkeyGroup:: grp;
#include "hotkeys.dat"
#undef DAT_CAT_START
#undef DAT_CMD
#undef DAT_CAT_END
            }
            return CustomHotkeyGroup::ED;
        }

        void remap_binding(Arena::Arena* arena,
                            HotkeyBindingMap* map,
                            CustomHotkeyGroup group,
                            HotkeyBindingNode node,
                            HotkeyCombo new_binding)
        {
            unmap_hk_binding(map, group, node);
            node.binding = new_binding;
            map_hk_binding(arena, map, group, node);
        }

        void rebind_no_compute(Hotkey hk, OS::Key k, OS::KeyMods mods)
        {
            HotkeyEntry* entry = &hotkeys[rep(hk)];
            entry->key = k;
            entry->mods = mods;
        }

        void rebind_and_remap(Arena::Arena* arena, HotkeyBindingMap* map, CustomHotkeyID id, OS::Key k, OS::KeyMods mods)
        {
            // We need to identify which list this hotkey belongs.
            CustomHotkeyEntry* entry = nullptr;
            CustomHotkeyGroup group = {};
            for EachIndex(g, count_of<CustomHotkeyGroup>)
            {
                CustomHotkeyList* lst = custom_list_for_group_ref(static_cast<CustomHotkeyGroup>(g));
                for EachNode(n, lst->first)
                {
                    if (n->id == id)
                    {
                        entry = n;
                        group = static_cast<CustomHotkeyGroup>(g);
                        break;
                    }
                }

                if (entry != nullptr)
                    break;
            }

            if (entry != nullptr)
            {
                HotkeyBindingNode node = {
                    .cust_hk_id = entry->id,
                    .hk = Hotkey::None,
                    .binding = { .key = entry->key, .mods = entry->mods }
                };
                remap_binding(arena, map, group, node, { .key = k, .mods = mods });
                entry->key = k;
                entry->mods = mods;
            }
        }

        CustomHotkeyID info_as_id(CustomHotkeyInfo* e)
        {
            return CustomHotkeyID{ reinterpret_cast<PrimitiveType<CustomHotkeyID>>(e) };
        }

        CustomHotkeyInfo* id_as_info(CustomHotkeyID id)
        {
            return reinterpret_cast<CustomHotkeyInfo*>(rep(id));
        }

        struct HotkeyArenaPair
        {
            Arena::Arena* arena;
            Arena::Arena* strings_arena;
        };

        CustomHotkeyEntry* push_custom_hotkey(HotkeyArenaPair arenas, CustomHotkeyList* lst, OS::Key k, OS::KeyMods mods, String8 fn)
        {
            CustomHotkeyEntry* entry = Arena::push_array<CustomHotkeyEntry>(arenas.arena, 1);
            CustomHotkeyInfo* info = Arena::push_array<CustomHotkeyInfo>(arenas.arena, 1);
            info->fn_name = str8_copy(arenas.strings_arena, fn);
            entry->key = k;
            entry->mods = mods;
            entry->id = info_as_id(info);
            SLLQueuePush(lst->first, lst->last, entry);
            ++lst->count;
            return entry;
        }

        ENABLE_UNHANDLED_CASE_WARNING()
        constexpr String8View key_to_human(OS::Key k)
        {
            using Key = OS::Key;
            switch (k)
            {
            case Key::Null:
                return str8_literal("Null");
            case Key::Esc:
                return str8_literal("Esc");
            case Key::F1:
                return str8_literal("F1");
            case Key::F2:
                return str8_literal("F2");
            case Key::F3:
                return str8_literal("F3");
            case Key::F4:
                return str8_literal("F4");
            case Key::F5:
                return str8_literal("F5");
            case Key::F6:
                return str8_literal("F6");
            case Key::F7:
                return str8_literal("F7");
            case Key::F8:
                return str8_literal("F8");
            case Key::F9:
                return str8_literal("F9");
            case Key::F10:
                return str8_literal("F10");
            case Key::F11:
                return str8_literal("F11");
            case Key::F12:
                return str8_literal("F12");
            case Key::F13:
                return str8_literal("F13");
            case Key::F14:
                return str8_literal("F14");
            case Key::F15:
                return str8_literal("F15");
            case Key::F16:
                return str8_literal("F16");
            case Key::F17:
                return str8_literal("F17");
            case Key::F18:
                return str8_literal("F18");
            case Key::F19:
                return str8_literal("F19");
            case Key::F20:
                return str8_literal("F20");
            case Key::F21:
                return str8_literal("F21");
            case Key::F22:
                return str8_literal("F22");
            case Key::F23:
                return str8_literal("F23");
            case Key::F24:
                return str8_literal("F24");
            case Key::Tick:
                return str8_literal("TODO: Tick");
            case Key::_0:
                return str8_literal("0");
            case Key::_1:
                return str8_literal("1");
            case Key::_2:
                return str8_literal("2");
            case Key::_3:
                return str8_literal("3");
            case Key::_4:
                return str8_literal("4");
            case Key::_5:
                return str8_literal("5");
            case Key::_6:
                return str8_literal("6");
            case Key::_7:
                return str8_literal("7");
            case Key::_8:
                return str8_literal("8");
            case Key::_9:
                return str8_literal("9");
            case Key::Minus:
                return str8_literal("-");
            case Key::Equal:
                return str8_literal("=");
            case Key::Backspace:
                return str8_literal("Backspace");
            case Key::Tab:
                return str8_literal("Tab");
            case Key::Q:
                return str8_literal("Q");
            case Key::W:
                return str8_literal("W");
            case Key::E:
                return str8_literal("E");
            case Key::R:
                return str8_literal("R");
            case Key::T:
                return str8_literal("T");
            case Key::Y:
                return str8_literal("Y");
            case Key::U:
                return str8_literal("U");
            case Key::I:
                return str8_literal("I");
            case Key::O:
                return str8_literal("O");
            case Key::P:
                return str8_literal("P");
            case Key::LeftBracket:
                return str8_literal("[");
            case Key::RightBracket:
                return str8_literal("]");
            case Key::BackSlash:
                return str8_literal("\\");
            case Key::CapsLock:
                return str8_literal("Caps Lock");
            case Key::A:
                return str8_literal("A");
            case Key::S:
                return str8_literal("S");
            case Key::D:
                return str8_literal("D");
            case Key::F:
                return str8_literal("F");
            case Key::G:
                return str8_literal("G");
            case Key::H:
                return str8_literal("H");
            case Key::J:
                return str8_literal("J");
            case Key::K:
                return str8_literal("K");
            case Key::L:
                return str8_literal("L");
            case Key::Semicolon:
                return str8_literal(";");
            case Key::Quote:
                return str8_literal("'");
            case Key::Return:
                return str8_literal("Return");
            case Key::Shift:
                return str8_literal("Shift");
            case Key::Z:
                return str8_literal("Z");
            case Key::X:
                return str8_literal("X");
            case Key::C:
                return str8_literal("C");
            case Key::V:
                return str8_literal("V");
            case Key::B:
                return str8_literal("B");
            case Key::N:
                return str8_literal("N");
            case Key::M:
                return str8_literal("M");
            case Key::Comma:
                return str8_literal(",");
            case Key::Period:
                return str8_literal(".");
            case Key::Slash:
                return str8_literal("/");
            case Key::Ctrl:
                return str8_literal("Ctrl");
            case Key::Alt:
                return str8_literal("Alt");
            case Key::Space:
                return str8_literal("Spacebar");
            case Key::Menu:
                return str8_literal("Menu");
            case Key::ScrollLock:
                return str8_literal("Scroll Lock");
            case Key::Pause:
                return str8_literal("Pause");
            case Key::Insert:
                return str8_literal("Insert");
            case Key::Home:
                return str8_literal("Home");
            case Key::PageUp:
                return str8_literal("PgUp");
            case Key::Delete:
                return str8_literal("Delete");
            case Key::End:
                return str8_literal("End");
            case Key::PageDown:
                return str8_literal("PgDn");
            case Key::Up:
                return str8_literal("Up");
            case Key::Left:
                return str8_literal("Left");
            case Key::Down:
                return str8_literal("Down");
            case Key::Right:
                return str8_literal("Right");
            case Key::Ex0:
                return str8_literal("TODO: Ex0");
            case Key::Ex1:
                return str8_literal("TODO: Ex1");
            case Key::Ex2:
                return str8_literal("TODO: Ex2");
            case Key::Ex3:
                return str8_literal("TODO: Ex3");
            case Key::Ex4:
                return str8_literal("TODO: Ex4");
            case Key::Ex5:
                return str8_literal("TODO: Ex5");
            case Key::Ex6:
                return str8_literal("TODO: Ex6");
            case Key::Ex7:
                return str8_literal("TODO: Ex7");
            case Key::Ex8:
                return str8_literal("TODO: Ex8");
            case Key::Ex9:
                return str8_literal("TODO: Ex9");
            case Key::Ex10:
                return str8_literal("TODO: Ex10");
            case Key::Ex11:
                return str8_literal("TODO: Ex11");
            case Key::Ex12:
                return str8_literal("TODO: Ex12");
            case Key::Ex13:
                return str8_literal("TODO: Ex13");
            case Key::Ex14:
                return str8_literal("TODO: Ex14");
            case Key::Ex15:
                return str8_literal("TODO: Ex15");
            case Key::Ex16:
                return str8_literal("TODO: Ex16");
            case Key::Ex17:
                return str8_literal("TODO: Ex17");
            case Key::Ex18:
                return str8_literal("TODO: Ex18");
            case Key::Ex19:
                return str8_literal("TODO: Ex19");
            case Key::Ex20:
                return str8_literal("TODO: Ex20");
            case Key::Ex21:
                return str8_literal("TODO: Ex21");
            case Key::Ex22:
                return str8_literal("TODO: Ex22");
            case Key::Ex23:
                return str8_literal("TODO: Ex23");
            case Key::Ex24:
                return str8_literal("TODO: Ex24");
            case Key::Ex25:
                return str8_literal("TODO: Ex25");
            case Key::Ex26:
                return str8_literal("TODO: Ex26");
            case Key::Ex27:
                return str8_literal("TODO: Ex27");
            case Key::Ex28:
                return str8_literal("TODO: Ex28");
            case Key::Ex29:
                return str8_literal("TODO: Ex29");
            case Key::NumLock:
                return str8_literal("Num Lock");
            case Key::NumSlash:
                return str8_literal("Num/");
            case Key::NumStar:
                return str8_literal("Num*");
            case Key::NumMinus:
                return str8_literal("Num-");
            case Key::NumPlus:
                return str8_literal("Num+");
            case Key::NumPeriod:
                return str8_literal("Num.");
            case Key::Num0:
                return str8_literal("Num0");
            case Key::Num1:
                return str8_literal("Num1");
            case Key::Num2:
                return str8_literal("Num2");
            case Key::Num3:
                return str8_literal("Num3");
            case Key::Num4:
                return str8_literal("Num4");
            case Key::Num5:
                return str8_literal("Num5");
            case Key::Num6:
                return str8_literal("Num6");
            case Key::Num7:
                return str8_literal("Num7");
            case Key::Num8:
                return str8_literal("Num8");
            case Key::Num9:
                return str8_literal("Num9");
            case Key::LeftMouseButton:
                return str8_literal("Left Mouse");
            case Key::MiddleMouseButton:
                return str8_literal("Middle Mouse");
            case Key::RightMouseButton:
                return str8_literal("Right Mouse");
            case Key::Command:
                return str8_literal("Cmd");
            case Key::Count:
                return str8_literal("TODO: Count");
            }
            return str8_literal("Null");
        }

        constexpr String8View custom_hotkey_group_name(CustomHotkeyGroup g)
        {
            switch (g)
            {
            case CustomHotkeyGroup::GLB:
                return str8_literal("GLB");
            case CustomHotkeyGroup::ED:
                return str8_literal("ED");
            case CustomHotkeyGroup::FILE_EXP:
                return str8_literal("FILE_EXP");
            case CustomHotkeyGroup::FIND:
                return str8_literal("FIND");
            case CustomHotkeyGroup::SMCLI:
                return str8_literal("SMCLI");
            case CustomHotkeyGroup::Count:
                return str8_literal("TODO: Count");
            }
            return str8_literal("");
        }
        DISABLE_UNHANDLED_CASE_WARNING()

        String8View hotkey_name(Hotkey hk)
        {
            switch (hk)
            {
#define DAT_CAT_START(e, grp, cat, desc)
#define DAT_CMD(c, k, m, desc) case Hotkey::c: return str8_literal(#c);
#define DAT_CAT_END(e, grp)
#include "hotkeys.dat"
#undef DAT_CAT_START
#undef DAT_CMD
#undef DAT_CAT_END
            }
            return str8_literal("");
        }

        Hotkey hotkey_for_name(String8 name)
        {
            struct NameHotkey
            {
                String8View name;
                Hotkey hk;
            };

            constexpr NameHotkey names[] =
            {
#define DAT_CAT_START(e, grp, cat, desc)
#define DAT_CMD(c, k, m, desc) { str8_literal(#c), Hotkey::c },
#define DAT_CAT_END(e, grp)
#include "hotkeys.dat"
#undef DAT_CAT_START
#undef DAT_CMD
#undef DAT_CAT_END
            };
            for (const NameHotkey& nk : names)
            {
                if (str8_match_exact(str8_mut(nk.name), name))
                    return nk.hk;
            }
            return Hotkey::None;
        }

        struct HumanKey
        {
            std::string_view name;
            OS::Key key;
        };

        struct HumanKeys
        {
            HumanKey keys[count_of<OS::Key>];
        };

        constexpr HumanKeys sorted_human_keys()
        {
            HumanKeys keys{};
            OS::Key key{};
            for (auto& e : keys.keys)
            {
                String8View k = key_to_human(key);
                e.name = { k.str, k.size };
                e.key = key;
                key = extend(key);
            }
            std::sort(std::begin(keys.keys),
                        std::end(keys.keys),
                        [](const auto& lhs, const auto& rhs)
                        {
                            return lhs.name < rhs.name;
                        });
            return keys;
        }

        OS::Key key_for_human(std::string_view name)
        {
            static constexpr HumanKeys keys = sorted_human_keys();
            auto found = std::lower_bound(std::begin(keys.keys),
                                            std::end(keys.keys),
                                            name,
                                            [](const auto& key, std::string_view name)
                                            {
                                                return key.name < name;
                                            });
            if (found == std::end(keys.keys))
                return OS::Key::Null;
            if (found->name != name)
                return OS::Key::Null;
            return found->key;
        }

        void populate_hotkey_bindings(Arena::Arena* arena, HotkeyBindingMap* map)
        {
            *map = make_hk_binding_map(arena);
            HotkeyCollection hkc{};
            HotkeyBindingNode node = {};
#define DAT_CAT_START(e, grp, cat, desc)                               \
 hkc = cat ## _hotkeys();                                              \
 for (;hkc.first != hkc.last; ++hkc.first) {                           \
  const HotkeyEntry* hk = hkc.first;                                   \
  node = { .cust_hk_id = CustomHotkeyID::None,                         \
            .hk = hk->cmd,                                             \
            .binding = { .key = hk->key, .mods = hk->mods } };         \
  map_hk_binding(arena, map, CustomHotkeyGroup:: grp, node);           \
 }
#define DAT_CMD(c, k, m, desc)
#define DAT_CAT_END(e, grp)
#include "hotkeys.dat"
#undef DAT_CAT_START
#undef DAT_CMD
#undef DAT_CAT_END

            // Now custom hotkeys.
            for EachIndex(g, count_of<CustomHotkeyGroup>)
            {
                CustomHotkeyList* lst = custom_list_for_group_ref(static_cast<CustomHotkeyGroup>(g));
                for EachNode(n, lst->first)
                {
                    node = { .cust_hk_id = n->id,
                                .hk = Hotkey::None,
                                .binding = { .key = n->key, .mods = n->mods } };
                    map_hk_binding(arena, map, static_cast<CustomHotkeyGroup>(g), node);
                }
            }
        }

        // TODO: Emit warning message saying that an entry was skipped and why.
        TinyToml::ParseMsg* fill_hotkey_entry(TinyToml::ParseMsg* table_root)
        {
            // We're looking for 'name', 'key', and 'mods'.
            // Skip the root and move to entries.
            TinyToml::ParseMsg* msg = table_root->next;
            enum HKVals { Name, Key, Mods, Count };
            TinyToml::ParseMsg* hk_vals[Count] = {};
            // Eh, just take the first one that's set and don't validate duplicate keys.
            while (msg != nullptr and msg->kind == TinyToml::ParseMsg::Kind::Pair)
            {
                if (TinyToml::bare_key_matches(msg->pair.key, str8_mut(str8_literal("name"))))
                {
                    hk_vals[Name] = msg->pair.val;
                }
                else if (TinyToml::bare_key_matches(msg->pair.key, str8_mut(str8_literal("key"))))
                {
                    hk_vals[Key] = msg->pair.val;
                }
                else if (TinyToml::bare_key_matches(msg->pair.key, str8_mut(str8_literal("mods"))))
                {
                    hk_vals[Mods] = msg->pair.val;
                }
                msg = msg->next;
            }

            Hotkey hk = Hotkey::None;
            OS::Key key = OS::Key::Null;
            OS::KeyMods mods = OS::KeyMods::None;
            bool good = hk_vals[Name] != nullptr
                            and hk_vals[Key] != nullptr
                            and hk_vals[Mods] != nullptr;
            if (good)
            {
                good = TinyToml::any_string(hk_vals[Name]);
                if (good)
                {
                    hk = hotkey_for_name(TinyToml::string_val(hk_vals[Name]));
                    good = good and hk != Hotkey::None;
                }
                good = good and TinyToml::any_string(hk_vals[Key]);
                if (good)
                {
                    key = key_for_human(sv_str8(TinyToml::string_val(hk_vals[Key])));
                }
                good = good and hk_vals[Mods]->kind == TinyToml::ParseMsg::Kind::Array;
                if (good)
                {
                    for EachNode(n, hk_vals[Mods]->array.first)
                    {
                        good = good and TinyToml::any_string(n->data);
                        if (not good)
                            break;
                        auto mod_key = key_for_human(sv_str8(TinyToml::string_val(n->data)));
                        switch (mod_key)
                        {
                        case OS::Key::Ctrl:
                            mods |= OS::KeyMods::Ctrl;
                            break;
                        case OS::Key::Alt:
                            mods |= OS::KeyMods::Alt;
                            break;
                        case OS::Key::Shift:
                            mods |= OS::KeyMods::Shift;
                            break;
                        case OS::Key::Command:
                            mods |= OS::KeyMods::Cmd;
                            break;
                        default:
                            good = false;
                            break;
                        }
                    }
                }
            }

            if (good)
            {
                // Finally, we can assemble the hotkey and set it.
                rebind_no_compute(hk, key, mods);
            }
            return msg;
        }

        struct KnownCustomGroups
        {
            String8View name;
            CustomHotkeyList* list;
        };

        constexpr KnownCustomGroups known_custom_groups[] =
        {
            { .name = str8_literal("GLB"), .list = &custom_hotkeys[rep(CustomHotkeyGroup::GLB)] },
            { .name = str8_literal("ED"), .list = &custom_hotkeys[rep(CustomHotkeyGroup::ED)] },
            { .name = str8_literal("FILE_EXP"), .list = &custom_hotkeys[rep(CustomHotkeyGroup::FILE_EXP)] },
            { .name = str8_literal("FIND"), .list = &custom_hotkeys[rep(CustomHotkeyGroup::FIND)] },
            { .name = str8_literal("SMCLI"), .list = &custom_hotkeys[rep(CustomHotkeyGroup::SMCLI)] },
        };

        static_assert(std::size(known_custom_groups) == count_of<CustomHotkeyGroup>);

        CustomHotkeyList* custom_list_for_group(String8 group)
        {
            for (const KnownCustomGroups& grp : known_custom_groups)
            {
                if (str8_match_exact(group, str8_mut(grp.name)))
                    return grp.list;
            }
            return nullptr;
        }

        TinyToml::ParseMsg* fill_custom_hotkey_entry(HotkeyArenaPair arenas, TinyToml::ParseMsg* table_root, Feed::MessageFeed* feed)
        {
            // We're looking for 'group', 'fn', 'key', and 'mods'.
            // Skip the root and move to entries.
            TinyToml::ParseMsg* msg = table_root->next;
            enum HKVals { Group, Fn, Key, Mods, Count };
            struct HKVal
            {
                TinyToml::ParseMsg* msg;
                bool good;
            };
            HKVal hk_vals[Count] = {};
            // Eh, just take the first one that's set and don't validate duplicate keys.
            while (msg != nullptr and msg->kind == TinyToml::ParseMsg::Kind::Pair)
            {
                if (TinyToml::bare_key_matches(msg->pair.key, str8_mut(str8_literal("group"))))
                {
                    hk_vals[Group].msg = msg->pair.val;
                    hk_vals[Group].good = true;
                }
                else if (TinyToml::bare_key_matches(msg->pair.key, str8_mut(str8_literal("fn"))))
                {
                    hk_vals[Fn].msg = msg->pair.val;
                    hk_vals[Fn].good = true;
                }
                else if (TinyToml::bare_key_matches(msg->pair.key, str8_mut(str8_literal("key"))))
                {
                    hk_vals[Key].msg = msg->pair.val;
                    hk_vals[Key].good = true;
                }
                else if (TinyToml::bare_key_matches(msg->pair.key, str8_mut(str8_literal("mods"))))
                {
                    hk_vals[Mods].msg = msg->pair.val;
                    hk_vals[Mods].good = true;
                }
                msg = msg->next;
            }

            CustomHotkeyList* list = nullptr;
            String8 fn = str8_empty;
            OS::Key key = OS::Key::Null;
            OS::KeyMods mods = OS::KeyMods::None;
            hk_vals[Group].good = hk_vals[Group].good and TinyToml::any_string(hk_vals[Group].msg);
            if (hk_vals[Group].good)
            {
                list = custom_list_for_group(TinyToml::string_val(hk_vals[Group].msg));
                hk_vals[Group].good = list != nullptr;
            }
            hk_vals[Fn].good = hk_vals[Fn].good and TinyToml::any_string(hk_vals[Fn].msg);
            if (hk_vals[Fn].good)
            {
                fn = TinyToml::string_val(hk_vals[Fn].msg);
                hk_vals[Fn].good = fn.size != 0;
            }
            hk_vals[Key].good = hk_vals[Key].good and TinyToml::any_string(hk_vals[Key].msg);
            if (hk_vals[Key].good)
            {
                key = key_for_human(sv_str8(TinyToml::string_val(hk_vals[Key].msg)));
            }
            hk_vals[Mods].good = hk_vals[Mods].good and hk_vals[Mods].msg->kind == TinyToml::ParseMsg::Kind::Array;
            if (hk_vals[Mods].good)
            {
                for EachNode(n, hk_vals[Mods].msg->array.first)
                {
                    hk_vals[Mods].good = hk_vals[Mods].good and TinyToml::any_string(n->data);
                    if (not hk_vals[Mods].good)
                        break;
                    auto mod_key = key_for_human(sv_str8(TinyToml::string_val(n->data)));
                    switch (mod_key)
                    {
                    case OS::Key::Ctrl:
                        mods |= OS::KeyMods::Ctrl;
                        break;
                    case OS::Key::Command:
                        mods |= OS::KeyMods::Cmd;
                        break;
                    case OS::Key::Alt:
                        mods |= OS::KeyMods::Alt;
                        break;
                    case OS::Key::Shift:
                        mods |= OS::KeyMods::Shift;
                        break;
                    default:
                        hk_vals[Mods].good = false;
                        break;
                    }
                }
            }

            bool good = hk_vals[Group].good
                        and hk_vals[Fn].good
                        and hk_vals[Key].good
                        and hk_vals[Mods].good;
            if (good)
            {
                push_custom_hotkey(arenas, list, key, mods, fn);
            }
            else
            {
                auto scratch = Arena::scratch_begin({ &arenas.arena, 1 });
                if (list == nullptr)
                {
                    constexpr uint64_t known_cnt = std::size(known_custom_groups);
                    String8Node nodes[known_cnt];
                    String8List str_lst{};
                    for EachIndex(i, known_cnt)
                    {
                        nodes[i].next = nullptr;
                        nodes[i].string = str8_mut(known_custom_groups[i].name);
                        str8_list_push_node(&str_lst, &nodes[i]);
                    }
                    JoinStringsInput in = {
                        .strings = str_lst,
                        .sep = str8_mut(str8_literal(",")),
                    };
                    String8 joined = join_strings(scratch.arena, in);
                    String8 feed_msg;
                    if (hk_vals[Group].msg != nullptr)
                    {
                        auto locus = hk_vals[Group].msg->locus;
                        feed_msg = str8_fmt(scratch.arena, "ERROR(%u:%u): Invalid group name or type; ignoring entry.  Try any of: %S.", unsigned(locus.line), unsigned(locus.col), joined);
                    }
                    else
                    {
                        feed_msg = str8_fmt(scratch.arena, "ERROR(%u:%u): Custom bindings require a 'group' field; ignoring.  Hint: try any of: %S.", unsigned(table_root->locus.line), unsigned(table_root->locus.col), joined);
                    }
                    feed->queue_warning(feed_msg);
                }

                if (fn.size == 0)
                {
                    String8 feed_msg;
                    if (hk_vals[Fn].msg != nullptr)
                    {
                        auto locus = hk_vals[Fn].msg->locus;
                        feed_msg = str8_fmt(scratch.arena, "ERROR(%u:%u): The target function for a custom bindings cannot be empty; ignoring.", unsigned(locus.line), unsigned(locus.col));
                    }
                    else
                    {
                        feed_msg = str8_fmt(scratch.arena, "ERROR(%u:%u): A target function 'fn' must be provided for custom bindings; ignoring.", unsigned(table_root->locus.line), unsigned(table_root->locus.col));
                    }
                    feed->queue_warning(feed_msg);
                }

                if (key == OS::Key::Null)
                {
                    if (hk_vals[Key].msg != nullptr)
                    {
                        auto locus = hk_vals[Key].msg->locus;
                        String8 feed_msg = str8_fmt(scratch.arena, "WARNING(%u:%u): The key for a custom binding is null and unreachable from any keys.", unsigned(locus.line), unsigned(locus.col));
                        feed->queue_warning(feed_msg);
                    }
                }

                if (not hk_vals[Mods].good)
                {
                    String8 feed_msg;
                    if (hk_vals[Mods].msg != nullptr)
                    {
                        auto locus = hk_vals[Mods].msg->locus;
                        feed_msg = str8_fmt(scratch.arena, "ERROR(%u:%u): The mods array was ill-formed; ignoring.", unsigned(locus.line), unsigned(locus.col));
                    }
                    else
                    {
                        feed_msg = str8_fmt(scratch.arena, "ERROR(%u:%u): A custom binding must provide a 'mods' array; ignoring.", unsigned(table_root->locus.line), unsigned(table_root->locus.col));
                    }
                    feed->queue_warning(feed_msg);
                }
                Arena::scratch_end(scratch);
            }
            return msg;
        }
    } // namespace [anon]

    void init()
    {
        hk_arena = Arena::alloc(Arena::default_params);
        hk_cust_strings_arena = Arena::alloc(Arena::default_params);
#define DAT_CAT_START(e, grp, cat, desc)
#define DAT_CMD(c, k, m, desc) hotkeys[rep(Hotkey:: c)] = { .cmd = Hotkey::c, .key = k, .mods = m };
#define DAT_CAT_END(e, grp)
#include "hotkeys.dat"
#undef DAT_CAT_START
#undef DAT_CMD
#undef DAT_CAT_END
        memcpy(defaults, hotkeys, sizeof(hotkeys[0]) * std::size(hotkeys));
        populate_hotkey_bindings(hk_arena, &binding_map);
    }

    // The definitions are:
    // [[binding]]
    // name = 'foo'
    // key = 'key'
    // mods = ['ctrl', 'shift']
    //
    // There are also custom hotkey definitions:
    // [[binding.custom]]
    // group = 'group'
    // fn = 'my_handler'
    // key = 'key'
    // mods = ['ctrl', 'alt']
    // ...
    bool load(String8 path, Feed::MessageFeed* feed)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String8 file_contents = str8_empty;
        bool file_good = read_entire_file(scratch.arena, &file_contents, path);
        TinyToml::ParseMsgList result_lst{};
        if (file_good)
        {
            TinyToml::Parser parser = TinyToml::make_parser(scratch.arena, file_contents);
            TinyToml::parser_parse(&parser);
            if (TinyToml::has_errors(parser))
            {
                // Emit errors.
                char buf[512];
                for EachNode(n, parser.err_msgs.first)
                {
                    TinyToml::ErrorInternal* err = n->error.err;
                    String8View k = TinyToml::err_kind_string(err->kind);
                    String8 msg = fmt_string(buf, "TOML ERROR: %.*s(%u:%u)", int(k.size), k.str, unsigned(err->locus.line), unsigned(err->locus.col));
                    feed->queue_error(msg);
                    file_good = false;
                }
            }
            else
            {
                // Reset custom bindings so we can parse new ones.
                for (CustomHotkeyList& lst : custom_hotkeys)
                {
                    lst = {};
                }
                Arena::clear(hk_arena);
                Arena::clear(hk_cust_strings_arena);
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
        HotkeyArenaPair hk_arenas = {
            .arena = hk_arena,
            .strings_arena = hk_cust_strings_arena,
        };
        while (msg != nullptr)
        {
            bool good = false;
            if (msg->kind == TinyToml::ParseMsg::Kind::ArrayOfTable)
            {
                if (TinyToml::bare_key_matches(msg->array_of_table.key, str8_mut(str8_literal("binding"))))
                {
                    msg = fill_hotkey_entry(msg);
                    good = true;
                }
                else if (TinyToml::dotted_key_matches(msg->array_of_table.key, str8_mut(str8_literal("binding.custom"))))
                {
                    msg = fill_custom_hotkey_entry(hk_arenas, msg, feed);
                    good = true;
                }
            }

            if (not good)
            {
                msg = msg->next;
            }
        }

        if (file_good)
        {
            populate_hotkey_bindings(hk_arena, &binding_map);
        }
        Arena::scratch_end(scratch);
        return file_good;
    }

    bool save(String8 path, Feed::MessageFeed* feed)
    {
        // Regardless of if the save is successful or not, we should tell the rest of the app that we do not need to save anymore.
        need_save = false;
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Start a serialization.
        String8List serial_lst{};
        str8_serial_begin(scratch.arena, &serial_lst);
        char buf[512];
        bool first = true;
        for EachIndex(i, count_of<Hotkey>)
        {
            HotkeyEntry* e = &hotkeys[i];
            if (e->key == OS::Key::Null)
                continue;
            if (not first)
            {
                // Separate entries by two newlines.
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(str8_literal("\n\n")));
            }
            first = false;
            // The definitions are:
            // [[binding]]
            // name = 'foo'
            // key = 'key'
            // mods = ['ctrl', 'shift']
            //
            // So we will generate a basic template and insert into that.
            String8 base = fmt_string(buf,
R"([[binding]]
name = '%S'
key = '%S'
mods = [)", str8_mut(hotkey_name(e->cmd)), str8_mut(key_to_human(e->key)));
            str8_serial_push_str8(scratch.arena, &serial_lst, base);
            // Now we generate the mods array.
            bool comma = false;
            if (implies(e->mods, OS::KeyMods::Ctrl))
            {
                str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Ctrl)));
                str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                comma = true;
            }

            if (implies(e->mods, OS::KeyMods::Cmd))
            {
                str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Command)));
                str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                comma = true;
            }

            if (implies(e->mods, OS::KeyMods::Alt))
            {
                if (comma)
                {
                    str8_serial_push_char(scratch.arena, &serial_lst, ',');
                }
                str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Alt)));
                str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                comma = true;
            }

            if (implies(e->mods, OS::KeyMods::Shift))
            {
                if (comma)
                {
                    str8_serial_push_char(scratch.arena, &serial_lst, ',');
                }
                str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Shift)));
                str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                comma = true;
            }

            // Close the list.
            str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(str8_literal("]")));
        }

        // Persist custom hotkeys.
        for EachIndex(i, count_of<CustomHotkeyGroup>)
        {
            CustomHotkeyList* lst = &custom_hotkeys[i];
            for EachNode(n, lst->first)
            {
                if (not first)
                {
                    // Separate entries by two newlines.
                    str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(str8_literal("\n\n")));
                }
                first = false;
                CustomHotkeyInfo* cust_info = id_as_info(n->id);
                // The definitions are:
                // [[binding.custom]]
                // group = 'group'
                // fn = 'my_handler'
                // key = 'key'
                // mods = ['ctrl', 'alt']
                //
                // So we will generate a basic template and insert into that.
                String8 base = fmt_string(buf,
R"([[binding.custom]]
group = '%S'
fn = '%S'
key = '%S'
mods = [)", str8_mut(custom_hotkey_group_name(CustomHotkeyGroup(i))), cust_info->fn_name, str8_mut(key_to_human(n->key)));
                str8_serial_push_str8(scratch.arena, &serial_lst, base);
                // Now we generate the mods array.
                bool comma = false;
                if (implies(n->mods, OS::KeyMods::Ctrl))
                {
                    str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                    str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Ctrl)));
                    str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                    comma = true;
                }

                if (implies(n->mods, OS::KeyMods::Cmd))
                {
                    str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                    str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Command)));
                    str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                    comma = true;
                }

                if (implies(n->mods, OS::KeyMods::Alt))
                {
                    if (comma)
                    {
                        str8_serial_push_char(scratch.arena, &serial_lst, ',');
                    }
                    str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                    str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Alt)));
                    str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                    comma = true;
                }

                if (implies(n->mods, OS::KeyMods::Shift))
                {
                    if (comma)
                    {
                        str8_serial_push_char(scratch.arena, &serial_lst, ',');
                    }
                    str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                    str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Shift)));
                    str8_serial_push_char(scratch.arena, &serial_lst, '\'');
                    comma = true;
                }

                // Close the list.
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(str8_literal("]")));
            }
        }

        String8 result = str8_serial_end(scratch.arena, serial_lst);
        bool success = save_file(path, result);
        if (not success)
        {
            char fmt_buf[1024];
            auto os_err = OS::last_os_error_code();
            String8 os_err_str = OS::format_error(scratch.arena, os_err);
            auto txt = fmt_string(fmt_buf, "Failed to save to '%S': %S", path, os_err_str);
            feed->queue_info(txt);
        }
        Arena::scratch_end(scratch);
        return success;
    }

    bool needs_save()
    {
        return need_save;
    }

#define DAT_CAT_START(e, grp, cat, desc) HotkeyCollection cat ## _hotkeys() { \
 HotkeyEntry* first = &hotkeys[rep(Hotkey:: e ## _Start) + 1];                \
 HotkeyEntry* last = &hotkeys[rep(Hotkey:: e ## _End)];                       \
 return { .first = first, .last = last };                                     \
}
#define DAT_CMD(c, k, m, desc)
#define DAT_CAT_END(e, grp)
#include "hotkeys.dat"
#undef DAT_CAT_START
#undef DAT_CMD
#undef DAT_CAT_END

#define DAT_CAT_START(e, grp, cat, desc) HotkeyEntry match_ ## cat ## _hotkey(OS::Key key, OS::KeyMods mods) { \
 HotkeyCollection keys = cat ## _hotkeys();                                                                    \
 for (;keys.first < keys.last; ++keys.first) {                                                                 \
  if (keys.first->key == key and keys.first->mods == mods) return *keys.first;                                 \
 }                                                                                                             \
 return { .cmd = Hotkey::None };                                                                               \
}
#define DAT_CMD(c, k, m, desc)
#define DAT_CAT_END(e, grp)
#include "hotkeys.dat"
#undef DAT_CAT_START
#undef DAT_CMD
#undef DAT_CAT_END

    HotkeyEntry entry_for(Hotkey hk)
    {
        return hotkeys[rep(hk)];
    }

    HotkeyMatch match_group(CustomHotkeyGroup group, OS::Key key, OS::KeyMods mods)
    {
        HotkeyMatch result = {};
        HotkeyBindingNode* entry = fetch_hk_binding(&binding_map, group, { .key = key, .mods = mods });
        if (entry != nullptr)
        {
            result.cust_hk_id = entry->cust_hk_id;
            result.hk = entry->hk;
        }
        return result;
    }

    CustomHotkeyID match_custom_hotkey(CustomHotkeyGroup group, OS::Key key, OS::KeyMods mods)
    {
        for EachNode(n, custom_list_for_group_ref(group)->first)
        {
            if (n->key == key and n->mods == mods)
                return n->id;
        }
        return CustomHotkeyID::None;
    }

    CustomHotkeyList custom_hotkeys_for_group(CustomHotkeyGroup group)
    {
        return *custom_list_for_group_ref(group);
    }

    CustomHotkeyInfo custom_hotkey_info(CustomHotkeyID id)
    {
        return *id_as_info(id);
    }

    HotkeyCombo custom_hotkey_binding(CustomHotkeyGroup group, CustomHotkeyID id)
    {
        HotkeyCombo result = {};
        for EachNode(n, custom_list_for_group_ref(group)->first)
        {
            if (n->id == id)
            {
                result.key = n->key;
                result.mods = n->mods;
                break;
            }
        }
        return result;
    }

    bool is_default_binding(Hotkey hk)
    {
        auto e = entry_for(hk);
        auto def_e = defaults[rep(hk)];
        return e.key == def_e.key and e.mods == def_e.mods;
    }

    bool is_custom_hotkey_bound(CustomHotkeyID id)
    {
        return id_as_info(id)->fn != nullptr;
    }

    bool valid_match(HotkeyMatch c)
    {
        return c.cust_hk_id != CustomHotkeyID::None
                or c.hk != Hotkey::None;
    }

    bool is_match(HotkeyMatch c, Hotkey hk)
    {
        return c.hk == hk;
    }

    bool is_match(HotkeyMatch c, CustomHotkeyID id)
    {
        return c.cust_hk_id == id;
    }

    HotkeyMatchList bindings_for(Arena::Arena* arena, HotkeyCombo binding)
    {
        HotkeyMatchList result = {};
        fill_binding_matches_hk_binding(arena, &binding_map, &result, binding);
        return result;
    }

    // Custom hotkeys.
    void start_custom_hotkey_bindings()
    {
        Arena::clear(hk_cust_strings_arena);
    }

    void update_custom_hotkey_info(CustomHotkeyID id, CustomHotkeyInfo info)
    {
        info.name = str8_copy(hk_cust_strings_arena, info.name);
        info.desc = str8_copy(hk_cust_strings_arena, info.desc);
        info.fn_name = str8_copy(hk_cust_strings_arena, info.fn_name);
        *id_as_info(id) = info;
        need_save = true;
    }

    CustomHotkeyID make_custom_hotkey(CustomHotkeyGroup group, HotkeyCombo binding)
    {
        CustomHotkeyList* lst = custom_list_for_group_ref(group);
        HotkeyArenaPair arenas = {
            .arena = hk_arena,
            .strings_arena = hk_cust_strings_arena,
        };
        CustomHotkeyEntry* e = push_custom_hotkey(arenas, lst, binding.key, binding.mods, str8_empty);
        need_save = true;
        return e->id;
    }

    void remove_custom_hotkey(CustomHotkeyGroup group, CustomHotkeyID id)
    {
        CustomHotkeyList* lst = custom_list_for_group_ref(group);
        CustomHotkeyEntry* remove = nullptr;
        for EachNode(n, lst->first)
        {
            if (n->id == id)
            {
                remove = n;
                break;
            }
        }

        if (remove != nullptr)
        {
            // Unbind first.
            {
                HotkeyBindingNode node = {
                    .cust_hk_id = remove->id,
                    .hk = Hotkey::None,
                    .binding = { .key = remove->key, .mods = remove->mods }
                };
                unmap_hk_binding(&binding_map, group, node);
            }
            // Rework into new list.
            CustomHotkeyList new_lst = {};
            CustomHotkeyEntry* node = lst->first;
            while (node != nullptr)
            {
                CustomHotkeyEntry* next = node->next;
                node->next = nullptr;
                if (node != remove)
                {
                    SLLQueuePush(new_lst.first, new_lst.last, node);
                    ++new_lst.count;
                }
                node = next;
            }
            *lst = new_lst;
            need_save = true;
        }
    }

    // Helpers.
    String8 hotkey_as_string(Arena::Arena* arena, OS::Key k, OS::KeyMods mods)
    {
        String8 result = str8_empty;
        auto scratch = Arena::scratch_begin({ &arena, 1 });
        String8List serial_lst{};
        str8_serial_begin(scratch.arena, &serial_lst);
        if (implies(mods, OS::KeyMods::Ctrl))
        {
            str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Ctrl)));
        }

        if (implies(mods, OS::KeyMods::Cmd))
        {
            if (serial_lst.total_size == 0)
            {
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Command)));
            }
            else
            {
                str8_serial_push_char(scratch.arena, &serial_lst, '+');
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Command)));
            }
        }

        if (implies(mods, OS::KeyMods::Shift))
        {
            if (serial_lst.total_size == 0)
            {
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Shift)));
            }
            else
            {
                str8_serial_push_char(scratch.arena, &serial_lst, '+');
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Shift)));
            }
        }

        if (implies(mods, OS::KeyMods::Alt))
        {
            if (serial_lst.total_size == 0)
            {
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Alt)));
            }
            else
            {
                str8_serial_push_char(scratch.arena, &serial_lst, '+');
                str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(OS::Key::Alt)));
            }
        }

        if (serial_lst.total_size == 0)
        {
            str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(k)));
        }
        else
        {
            str8_serial_push_char(scratch.arena, &serial_lst, '+');
            str8_serial_push_str8(scratch.arena, &serial_lst, str8_mut(key_to_human(k)));
        }
        result = str8_serial_end(arena, serial_lst);
        Arena::scratch_end(scratch);
        return result;
    }

    String8 hotkey_as_string(Arena::Arena* arena, Hotkey hk)
    {
        HotkeyEntry* entry = &hotkeys[rep(hk)];
        return hotkey_as_string(arena, entry->key, entry->mods);
    }

    String8 hotkey_description(Hotkey hk)
    {
        switch (hk)
        {
#define DAT_CAT_START(e, grp, cat, desc)
#define DAT_CMD(c, k, m, desc) case Hotkey::c: return str8_mut(str8_literal(desc));
#define DAT_CAT_END(e, grp)
#include "hotkeys.dat"
#undef DAT_CAT_START
#undef DAT_CMD
#undef DAT_CAT_END
        }
        return str8_empty;
    }

    String8 hotkey_group_as_string(CustomHotkeyGroup group)
    {
        return str8_mut(custom_hotkey_group_name(group));
    }

    String8 describe_match(HotkeyMatch match)
    {
        if (match.hk != Hotkey::None)
            return hotkey_description(match.hk);
        if (match.cust_hk_id != CustomHotkeyID::None)
            return id_as_info(match.cust_hk_id)->name;
        return str8_empty;
    }

    void rebind(Hotkey hk, OS::Key k, OS::KeyMods mods)
    {
        HotkeyEntry entry = entry_for(hk);
        HotkeyBindingNode node = {
            .cust_hk_id = CustomHotkeyID::None,
            .hk = hk,
            .binding = { .key = entry.key, .mods = entry.mods }
        };
        CustomHotkeyGroup group = group_for_hotkey(hk);
        remap_binding(hk_arena, &binding_map, group, node, { .key = k, .mods = mods });
        rebind_no_compute(hk, k, mods);
        need_save = true;
    }

    void rebind(CustomHotkeyID id, OS::Key k, OS::KeyMods mods)
    {
        rebind_and_remap(hk_arena, &binding_map, id, k, mods);
        need_save = true;
    }

    void clear_binding(Hotkey hk)
    {
        HotkeyEntry entry = entry_for(hk);
        HotkeyBindingNode node = {
            .cust_hk_id = CustomHotkeyID::None,
            .hk = hk,
            .binding = { .key = entry.key, .mods = entry.mods }
        };
        CustomHotkeyGroup group = group_for_hotkey(hk);
        remap_binding(hk_arena, &binding_map, group, node, { .key = OS::Key::Null, .mods = OS::KeyMods::None });
        rebind_no_compute(hk, OS::Key::Null, OS::KeyMods::None);
        need_save = true;
    }

    void clear_binding(CustomHotkeyID id)
    {
        rebind_and_remap(hk_arena, &binding_map, id, OS::Key::Null, OS::KeyMods::None);
        need_save = true;
    }

    void reset_to_default(Hotkey hk)
    {
        HotkeyEntry def_entry = defaults[rep(hk)];
        rebind(hk, def_entry.key, def_entry.mods);
    }
} // namespace Hotkeys
