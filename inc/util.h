#pragma once

#include <concepts>

#include "arena.h"
#include "gap-strings.h"
#include "os.h"
#include "types.h"
#include "ui-common.h"
#include "vec.h"

enum class FileSizeMagnitude
{
    B,
    KB,
    MB,
    GB,
    TB,
    PB,
    EB,
};

struct HumanFileSize
{
    String8 string;
    FileSizeMagnitude mag;
};

// File handling.
bool read_entire_file(Arena::Arena* arena, String8* buf, String8 file_path);
bool save_file(String8 file_path, String8 buf);
String8 filename(String8 path);
String8 default_config_directory(Arena::Arena* arena);
String8 default_config_file(Arena::Arena* arena);
String8 directory_of_file(String8 file);
uint64_t depth_of_file(String8 file);

const Vec4f& mag_to_color(FileSizeMagnitude mag);
HumanFileSize to_human_readable_file_size(Arena::Arena* arena, OS::FileLength size);

struct FilesInDirList
{
    String8Node* first;
    String8Node* last;
    uint64_t count;
};
// Note: 'ext_filter' must have the '.'.
FilesInDirList files_in_dir(Arena::Arena* arena, String8 dir, String8 ext_filter);

// DPI requests.
float get_platform_dpi_pixel_ratio();

// Hashing.
struct HashResult
{
    // Could be extended to be 128-bit if necessary.
    uint64_t result[1];

    bool operator==(const HashResult&) const = default;
};

struct HashInput
{
    const uint8_t* bytes;
    size_t len;
};

bool hash_bytes(HashInput in, HashResult* out);

template <typename T>
HashInput as_hash_input(const T& x)
{
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&x);
    return { .bytes = bytes, .len = sizeof(T) };
}

inline bool hash_str8(String8 str, HashResult* out)
{
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(str.str);
    HashInput in{
        .bytes = bytes,
        .len = str.size
    };
    return hash_bytes(in, out);
}

// Bit manipulations.
SUPPRESS_MEMSET_NON_TRIVIAL_WARNING();
template <typename T>
void zero_bytes(T* x, uint64_t count = 1)
{
    memset(x, 0, sizeof(T) * count);
}
ENABLE_MEMSET_NON_TRIVIAL_WARNING();

// Color handling.
Vec4f rgb_to_hsv(Vec4f color);
Vec4f hsv_to_rgb(Vec4f hsv);

struct HSLInput
{
    float h;  // Hue.
    float s;  // Sat.
    float l;  // Lum.
};

Vec4f hsl_to_rgb(HSLInput in);

// General.
template <typename T, typename U>
concept Lerpable = requires(T s, U t) {
    { 1 - t };
    { s * t } -> std::convertible_to<T>;
    { s + s };
};

template <typename T, typename U>
requires Lerpable<T, U>
inline auto lerp(const T& start, const T& end, const U& mixin)
{
    return start * (1 - mixin) + end * mixin;
}

uint64_t digits(uint64_t n);