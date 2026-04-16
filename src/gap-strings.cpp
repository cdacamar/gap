#include "gap-strings.h"

#include <ctype.h>
#include <stdarg.h>
#include <string.h>

#include <charconv>

#include <stb_sprintf.h>

#include "enum-utils.h"

// Node construction.
String8Node* str8_list_push_node(String8List* lst, String8Node* node)
{
    SLLQueuePush(lst->first, lst->last, node);
    ++lst->node_count;
    lst->total_size += node->string.size;
    return node;
}

String8Node* str8_list_push_node_set_string(String8List* lst, String8Node* node, String8 string)
{
    SLLQueuePush(lst->first, lst->last, node);
    ++lst->node_count;
    lst->total_size += string.size;
    node->string = string;
    return node;
}

// List construction.
String8Node* str8_list_push(Arena::Arena* arena, String8List* lst, String8 string)
{
    String8Node* node = Arena::push_array_no_zero<String8Node>(arena, 1);
    return str8_list_push_node_set_string(lst, node, string);
}

// Serializing data.
void str8_serial_begin(Arena::Arena* arena, String8List* lst)
{
    String8Node* node = str8_list_push(arena, lst, str8_empty);
    // Begin the string allocation site.
    node->string.str = Arena::push_array_no_zero<char>(arena, 0);
}

String8 str8_serial_end(Arena::Arena* arena, const String8List& lst)
{
    String8 result = str8_cstr_alloc(arena, lst.total_size);
    char* out = result.str;
    for EachNode(n, lst.first)
    {
        memcpy(out, n->string.str, n->string.size);
        out += n->string.size;
    }
    return result;
}

void str8_serial_push_char(Arena::Arena* arena, String8List* lst, char c)
{
    str8_serial_push_str8(arena, lst, str8(&c, 1));
}

void str8_serial_push_str8(Arena::Arena* arena, String8List* lst, String8 str)
{
    if (str.size == 0)
        return;
    // Try to append allocations.
    auto arena_pos = Arena::pos(arena);
    char* buf = Arena::push_array_no_zero_aligned<char>(arena, str.size, Arena::Alignment{ alignof(char) });
    String8* latest = &lst->last->string;
    if (latest->str + latest->size == buf)
    {
        latest->size += str.size;
        lst->total_size += str.size;
    }
    // Append a new node.
    else
    {
        // Note: in order for this to remain efficient, we will actually discard the memory allocated above, allocate a new node and _then_
        // allocate a new string buffer.  This is to ensure that we can grow the buffer of the new string in the new chunk.
        Arena::pop_to(arena, arena_pos);
        str8_list_push(arena, lst, str);
        // Now we allocate a buffer for the string, and assign it.
        buf = Arena::push_array_no_zero<char>(arena, str.size);
        lst->last->string.str = buf;
    }
    memcpy(buf, str.str, str.size);
}

// List joining.
String8 str8_list_join(Arena::Arena* arena, const String8List& lst)
{
    String8 result = str8_cstr_alloc(arena, lst.total_size);
    char* ptr = result.str;
    for EachNode(n, lst.first)
    {
        memcpy(ptr, n->string.str, n->string.size);
        ptr += n->string.size;
    }
    return result;
}

// Basic string construction.
String8 str8_cstr(char* str)
{
    String8 s{};
    s.str = str;
    s.size = str != nullptr ? strlen(str) : 0;
    return s;
}

String8 str8_cppview(std::string_view str)
{
    String8 s{};
    // Bad, but a workaround until I get rid of string_view.
    s.str = const_cast<char*>(str.data());
    s.size = str.size();
    return s;
}

String8 str8_mut(String8View str)
{
    String8 s{};
    s.str = const_cast<char*>(str.str);
    s.size = str.size;
    return s;
}

String8 str8_alloc(Arena::Arena* arena, uint64_t size)
{
    char* str = Arena::push_array_no_zero<char>(arena, size);
    return str8(str, size);
}

String8 str8_cstr_alloc(Arena::Arena* arena, uint64_t size)
{
    char* str = Arena::push_array_no_zero<char>(arena, size + 1);
    str[size] = 0;
    return str8(str, size);
}

