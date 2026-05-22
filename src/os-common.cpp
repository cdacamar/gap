#include "os.h"

#include <cassert>

#include "macros.h"
#include "thread-ctx.h"

namespace OS
{
    DenseTime dense_time_from_date_time(const DateTime& date_time)
    {
        PrimitiveType<DenseTime> result = 0;
        result += date_time.year;
        result *= 12;
        result += date_time.mon;
        result *= 31;
        result += date_time.day;
        result *= 24;
        result += date_time.hour;
        result *= 60;
        result += date_time.min;
        result *= 61;
        result += date_time.sec;
        result *= 1000;
        result += date_time.msec;
        return DenseTime{ result };
    }

    DateTime date_time_from_micro_seconds(MicroSec us)
    {
        uint64_t time = rep(us);
        DateTime result{};
        result.micro_sec = time % 1000;
        time /= 1000;
        result.msec = time % 1000;
        time /= 1000;
        result.sec = time % 60;
        time /= 60;
        result.min = time % 60;
        time /= 60;
        result.hour = time % 24;
        time /= 24;
        result.day = time % 31;
        time /= 31;
        result.mon = time % 12;
        time /= 12;
        assert(time <= rep(MicroSec::Infinite));
        result.year = static_cast<uint32_t>(time);
        return result;
    }

    ENABLE_UNHANDLED_CASE_WARNING()
    String8 event_sort_string(EventSort s)
    {
        switch (s)
        {
        case EventSort::Quit:
            return str8_mut(str8_literal("EventSort::Quit"));
        case EventSort::Release:
            return str8_mut(str8_literal("EventSort::Release"));
        case EventSort::Press:
            return str8_mut(str8_literal("EventSort::Press"));
        case EventSort::MouseMove:
            return str8_mut(str8_literal("EventSort::MouseMove"));
        case EventSort::Scroll:
            return str8_mut(str8_literal("EventSort::Scroll"));
        case EventSort::Text:
            return str8_mut(str8_literal("EventSort::Text"));
        case EventSort::FileDrop:
            return str8_mut(str8_literal("EventSort::FileDrop"));
        case EventSort::FocusLost:
            return str8_mut(str8_literal("EventSort::FocusLost"));
        case EventSort::GapThreadWakeup:
            return str8_mut(str8_literal("EventSort::GapThreadWakeup"));
        case EventSort::Count:
            return str8_mut(str8_literal("EventSort::Count"));
        }
        return str8_empty;
    }

