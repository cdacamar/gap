#pragma once

#include <stdarg.h>

#include <string_view>

#include "arena.h"

// Basic strings.
struct String8
{
    char* str;
    uint64_t size;
};

struct String16
{
    wchar_t* str;
    uint64_t size;
};

struct String8View
{
    const char* str;
    uint64_t size;
};

struct String16View
{
    const wchar_t* str;
    uint64_t size;
};

struct String8Node
{
    String8Node* next;
    String8 string;
};

struct String8List
{
    String8Node* first;
    String8Node* last;
    uint64_t node_count;
    uint64_t total_size;
};

struct String8Array
{
    String8* strs;
    uint64_t size;
};

// C++ nonsense.
struct String8ListItr
{
    String8Node* cur;

    constexpr String8 operator*() { return cur->string; }
    constexpr void operator++() { cur = cur->next; }
    friend bool operator==(String8ListItr, String8ListItr) = default;
};

constexpr String8ListItr begin(const String8List& lst)
{
    return { .cur = lst.first };
}

constexpr String8ListItr end(const String8List&)
{
    return {};
}

constexpr std::string_view sv_str8(String8 str)
{
    return { str.str, str.size };
}

// Node construction.
String8Node* str8_list_push_node(String8List* lst, String8Node* node);
String8Node* str8_list_push_node_set_string(String8List* lst, String8Node* node, String8 string);

// List construction.
String8Node* str8_list_push(Arena::Arena* arena, String8List* lst, String8 string);

// Serializing data.
void str8_serial_begin(Arena::Arena* arena, String8List* lst);
String8 str8_serial_end(Arena::Arena* arena, const String8List& lst);
void str8_serial_push_char(Arena::Arena* arena, String8List* lst, char c);
void str8_serial_push_str8(Arena::Arena* arena, String8List* lst, String8 str);

// List joining.
String8 str8_list_join(Arena::Arena* arena, const String8List& lst);

// Basic string construction.
inline constexpr String8 str8_empty{};

String8 str8_cstr(char* str);
String8 str8_cppview(std::string_view str);
String8 str8_mut(String8View str);

constexpr String8 str8(char* str, uint64_t size)
{
    return { .str = str, .size = size };
}

template <int N>
constexpr String8 str8(char (&arr)[N])
{
    return str8(arr, N);
}

template <int N>
constexpr String8View str8_literal(const char (&arr)[N])
{
    return { .str = arr, .size = N - 1 };
}

String8 str8_alloc(Arena::Arena* arena, uint64_t size);
String8 str8_cstr_alloc(Arena::Arena* arena, uint64_t size);
String8 str8_copy(Arena::Arena* arena, String8 string);
String8 str8_cat(Arena::Arena* arena, String8 a, String8 b);

// Formatting.
String8 str8_fmt(Arena::Arena* arena, const char* fmt, ...);
String8 str8_fmt_va(Arena::Arena* arena, const char* fmt, va_list lst);

// Conversions.
uint64_t u64_from_str8(String8 string, uint32_t radix);
int64_t s64_from_str8(String8 string, uint32_t radix);
bool str8_is_integer(String8 string, uint32_t radix);
bool try_f64_from_str8(String8 string, double* result);
bool try_f64_from_str8_hex_float(String8 string, double* result);

constexpr String16 str16(wchar_t* str, uint64_t size)
{
    return { .str = str, .size = size };
}

template <int N>
constexpr String16View str16_literal(const wchar_t (&arr)[N])
{
    return { .str = arr, .size = N - 1 };
}

String16 str16_cstr(wchar_t* str);
String16 str16_mut(String16View str);

String16 str16_cstr_alloc(Arena::Arena* arena, uint64_t size);

// String slicing.
struct String8Slice
{
    uint64_t off = 0;
    uint64_t len = uint64_t(-1);
};

String8 str8_substr(String8 str, String8Slice slice);
String8 str8_chop_prefix(String8 str, String8 prefix);

// String trimming.
struct TrimResult
{
    uint64_t trimmed_start;
    uint64_t trimmed_end;
    String8 result;
};

TrimResult str8_trim_whitespace(String8 str);

// String searching.
inline constexpr uint64_t str8_index_sentinel = uint64_t(-1);
bool str8_match_exact(String8 a, String8 b);
uint64_t str8_find_last_of(String8 str, String8 pattern);
bool str8_ends_with(String8 str, String8 suffix);
bool str8_starts_with(String8 str, String8 prefix);

bool str16_match_exact(String16 a, String16 b);

struct BoyerMooreSearchData
{
    // Note: Make this 255 + 1 to account for cases where the target char is -1 (255).
    static constexpr uint64_t table_size = 0x100;
    char* pat_first;
    int64_t pat_sz; // The pattern size - 1, but cached so we don't have to compute it each time.
    int64_t shift[table_size];
};

void str8_boyer_moore_init(BoyerMooreSearchData* data, String8 pattern);
void str8_boyer_moore_ignore_case_init(BoyerMooreSearchData* data, String8 pattern);
// This is really the Boyer-Moore Horspool variant.
char* str8_boyer_moore_search(BoyerMooreSearchData* data, char* first, char* last);
char* str8_boyer_moore_search_ignore_case(BoyerMooreSearchData* data, char* first, char* last);

// String comparison.
int str8_compare(String8 a, String8 b);

// Formatting.
String8 fmt_string(char* buf, size_t count, const char* fmt, ...);
String8 fmt_string(char* buf, size_t count, const char* fmt, va_list lst);

template <int N>
std::string_view fmt_string_sv(char (&buf)[N], const char* fmt, ...)
{
    std::string_view result;
    va_list va;
    va_start(va, fmt);
    result = sv_str8(fmt_string(buf, N, fmt, va));
    va_end(va);
    return result;
}

template <int N>
String8 fmt_string(char (&buf)[N], const char* fmt, ...)
{
    String8 result;
    va_list va;
    va_start(va, fmt);
    result = fmt_string(buf, N, fmt, va);
    va_end(va);
    return result;
}

// Spliting/joining.
struct JoinStringsInput
{
    String8List strings;
    String8 start_sep;
    String8 sep;
    String8 end_sep;
};

enum class SplitStringsFlags
{
    None          = 0,
    NoResultClear = 1U << 0 // Do not clear 'result' on entering.  Useful for appending multiple results together.
};

struct SplitStringsInput
{
    String8 in;
    String8 seps;
    SplitStringsFlags flags;
};

String8 join_strings(Arena::Arena* arena, JoinStringsInput in);
void split_strings(Arena::Arena* arena, String8List* result, SplitStringsInput in);