String8 str8_copy(Arena::Arena* arena, String8 string)
{
    // +1 for null byte.
    String8 cpy{};
    cpy.size = string.size;
    cpy.str = Arena::push_array_no_zero<char>(arena, string.size + 1);
    memcpy(cpy.str, string.str, string.size);
    cpy.str[cpy.size] = 0;
    return cpy;
}

String8 str8_cat(Arena::Arena* arena, String8 a, String8 b)
{
    String8 cpy{};
    cpy.size = a.size + b.size;
    cpy.str = Arena::push_array_no_zero<char>(arena, cpy.size + 1);
    memcpy(cpy.str, a.str, a.size);
    memcpy(cpy.str + a.size, b.str, b.size);
    cpy.str[cpy.size] = 0;
    return cpy;
}

// Formatting.
String8 str8_fmt(Arena::Arena* arena, const char* fmt, ...)
{
    String8 result;
    va_list va;
    va_start(va, fmt);
    va_list va_tmp;
    va_copy(va_tmp, va);
    uint32_t needed = stbsp_vsnprintf(nullptr, 0, fmt, va_tmp);
    va_end(va_tmp);
    result = str8_cstr_alloc(arena, needed);
    // We add an additional byte here because ctr_alloc will give us a null by default.
    result.size = stbsp_vsnprintf(result.str, needed + 1, fmt, va);
    result.str[result.size] = 0;
    va_end(va);
    return result;
}

String8 str8_fmt_va(Arena::Arena* arena, const char* fmt, va_list lst)
{
    String8 result;
    va_list va_tmp;
    va_copy(va_tmp, lst);
    uint32_t needed = stbsp_vsnprintf(nullptr, 0, fmt, va_tmp);
    va_end(va_tmp);
    result = str8_cstr_alloc(arena, needed);
    // We add an additional byte here because ctr_alloc will give us a null by default.
    result.size = stbsp_vsnprintf(result.str, needed + 1, fmt, lst);
    result.str[result.size] = 0;
    return result;
}

// Conversions.

// This is an injective table which encodes ascii values -> integral counterparts.  Includes
// upper-case letters for hex.
constexpr uint8_t intval_from_char_map[] =
{
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
};

constexpr uint8_t ascii_end = 0x7F;

uint64_t u64_from_str8(String8 string, uint32_t radix)
{
    uint64_t x = 0;
    if (1 < radix and radix <= 16)
    {
        for EachIndex(i, string.size)
        {
            x *= radix;
            x += intval_from_char_map[string.str[i] & ascii_end];
        }
    }
    return x;
}

int64_t s64_from_str8(String8 string, uint32_t radix)
{
    // Count signs and eat them.
    uint64_t sign_count = 0;
    uint64_t off = 0;
    for (; off < string.size; ++off)
    {
        if (string.str[off] == '-')
        {
            ++sign_count;
        }
        else if (string.str[off] != '+')
        {
            break;
        }
    }
    int64_t sign = (sign_count & 1) ? -1 : 1;
    int64_t result = static_cast<int64_t>(u64_from_str8(str8_substr(string, { .off = off }), radix));
    return result * sign;
}

bool str8_is_integer(String8 string, uint32_t radix)
{
    if (string.size == 0)
        return false;
    if (radix <= 1)
        return false;
    if (radix > 16)
        return false;
    for EachIndex(i, string.size)
    {
        char c = string.str[i];
        if (c > ascii_end)
            return false;
        uint8_t v = intval_from_char_map[c & ascii_end];
        if (v >= radix)
            return false;
    }
    return true;
}

bool try_f64_from_str8(String8 string, double* result)
{
    std::from_chars_result r = std::from_chars(string.str, string.str + string.size, *result);
    return r.ec == std::errc{};
}

bool try_f64_from_str8_hex_float(String8 string, double* result)
{
    std::from_chars_result r = std::from_chars(string.str, string.str + string.size, *result, std::chars_format::hex);
    return r.ec == std::errc{};
}

String16 str16_cstr(wchar_t* str)
{
    String16 s{};
    s.str = str;
    s.size = str != nullptr ? wcslen(str) : 0;
    return s;
}

String16 str16_mut(String16View str)
{
    String16 s{};
    s.str = const_cast<wchar_t*>(str.str);
    s.size = str.size;
    return s;
}

