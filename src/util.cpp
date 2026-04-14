#include "util.h"

#include <stdio.h>

#include <cassert>
#include <cmath>

#include <algorithm>

#define XXH_INLINE_ALL
#define XXH_IMPLEMENTATION
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

#include "config.h"
#include "os.h"
#include "scoped-handle.h"
#include "gap-strings.h"

namespace
{
    constexpr bool is_slash(char c)
    {
        return c == '\\' or c == '/';
    }

    // Helpful color functions.
    // Ignore alpha.
    float max_color(Vec4f color)
    {
        return std::max(color.x, std::max(color.y, color.z));
    }

    float min_color(Vec4f color)
    {
        return std::min(color.x, std::min(color.y, color.z));
    }
} // namespace [anon]

bool read_entire_file(Arena::Arena* arena, String8* buf, String8 file_path)
{
    auto file = OS::open_file(file_path, OS::FileAccess::Read | OS::FileAccess::ShareRead);
    if (file == OS::FileHandle::Sentinel)
        return false;
    auto file_size = OS::file_length(file);
    auto read_len = OS::read_file(arena, buf, file, file_size);
    bool success = read_len == file_size;
    OS::close_file(file);
    return success;
}

bool save_file(String8 file_path, String8 buf)
{
    auto file = OS::open_file(file_path, OS::FileAccess::Write | OS::FileAccess::ShareWrite);
    if (file == OS::FileHandle::Sentinel)
        return false;
    auto written = OS::write_file(file, OS::FileOffset{ 0 }, buf);
    bool success = rep(written) == buf.size;
    OS::close_file(file);
    return success;
}

// The filename component is the final component along the path.  Let's find
// the final 'slash' and return the view to the end.
String8 filename(String8 path)
{
    // Start at the end and iterate backwards.
    uint64_t last_slash_idx = str8_index_sentinel;
    for (int64_t i = path.size - 1; i >= 0; --i)
    {
        if (is_slash(path.str[i]))
        {
            last_slash_idx = static_cast<uint64_t>(i);
            break;
        }
    }

    if (last_slash_idx != str8_index_sentinel)
    {
        // Ensure we chop the slash.
        path = str8_substr(path, { .off = last_slash_idx + 1 });
    }
    return path;
}

String8 default_config_directory(Arena::Arena* arena)
{
    // Not really sure what my 'org' is, but I'll just use my alias for now...
    String8 app_path_str = str8_empty;
    auto err = OS::app_path(arena, &app_path_str, str8_mut(str8_literal("cadacama")), str8_mut(str8_literal("gap")));
    if (err != OS::Error::None)
    {
        fprintf(stderr, "ERROR: Unable to resolve app path: %s\n", OS::error_text(err).str);
    }
    return app_path_str;
}

const Vec4f& mag_to_color(FileSizeMagnitude mag)
{
    const auto& colors = Config::widget_colors();
    switch (mag)
    {
    case FileSizeMagnitude::B:
        return colors.B_color;
    case FileSizeMagnitude::KB:
        return colors.KB_color;
    case FileSizeMagnitude::MB:
        return colors.MB_color;
    case FileSizeMagnitude::GB:
        return colors.GB_color;
    case FileSizeMagnitude::TB:
        return colors.TB_color;
    case FileSizeMagnitude::PB:
        return colors.PB_color;
    case FileSizeMagnitude::EB:
        return colors.EB_color;
    }
    return colors.B_color;
}

HumanFileSize to_human_readable_file_size(Arena::Arena* arena, OS::FileLength size)
{
    HumanFileSize result{};
    int order = 0;
    double mantissa = static_cast<double>(size);
    for (; mantissa >= 1024.; mantissa /= 1024., ++order);
    char c = "BKMGTPE"[order];
    // We shouldn't get strings longer than this.
    char fmt_buf[100];
    String8 file_size = str8_empty;
    if (order == 0)
    {
        file_size = fmt_string(fmt_buf, "%dB", int(size));
    }
    else
    {
        file_size = fmt_string(fmt_buf, "%.1f%cB", float(mantissa), c);
    }
    result.string = str8_copy(arena, file_size);
    result.mag = static_cast<FileSizeMagnitude>(order);
    return result;
}

String8 default_config_file(Arena::Arena* arena)
{
    auto scratch = Arena::scratch_begin({ &arena, 1 });
    String8 cfg_dir = default_config_directory(scratch.arena);
    String8 result = OS::combine_paths(arena, cfg_dir, str8_mut(str8_literal("config.toml")));
    Arena::scratch_end(scratch);
    return result;
}

String8 directory_of_file(String8 file_path)
{
    if (file_path.size == 0)
        return file_path;
    // Assume there is no last slash.
    uint64_t last_slash = str8_index_sentinel;
    for EachIndex(i, file_path.size)
    {
        if (file_path.str[i] == '/' or file_path.str[i] == '\\')
        {
            last_slash = i;
        }
    }
    file_path = str8_substr(file_path, { .len = last_slash + 1 });
    return file_path;
}