    String8 key_to_string(Key k)
    {
        switch (k)
        {
        case Key::Null:
            return str8_mut(str8_literal("Key::Null"));
        case Key::Esc:
            return str8_mut(str8_literal("Key::Esc"));
        case Key::F1:
            return str8_mut(str8_literal("Key::F1"));
        case Key::F2:
            return str8_mut(str8_literal("Key::F2"));
        case Key::F3:
            return str8_mut(str8_literal("Key::F3"));
        case Key::F4:
            return str8_mut(str8_literal("Key::F4"));
        case Key::F5:
            return str8_mut(str8_literal("Key::F5"));
        case Key::F6:
            return str8_mut(str8_literal("Key::F6"));
        case Key::F7:
            return str8_mut(str8_literal("Key::F7"));
        case Key::F8:
            return str8_mut(str8_literal("Key::F8"));
        case Key::F9:
            return str8_mut(str8_literal("Key::F9"));
        case Key::F10:
            return str8_mut(str8_literal("Key::F10"));
        case Key::F11:
            return str8_mut(str8_literal("Key::F11"));
        case Key::F12:
            return str8_mut(str8_literal("Key::F12"));
        case Key::F13:
            return str8_mut(str8_literal("Key::F13"));
        case Key::F14:
            return str8_mut(str8_literal("Key::F14"));
        case Key::F15:
            return str8_mut(str8_literal("Key::F15"));
        case Key::F16:
            return str8_mut(str8_literal("Key::F16"));
        case Key::F17:
            return str8_mut(str8_literal("Key::F17"));
        case Key::F18:
            return str8_mut(str8_literal("Key::F18"));
        case Key::F19:
            return str8_mut(str8_literal("Key::F19"));
        case Key::F20:
            return str8_mut(str8_literal("Key::F20"));
        case Key::F21:
            return str8_mut(str8_literal("Key::F21"));
        case Key::F22:
            return str8_mut(str8_literal("Key::F22"));
        case Key::F23:
            return str8_mut(str8_literal("Key::F23"));
        case Key::F24:
            return str8_mut(str8_literal("Key::F24"));
        case Key::Tick:
            return str8_mut(str8_literal("Key::Tick"));
        case Key::_0:
            return str8_mut(str8_literal("Key::_0"));
        case Key::_1:
            return str8_mut(str8_literal("Key::_1"));
        case Key::_2:
            return str8_mut(str8_literal("Key::_2"));
        case Key::_3:
            return str8_mut(str8_literal("Key::_3"));
        case Key::_4:
            return str8_mut(str8_literal("Key::_4"));
        case Key::_5:
            return str8_mut(str8_literal("Key::_5"));
        case Key::_6:
            return str8_mut(str8_literal("Key::_6"));
        case Key::_7:
            return str8_mut(str8_literal("Key::_7"));
        case Key::_8:
            return str8_mut(str8_literal("Key::_8"));
        case Key::_9:
            return str8_mut(str8_literal("Key::_9"));
        case Key::Minus:
            return str8_mut(str8_literal("Key::Minus"));
        case Key::Equal:
            return str8_mut(str8_literal("Key::Equal"));
        case Key::Backspace:
            return str8_mut(str8_literal("Key::Backspace"));
        case Key::Tab:
            return str8_mut(str8_literal("Key::Tab"));
        case Key::Q:
            return str8_mut(str8_literal("Key::Q"));
        case Key::W:
            return str8_mut(str8_literal("Key::W"));
        case Key::E:
            return str8_mut(str8_literal("Key::E"));
        case Key::R:
            return str8_mut(str8_literal("Key::R"));
        case Key::T:
            return str8_mut(str8_literal("Key::T"));
        case Key::Y:
            return str8_mut(str8_literal("Key::Y"));
        case Key::U:
            return str8_mut(str8_literal("Key::U"));
        case Key::I:
            return str8_mut(str8_literal("Key::I"));
        case Key::O:
            return str8_mut(str8_literal("Key::O"));
        case Key::P:
            return str8_mut(str8_literal("Key::P"));
        case Key::LeftBracket:
            return str8_mut(str8_literal("Key::LeftBracket"));
        case Key::RightBracket:
            return str8_mut(str8_literal("Key::RightBracket"));
        case Key::BackSlash:
            return str8_mut(str8_literal("Key::BackSlash"));
        case Key::CapsLock:
            return str8_mut(str8_literal("Key::CapsLock"));
        case Key::A:
            return str8_mut(str8_literal("Key::A"));
        case Key::S:
            return str8_mut(str8_literal("Key::S"));
        case Key::D:
            return str8_mut(str8_literal("Key::D"));
        case Key::F:
            return str8_mut(str8_literal("Key::F"));
        case Key::G:
            return str8_mut(str8_literal("Key::G"));
        case Key::H:
            return str8_mut(str8_literal("Key::H"));
        case Key::J:
            return str8_mut(str8_literal("Key::J"));
        case Key::K:
            return str8_mut(str8_literal("Key::K"));
        case Key::L:
            return str8_mut(str8_literal("Key::L"));
        case Key::Semicolon:
            return str8_mut(str8_literal("Key::Semicolon"));
        case Key::Quote:
            return str8_mut(str8_literal("Key::Quote"));
        case Key::Return:
            return str8_mut(str8_literal("Key::Return"));
        case Key::Shift:
            return str8_mut(str8_literal("Key::Shift"));
        case Key::Z:
            return str8_mut(str8_literal("Key::Z"));
        case Key::X:
            return str8_mut(str8_literal("Key::X"));
        case Key::C:
            return str8_mut(str8_literal("Key::C"));
        case Key::V:
            return str8_mut(str8_literal("Key::V"));
        case Key::B:
            return str8_mut(str8_literal("Key::B"));
        case Key::N:
            return str8_mut(str8_literal("Key::N"));
        case Key::M:
            return str8_mut(str8_literal("Key::M"));
        case Key::Comma:
            return str8_mut(str8_literal("Key::Comma"));
        case Key::Period:
            return str8_mut(str8_literal("Key::Period"));
        case Key::Slash:
            return str8_mut(str8_literal("Key::Slash"));
        case Key::Ctrl:
            return str8_mut(str8_literal("Key::Ctrl"));
        case Key::Alt:
            return str8_mut(str8_literal("Key::Alt"));
        case Key::Space:
            return str8_mut(str8_literal("Key::Space"));
        case Key::Menu:
            return str8_mut(str8_literal("Key::Menu"));
        case Key::ScrollLock:
            return str8_mut(str8_literal("Key::ScrollLock"));
        case Key::Pause:
            return str8_mut(str8_literal("Key::Pause"));
        case Key::Insert:
            return str8_mut(str8_literal("Key::Insert"));
        case Key::Home:
            return str8_mut(str8_literal("Key::Home"));
        case Key::PageUp:
            return str8_mut(str8_literal("Key::PageUp"));
        case Key::Delete:
            return str8_mut(str8_literal("Key::Delete"));
        case Key::End:
            return str8_mut(str8_literal("Key::End"));
        case Key::PageDown:
            return str8_mut(str8_literal("Key::PageDown"));
        case Key::Up:
            return str8_mut(str8_literal("Key::Up"));
        case Key::Left:
            return str8_mut(str8_literal("Key::Left"));
        case Key::Down:
            return str8_mut(str8_literal("Key::Down"));
        case Key::Right:
            return str8_mut(str8_literal("Key::Right"));
        case Key::Ex0:
            return str8_mut(str8_literal("Key::Ex0"));
        case Key::Ex1:
            return str8_mut(str8_literal("Key::Ex1"));
        case Key::Ex2:
            return str8_mut(str8_literal("Key::Ex2"));
        case Key::Ex3:
            return str8_mut(str8_literal("Key::Ex3"));
        case Key::Ex4:
            return str8_mut(str8_literal("Key::Ex4"));
        case Key::Ex5:
            return str8_mut(str8_literal("Key::Ex5"));
        case Key::Ex6:
            return str8_mut(str8_literal("Key::Ex6"));
        case Key::Ex7:
            return str8_mut(str8_literal("Key::Ex7"));
        case Key::Ex8:
            return str8_mut(str8_literal("Key::Ex8"));
        case Key::Ex9:
            return str8_mut(str8_literal("Key::Ex9"));
        case Key::Ex10:
            return str8_mut(str8_literal("Key::Ex10"));
        case Key::Ex11:
            return str8_mut(str8_literal("Key::Ex11"));
        case Key::Ex12:
            return str8_mut(str8_literal("Key::Ex12"));
        case Key::Ex13:
            return str8_mut(str8_literal("Key::Ex13"));
        case Key::Ex14:
            return str8_mut(str8_literal("Key::Ex14"));
        case Key::Ex15:
            return str8_mut(str8_literal("Key::Ex15"));
        case Key::Ex16:
            return str8_mut(str8_literal("Key::Ex16"));
        case Key::Ex17:
            return str8_mut(str8_literal("Key::Ex17"));
        case Key::Ex18:
            return str8_mut(str8_literal("Key::Ex18"));
        case Key::Ex19:
            return str8_mut(str8_literal("Key::Ex19"));
        case Key::Ex20:
            return str8_mut(str8_literal("Key::Ex20"));
        case Key::Ex21:
            return str8_mut(str8_literal("Key::Ex21"));
        case Key::Ex22:
            return str8_mut(str8_literal("Key::Ex22"));
        case Key::Ex23:
            return str8_mut(str8_literal("Key::Ex23"));
        case Key::Ex24:
            return str8_mut(str8_literal("Key::Ex24"));
        case Key::Ex25:
            return str8_mut(str8_literal("Key::Ex25"));
        case Key::Ex26:
            return str8_mut(str8_literal("Key::Ex26"));
        case Key::Ex27:
            return str8_mut(str8_literal("Key::Ex27"));
        case Key::Ex28:
            return str8_mut(str8_literal("Key::Ex28"));
        case Key::Ex29:
            return str8_mut(str8_literal("Key::Ex29"));
        case Key::NumLock:
            return str8_mut(str8_literal("Key::NumLock"));
        case Key::NumSlash:
            return str8_mut(str8_literal("Key::NumSlash"));
        case Key::NumStar:
            return str8_mut(str8_literal("Key::NumStar"));
        case Key::NumMinus:
            return str8_mut(str8_literal("Key::NumMinus"));
        case Key::NumPlus:
            return str8_mut(str8_literal("Key::NumPlus"));
        case Key::NumPeriod:
            return str8_mut(str8_literal("Key::NumPeriod"));
        case Key::Num0:
            return str8_mut(str8_literal("Key::Num0"));
        case Key::Num1:
            return str8_mut(str8_literal("Key::Num1"));
        case Key::Num2:
            return str8_mut(str8_literal("Key::Num2"));
        case Key::Num3:
            return str8_mut(str8_literal("Key::Num3"));
        case Key::Num4:
            return str8_mut(str8_literal("Key::Num4"));
        case Key::Num5:
            return str8_mut(str8_literal("Key::Num5"));
        case Key::Num6:
            return str8_mut(str8_literal("Key::Num6"));
        case Key::Num7:
            return str8_mut(str8_literal("Key::Num7"));
        case Key::Num8:
            return str8_mut(str8_literal("Key::Num8"));
        case Key::Num9:
            return str8_mut(str8_literal("Key::Num9"));
        case Key::LeftMouseButton:
            return str8_mut(str8_literal("Key::LeftMouseButton"));
        case Key::MiddleMouseButton:
            return str8_mut(str8_literal("Key::MiddleMouseButton"));
        case Key::RightMouseButton:
            return str8_mut(str8_literal("Key::RightMouseButton"));
        case Key::Command:
            return str8_mut(str8_literal("Key::Command"));
        case Key::Count:
            return str8_mut(str8_literal("Key::Count"));
        }
        return str8_empty;
    }
    DISABLE_UNHANDLED_CASE_WARNING()

    String8 file_extension(String8 file)
    {
        auto pos = str8_find_last_of(file, str8_mut(str8_literal(".")));
        if (pos == str8_index_sentinel)
            return str8_empty;
        // Retain the '.'.
        return str8_substr(file, { .off = pos });
    }

    void thread_entry_bridge(ThreadEntryPointFunctionType target, void* data_p)
    {
        {
            ::Thread::TLD* tld_this_thread = ::Thread::tld_alloc();
            ::Thread::tld_select(tld_this_thread);
        }
        target(data_p);
        ::Thread::tld_release(::Thread::tld_selected());
    }
} // namespace OS