String16 str16_cstr_alloc(Arena::Arena* arena, uint64_t size)
{
    wchar_t* str = Arena::push_array_no_zero<wchar_t>(arena, size + 1);
    str[size] = 0;
    return str16(str, size);
}

// String slicing.
String8 str8_substr(String8 str, String8Slice slice)
{
    // Clip the result.
    slice.off = std::min(str.size, slice.off);
    slice.len = std::min(str.size - slice.off, slice.len);
    str.str += slice.off;
    str.size = slice.len;
    return str;
}

String8 str8_chop_prefix(String8 str, String8 prefix)
{
    if (prefix.size > str.size)
        return str;
    String8 chopped = str8_substr(str, { .len = prefix.size });
    if (str8_match_exact(chopped, prefix))
    {
        str = str8_substr(str, { .off = prefix.size, .len = str.size - prefix.size });
    }
    return str;
}

// String trimming.
static bool str8_whitespace(unsigned char c)
{
    return isspace(c);
}

TrimResult str8_trim_whitespace(String8 str)
{
    TrimResult result{};
    uint64_t first_nspc = str.size;
    for EachIndex(i, str.size)
    {
        if (not str8_whitespace(str.str[i]))
        {
            first_nspc = i;
            break;
        }
    }
    result.trimmed_start = first_nspc;
    str = str8_substr(str, { .off = first_nspc });
    // Find last non-space.
    uint64_t tail_len = 0;
    for EachIndex(i, str.size)
    {
        if (str8_whitespace(str.str[i]))
            break;
        ++tail_len;
    }
    result.trimmed_end = str.size - tail_len;
    str = str8_substr(str, { .len = tail_len });
    result.result = str;
    return result;
}

// String searching.
bool str8_match_exact(String8 a, String8 b)
{
    if (a.size != b.size)
        return false;
    return memcmp(a.str, b.str, a.size) == 0;
}

uint64_t str8_find_last_of(String8 str, String8 pattern)
{
    if (pattern.size > str.size)
        return str8_index_sentinel;
    for (int64_t i = str.size - pattern.size; i >= 0; --i)
    {
        String8 chopped = str8_substr(str, { .off = static_cast<uint64_t>(i), .len = pattern.size });
        if (str8_match_exact(chopped, pattern))
            return static_cast<uint64_t>(i);
    }
    return str8_index_sentinel;
}

bool str8_ends_with(String8 str, String8 suffix)
{
    if (suffix.size > str.size)
        return false;
    String8 chopped = str8_substr(str, { .off = str.size - suffix.size, .len = suffix.size });
    return str8_match_exact(chopped, suffix);
}

bool str8_starts_with(String8 str, String8 prefix)
{
    if (prefix.size > str.size)
        return false;
    String8 chopped = str8_substr(str, { .len = prefix.size });
    return str8_match_exact(chopped, prefix);
}

bool str16_match_exact(String16 a, String16 b)
{
    if (a.size != b.size)
        return false;
    return memcmp(a.str, b.str, a.size * sizeof(wchar_t)) == 0;
}

void str8_boyer_moore_init(BoyerMooreSearchData* data, String8 pattern)
{
    data->pat_first = pattern.str;
    data->pat_sz = static_cast<int64_t>(pattern.size - 1);
    for EachIndex(i, BoyerMooreSearchData::table_size)
    {
        data->shift[i] = static_cast<int64_t>(pattern.size);
    }

    // Build the shift table itself.
    for (uint64_t i = 1; i <= pattern.size; ++i)
    {
        data->shift[static_cast<unsigned char>(pattern.str[i - 1])] = static_cast<int64_t>(pattern.size - i);
    }
}

void str8_boyer_moore_ignore_case_init(BoyerMooreSearchData* data, String8 pattern)
{
    data->pat_first = pattern.str;
    data->pat_sz = static_cast<int64_t>(pattern.size - 1);
    for EachIndex(i, BoyerMooreSearchData::table_size)
    {
        data->shift[i] = static_cast<int64_t>(pattern.size);
    }

    // Build the shift table itself.
    for (uint64_t i = 1; i <= pattern.size; ++i)
    {
        data->shift[tolower(static_cast<unsigned char>(pattern.str[i - 1]))] = static_cast<int64_t>(pattern.size - i);
    }
}