uint64_t depth_of_file(String8 file)
{
    uint64_t result = 0;
    for EachIndex(i, file.size)
    {
        result += file.str[i] == '/' or file.str[i] == '\\';
    }
    return result;
}

float get_platform_dpi_pixel_ratio()
{
    OS::DPI dpi = OS::monitor_dpi();
    if (rep(dpi) == 0)
        return 1.f;
    constexpr float standard_dpi = 96.f;
    return standard_dpi / rep(dpi);
}

FilesInDirList files_in_dir(Arena::Arena* arena, String8 dir, String8 ext_filter)
{
    FilesInDirList result{};
    auto scratch = Arena::scratch_begin({ &arena, 1 });
    String8 canon_dir = str8_empty;
    OS::Error err = OS::canonical_file_path(scratch.arena, &canon_dir, dir);
    bool precheck = err == OS::Error::None and OS::directory_exists(canon_dir);
    if (not precheck)
    {
        Arena::scratch_end(scratch);
        return result;
    }
    OS::DirIter os_itr = OS::open_dir_iter(dir, OS::DirIterFlags::SkipDirs | OS::DirIterFlags::FullPath);
    if (os_itr == OS::DirIter::Sentinel)
    {
        Arena::scratch_end(scratch);
        return result;
    }
    OS::DirIterResult item;
    do
    {
        if (not OS::dir_iter_next(scratch.arena, &item, os_itr))
            break;
        if (not str8_match_exact(OS::file_extension(item.path), ext_filter))
            continue;
        String8Node* node = Arena::push_array<String8Node>(arena, 1);
        node->string = str8_copy(arena, item.path);
        SLLQueuePush(result.first, result.last, node);
        ++result.count;
    } while (true);
    OS::close_dir_iter(os_itr);
    Arena::scratch_end(scratch);
    return result;
}

// Hashing.
bool hash_bytes(HashInput in, HashResult* out)
{
    out->result[0] = XXH3_64bits(in.bytes, in.len);
    return true;
}

// Color handling.
Vec4f rgb_to_hsv(Vec4f color)
{
    // see https://en.wikipedia.org/wiki/HSL_and_HSV#Formal_derivation
    // Our color is already in the range [0, 1] so all we need are the
    // min and max values.
    float max = max_color(color);
    float min = min_color(color);
    float d = max - min;
    float v = max;

    float sat = 0.f;
    float hue = 0.f;
    if (max != 0.f)
    {
        sat = d / max;
    }

    if (max != min)
    {
        if (max == color.x)
        {
            hue = (color.y - color.z) / d;
            if (color.y < color.z)
            {
                hue += 6.f;
            }
        }
        else if (max == color.y)
        {
            hue = (color.z - color.x) / d + 2.f;
        }
        else if (max == color.z)
        {
            hue = (color.x - color.y) / d + 4.f;
        }
        hue /= 6.f;
    }
    return { hue, sat, v, color.a };
}

Vec4f hsv_to_rgb(Vec4f hsv)
{
    float h = hsv.x;
    float s = hsv.y;
    float v = hsv.z;

    int i = static_cast<int>(std::floor(h * 6.f));

    float f = h * 6.f - i;
    float p = v * (1.f - s);
    float q = v * (1.f - f * s);
    float t = v * (1.f - (1.f - f) * s);

    int rem = i % 6;

    Vec4f result = hsv;

    switch (rem)
    {
    case 0:
        result.x = v;
        result.y = t;
        result.z = p;
        break;
    case 1:
        result.x = q;
        result.y = v;
        result.z = p;
        break;
    case 2:
        result.x = p;
        result.y = v;
        result.z = t;
        break;
    case 3:
        result.x = p;
        result.y = q;
        result.z = v;
        break;
    case 4:
        result.x = t;
        result.y = p;
        result.z = v;
        break;
    case 5:
        result.x = v;
        result.y = p;
        result.z = q;
        break;
    default:
        // We mod by 6 so no other case is possible.
        assert(false);
    }
    return result;
}

Vec4f hsl_to_rgb(HSLInput in)
{
    Vec4f result;
    const float h_mul = in.h * 6.f;
    result.x = std::clamp(fabs(fmod(h_mul, 6.f) - 3.f) - 1.f, 0.f, 1.f);
    result.y = std::clamp(fabs(fmod(h_mul + 4.f, 6.f) - 3.f) - 1.f, 0.f, 1.f);
    result.z = std::clamp(fabs(fmod(h_mul + 2.f, 6.f) - 3.f) - 1.f, 0.f, 1.f);

    result.x = in.l + in.s * (result.x - .5f) * (1.f - fabs(2.f * in.l - 1.f));
    result.y = in.l + in.s * (result.y - .5f) * (1.f - fabs(2.f * in.l - 1.f));
    result.z = in.l + in.s * (result.z - .5f) * (1.f - fabs(2.f * in.l - 1.f));
    return result;
}

uint64_t digits(uint64_t n)
{
    return static_cast<uint64_t>(std::floor(std::log10(n) + 1));
}