char* str8_boyer_moore_search(BoyerMooreSearchData* data, char* first, char* last)
{
    int64_t shift_i = data->pat_sz;
    while (shift_i < (last - first))
    {
        first += shift_i;
        shift_i = data->shift[static_cast<unsigned char>(*first)];
        if (shift_i == 0) // implies *first == data->pat_first[data->pat_sz].
        {
            char* candidate = first - data->pat_sz;
            if (memcmp(candidate, data->pat_first, data->pat_sz) == 0)
                return candidate;
            shift_i = 1;
        }
    }
    return last;
}

char* str8_boyer_moore_search_ignore_case(BoyerMooreSearchData* data, char* first, char* last)
{
    int64_t shift_i = data->pat_sz;
    while (shift_i < (last - first))
    {
        first += shift_i;
        shift_i = data->shift[tolower(static_cast<unsigned char>(*first))];
        if (shift_i == 0) // implies *first == data->pat_first[data->pat_sz].
        {
            char* candidate = first - data->pat_sz;
            bool match = true;
            for EachIndex(i, static_cast<uint64_t>(data->pat_sz))
            {
                if (tolower(static_cast<unsigned int>(candidate[i])) != tolower(static_cast<unsigned int>(data->pat_first[i])))
                {
                    match = false;
                    break;
                }
            }

            if (match)
                return candidate;
            shift_i = 1;
        }
    }
    return last;
}

// String comparison.
int str8_compare(String8 a, String8 b)
{
    uint64_t sz = std::min(a.size, b.size);
    int c = memcmp(a.str, b.str, sz);
    if (c == 0)
    {
        if (a.size < b.size)
        {
            c = -1;
        }
        else if (a.size > b.size)
        {
            c = 1;
        }
    }
    return c;
}

String8 fmt_string(char* buf, size_t count, const char* fmt, ...)
{
    String8 result;
    va_list va;
    va_start(va, fmt);
    result = fmt_string(buf, static_cast<int>(count), fmt, va);
    va_end(va);
    return result;
}

String8 fmt_string(char* buf, size_t count, const char* fmt, va_list lst)
{
    int result_count = 0;
    result_count = stbsp_vsnprintf(buf, static_cast<int>(count), fmt, lst);
    return str8(buf, static_cast<uint64_t>(result_count));
}

String8 join_strings(Arena::Arena* arena, JoinStringsInput in)
{
    String8 result = str8_empty;
    // Preallocate.
    size_t alloc_count = in.strings.total_size;
    size_t elm_count = in.strings.node_count;
    // We actually have middle bits to add.
    if (elm_count != 0)
    {
        alloc_count += in.sep.size * (elm_count - 1);
    }
    alloc_count += in.start_sep.size + in.end_sep.size;
    result = str8_cstr_alloc(arena, alloc_count);
    char* buf = result.str;
    bool first_elm = true;
    memcpy(buf, in.start_sep.str, in.start_sep.size);
    buf += in.start_sep.size;
    for EachNode(n, in.strings.first)
    {
        // Prepend with separator.
        if (not first_elm)
        {
            memcpy(buf, in.sep.str, in.sep.size);
            buf += in.sep.size;
        }
        memcpy(buf, n->string.str, n->string.size);
        buf += n->string.size;
        first_elm = false;
    }
    memcpy(buf, in.end_sep.str, in.end_sep.size);
    return result;
}

void split_strings(Arena::Arena* arena, String8List* result, SplitStringsInput in)
{
    if (not implies(in.flags, SplitStringsFlags::NoResultClear))
    {
        *result = String8List{};
    }
    char* first = in.in.str;
    char* last = first + in.in.size;
    for (;first < last; ++first)
    {
        char* trailer = first;
        bool split = false;
        for (;first != last; ++first)
        {
            for (size_t i = 0; i < in.seps.size; ++i)
            {
                if (*first == in.seps.str[i])
                {
                    split = true;
                    break;
                }
            }

            if (split)
                break;
        }
        String8 elm = str8(trailer, first - trailer);
        if (elm.size != 0)
        {
            str8_list_push(arena, result, elm);
        }
    }
}