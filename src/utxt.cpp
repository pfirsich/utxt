#include "utxt.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <utility>

#include "stb_truetype.h"

#define EXPORT extern "C"

namespace utxt {
static std::string_view last_error;

static void* realloc(void* ptr, size_t, size_t new_size, void*)
{
    if (new_size) {
        return std::realloc(ptr, new_size);
    } else {
        std::free(ptr);
        return nullptr;
    }
}

template <typename T>
static T* allocate(utxt_alloc alloc, size_t count = 1)
{
    auto ptr = (T*)alloc.realloc(nullptr, 0, sizeof(T) * count, alloc.ctx);
    for (size_t i = 0; i < count; ++i) {
        new (ptr + i) T {};
    }
    return ptr;
}

template <typename T>
static void deallocate(utxt_alloc alloc, T* ptr, size_t count = 1)
{
    if (!ptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        (ptr + i)->~T();
    }
    alloc.realloc(ptr, sizeof(T) * count, 0, alloc.ctx);
}

EXPORT utxt_string utxt_get_last_error()
{
    return { .data = last_error.data(), .len = last_error.size() };
}

template <typename T>
struct AutoFree {
    utxt_alloc alloc;
    T* ptr;
    size_t count;

    ~AutoFree()
    {
        deallocate(alloc, ptr, count);
        ptr = nullptr;
    }

    T* release() { return std::exchange(ptr, nullptr); }
};

static uint8_t* read_file(utxt_alloc alloc, const char* path, size_t* size)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return nullptr;
    }
    std::fseek(f, 0, SEEK_END);
    const auto ssize = std::ftell(f);
    if (ssize < 0) {
        return nullptr;
    }
    *size = (size_t)ssize;
    std::fseek(f, 0, SEEK_SET);
    auto buf = allocate<uint8_t>(alloc, *size);
    const auto n = std::fread(buf, 1, *size, f);
    if (n != *size) {
        deallocate(alloc, buf, *size);
        return nullptr;
    }
    return buf;
}

struct Font {
    utxt_alloc alloc;
    uint8_t* atlas_data;
    uint32_t atlas_width;
    uint32_t atlas_height;
    uint32_t atlas_channels;
    utxt_font_metrics metrics;
    utxt_glyph* glyphs;
    size_t num_glyphs;
    // There is a separate array for codepoint -> glyph lookup, because we need to do it A LOT and
    // it should be fast.
    uint32_t* glyph_codepoints;
    utxt_kerning_pair* kerning_pairs;
    size_t num_kerning_pairs;
};

static uint32_t sort_key(uint32_t v)
{
    return v;
}

static uint64_t sort_key(const utxt_kerning_pair& pair)
{
    // Table is sorted by first_glyph, then second_glyph.
    // So we can combine both into a single key.
    return ((uint64_t)pair.first_glyph << 32) | pair.second_glyph;
}

template <typename T>
static bool is_sorted(std::span<const T> values)
{
    for (size_t i = 0; i < values.size() - 1; ++i) {
        if (sort_key(values[i]) >= sort_key(values[i + 1])) {
            return false;
        }
    }
    return true;
}

static uint32_t default_code_point_ranges[4] = {
    0x20, 0x7f, // Basic Latin
    0xa0, 0xff, // Latin-1 Supplement
};

EXPORT utxt_font* utxt_font_load_ttf_buffer(
    utxt_alloc alloc, const uint8_t* buffer, size_t, utxt_load_ttf_params params)
{
    if (!alloc.realloc) {
        alloc = { realloc, nullptr };
    }
    if (!params.code_point_ranges) {
        params.code_point_ranges = default_code_point_ranges;
        params.num_code_point_ranges = std::size(default_code_point_ranges) / 2;
    }
    params.oversampling_h = params.oversampling_h ? params.oversampling_h : 2;
    params.oversampling_v = params.oversampling_v ? params.oversampling_v : 2;

    const auto num_fonts = stbtt_GetNumberOfFonts(buffer);
    if (num_fonts <= 0) {
        last_error = "No fonts in file";
        return nullptr;
    }
    if (params.font_index >= (uint32_t)num_fonts) {
        last_error = "Font index out of range";
        return nullptr;
    }
    const auto font_offset = stbtt_GetFontOffsetForIndex(buffer, (int)params.font_index);
    if (font_offset < 0) {
        last_error = "Invalid font index";
        return nullptr;
    }

    stbtt_fontinfo font_info;
    if (!stbtt_InitFont(&font_info, buffer, font_offset)) {
        last_error = "Could not load font";
        return nullptr;
    }

    const auto atlas_data_size = params.atlas_size * params.atlas_size;
    auto atlas_data = allocate<uint8_t>(alloc, atlas_data_size);
    auto atlas_data_autofree = AutoFree<uint8_t> { alloc, atlas_data, atlas_data_size };

    const auto num_pack_ranges = params.num_code_point_ranges;
    auto pack_ranges = allocate<stbtt_pack_range>(alloc, num_pack_ranges);
    auto pack_ranges_autofree = AutoFree<stbtt_pack_range> { alloc, pack_ranges, num_pack_ranges };

    size_t num_packed_chars = 0;
    for (size_t i = 0; i < num_pack_ranges; ++i) {
        const auto cp_first = params.code_point_ranges[i * 2 + 0];
        const auto cp_last = params.code_point_ranges[i * 2 + 1];
        // in stb_truetype it seems every codepoint maps to a single glyph and
        // that there a no multi-codepoint glyphs
        num_packed_chars += cp_last - cp_first + 1;
    }

    auto packed_chars = allocate<stbtt_packedchar>(alloc, num_packed_chars);
    auto packed_chars_autofree
        = AutoFree<stbtt_packedchar> { alloc, packed_chars, num_packed_chars };

    size_t pc_index = 0;
    for (size_t i = 0; i < num_pack_ranges; ++i) {
        const auto cp_first = params.code_point_ranges[i * 2 + 0];
        const auto cp_last = params.code_point_ranges[i * 2 + 1];
        // sorted and disjunct
        assert(i == 0 || cp_first > params.code_point_ranges[(i - 1) * 2 + 1]);
        const auto cp_count = cp_last - cp_first + 1;
        pack_ranges[i] = {
            .font_size = params.size,
            .first_unicode_codepoint_in_range = (int)cp_first,
            .array_of_unicode_codepoints = nullptr,
            .num_chars = (int)cp_count,
            .chardata_for_range = packed_chars + pc_index,
        };
        pc_index += cp_count;
    }

    stbtt_pack_context pack_ctx;
    const int padding = 1;
    if (!stbtt_PackBegin(&pack_ctx, atlas_data, (int)params.atlas_size, (int)params.atlas_size, 0,
            padding, nullptr)) {
        last_error = "Failed to initialize packing context";
        return nullptr;
    }
    stbtt_PackSetOversampling(&pack_ctx, params.oversampling_h, params.oversampling_v);
    const auto ret = stbtt_PackFontRanges(
        &pack_ctx, buffer, (int)params.font_index, pack_ranges, (int)num_pack_ranges);
    if (!ret) {
        last_error = "Failed to pack character bitmaps";
        return nullptr;
    }
    stbtt_PackEnd(&pack_ctx);

    // If we got here, we won't fail anymore. Let's create the font object
    auto font = allocate<Font>(alloc);
    font->alloc = alloc;
    font->atlas_data = atlas_data_autofree.release();
    font->atlas_width = params.atlas_size;
    font->atlas_height = params.atlas_size;
    font->atlas_channels = 1;

    const auto scale = stbtt_ScaleForPixelHeight(&font_info, (float)params.size);

    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &line_gap);
    font->metrics.ascent = std::roundf(scale * (float)ascent);
    font->metrics.descent = std::roundf(scale * (float)descent);
    font->metrics.line_gap = std::roundf(scale * (float)line_gap);
    font->metrics.line_height = std::roundf(scale * (float)(ascent - descent + line_gap));

    font->num_glyphs = num_packed_chars;
    font->glyphs = allocate<utxt_glyph>(alloc, font->num_glyphs);
    font->glyph_codepoints = allocate<uint32_t>(alloc, font->num_glyphs);

    size_t glyph_idx = 0;
    for (size_t i = 0; i < num_pack_ranges; ++i) {
        const auto cp_first = params.code_point_ranges[i * 2 + 0];
        const auto cp_last = params.code_point_ranges[i * 2 + 1];
        const auto cp_count = cp_last - cp_first + 1;

        for (uint32_t i = 0; i < cp_count; ++i) {
            const auto cp = cp_first + i;
            const auto& pc = packed_chars[glyph_idx];
            const auto font_glyph_idx = stbtt_FindGlyphIndex(&font_info, (int)cp);
            font->glyphs[glyph_idx] = {
                .codepoint = cp,
                .glyph_index = (uint32_t)font_glyph_idx,
                .bearing_x = pc.xoff,
                .bearing_y = pc.yoff,
                .width = (pc.xoff2 - pc.xoff),
                .height = (pc.yoff2 - pc.yoff),
                .advance = pc.xadvance,
                .u0 = pc.x0 / (float)params.atlas_size,
                .v0 = pc.y0 / (float)params.atlas_size,
                .u1 = pc.x1 / (float)params.atlas_size,
                .v1 = pc.y1 / (float)params.atlas_size,
            };
            // NOTE: glyph index 0 is missing glyph symbols! (TrueType spec)
            // so it is possible we packed the missing glyph for this codepoint
            font->glyph_codepoints[glyph_idx] = cp;
            glyph_idx++;
        }
    }
    assert(is_sorted<uint32_t>({ font->glyph_codepoints, font->num_glyphs }));

    font->num_kerning_pairs = (size_t)stbtt_GetKerningTableLength(&font_info);
    font->kerning_pairs = nullptr;
    if (font->num_kerning_pairs > 0) {
        font->kerning_pairs = allocate<utxt_kerning_pair>(alloc, font->num_kerning_pairs);
        auto* table = allocate<stbtt_kerningentry>(alloc, font->num_kerning_pairs);
        stbtt_GetKerningTable(&font_info, table, (int)font->num_kerning_pairs);
        for (size_t i = 0; i < font->num_kerning_pairs; ++i) {
            font->kerning_pairs[i] = {
                .first_glyph = (uint32_t)table[i].glyph1,
                .second_glyph = (uint32_t)table[i].glyph2,
                .amount = scale * (float)table[i].advance,
            };
        }
        deallocate(alloc, table, font->num_kerning_pairs);
        // Make sure it's sorted.
        // Accoring to the docs the table is sorted by glyph1, then glyph2.
        assert(is_sorted<utxt_kerning_pair>({ font->kerning_pairs, font->num_kerning_pairs }));
    }

    return (utxt_font*)font;
}

EXPORT utxt_font* utxt_font_load_ttf(
    utxt_alloc alloc, const char* path, utxt_load_ttf_params params)
{
    if (!alloc.realloc) {
        alloc = { realloc, nullptr };
    }

    size_t file_size = 0;
    auto file_data = read_file(alloc, path, &file_size);
    if (!file_data) {
        last_error = "Could not read file";
        return nullptr;
    }
    auto file_data_autofree = AutoFree<uint8_t> { alloc, file_data, file_size };

    return utxt_font_load_ttf_buffer(alloc, file_data, file_size, params);
}

EXPORT utxt_font* utxt_font_create(utxt_alloc alloc, utxt_font_create_params params)
{
    if (!alloc.realloc) {
        alloc = { realloc, nullptr };
    }

    auto font = allocate<Font>(alloc);
    font->alloc = alloc;

    if (params.atlas_data) {
        font->atlas_width = params.atlas_width;
        font->atlas_height = params.atlas_height;
        font->atlas_channels = params.atlas_channels ? params.atlas_channels : 1;
        const auto atlas_size_bytes = font->atlas_width * font->atlas_height * font->atlas_channels;
        font->atlas_data = allocate<uint8_t>(alloc, atlas_size_bytes);
        std::memcpy(font->atlas_data, params.atlas_data, atlas_size_bytes);
    }

    font->metrics = params.metrics;

    assert(params.glyphs);
    font->num_glyphs = params.num_glyphs;
    font->glyphs = allocate<utxt_glyph>(alloc, font->num_glyphs);
    std::memcpy(font->glyphs, params.glyphs, font->num_glyphs * sizeof(utxt_glyph));

    font->glyph_codepoints = allocate<uint32_t>(alloc, font->num_glyphs);
    for (size_t i = 0; i < font->num_glyphs; ++i) {
        font->glyph_codepoints[i] = font->glyphs[i].codepoint;
    }
    assert(is_sorted<uint32_t>({ font->glyph_codepoints, font->num_glyphs }));

    if (params.kerning_pairs) {
        font->num_kerning_pairs = params.num_kerning_pairs;
        font->kerning_pairs = allocate<utxt_kerning_pair>(alloc, font->num_kerning_pairs);
        std::memcpy(font->kerning_pairs, params.kerning_pairs,
            font->num_kerning_pairs * sizeof(utxt_kerning_pair));
        assert(is_sorted<utxt_kerning_pair>({ font->kerning_pairs, font->num_kerning_pairs }));
    }

    return (utxt_font*)font;
}

EXPORT void utxt_font_free(utxt_font* font)
{
    auto fnt = (Font*)font;
    const auto atlas_data_size = fnt->atlas_width * fnt->atlas_height * fnt->atlas_channels;
    deallocate(fnt->alloc, fnt->atlas_data, atlas_data_size);
    deallocate(fnt->alloc, fnt->glyphs, fnt->num_glyphs);
    deallocate(fnt->alloc, fnt->glyph_codepoints, fnt->num_glyphs);
    deallocate(fnt->alloc, fnt->kerning_pairs, fnt->num_kerning_pairs);
    deallocate(fnt->alloc, fnt);
}

EXPORT const uint8_t* utxt_get_atlas(
    const utxt_font* font, uint32_t* width, uint32_t* height, uint32_t* channels)
{
    auto& fnt = *(Font*)font;
    *width = fnt.atlas_width;
    *height = fnt.atlas_height;
    *channels = fnt.atlas_channels;
    return fnt.atlas_data;
}

EXPORT const utxt_font_metrics* utxt_get_font_metrics(const utxt_font* font)
{
    auto& fnt = *(Font*)font;
    return &fnt.metrics;
}

EXPORT const utxt_glyph* utxt_get_glyphs(const utxt_font* font, size_t* count)
{
    auto& fnt = *(Font*)font;
    *count = fnt.num_glyphs;
    return fnt.glyphs;
}

// https://hiandrewquinn.github.io/til-site/posts/binary-search-isn-t-about-search/
template <typename T>
static size_t binary_search(std::span<const T> haystack, const T& needle)
{
    const auto needle_key = sort_key(needle);

    size_t low = 0;
    size_t high = haystack.size(); // high is exclusive (open)
    // [low, high) is our current range
    // everything outside of this range has been excluded
    // below low all values are smaller than needle, above (and at) high all values are greater

    while (low < high) {
        const auto mid = low + (high - low) / 2;
        const auto mid_key = sort_key(haystack[mid]);
        if (mid_key < needle_key) {
            // everything below low is smaller, so we exclude everything below mid
            low = mid + 1;
        } else if (mid_key > needle_key) {
            // everythihg above and at high is greater, so we exclude above mid (including)
            high = mid;
        } else {
            // must be equal
            return mid;
        }
    }

    return haystack.size();
}

static utxt_glyph* find_glyph(Font& font, uint32_t cp)
{
    const auto idx = binary_search<uint32_t>({ font.glyph_codepoints, font.num_glyphs }, cp);
    if (idx >= font.num_glyphs) {
        return nullptr;
    }
    return &font.glyphs[idx];
}

const utxt_glyph* utxt_find_glyph(const utxt_font* font, uint32_t codepoint)
{
    auto& fnt = *(Font*)font;
    return find_glyph(fnt, codepoint);
}

EXPORT const utxt_kerning_pair* utxt_get_kerning_pairs(const utxt_font* font, size_t* count)
{
    auto& fnt = *(Font*)font;
    *count = fnt.num_kerning_pairs;
    return fnt.kerning_pairs;
}

EXPORT float utxt_get_kerning(const utxt_font* font, uint32_t first_glyph, uint32_t second_glyph)
{
    auto& fnt = *(Font*)font;
    const auto idx = binary_search<utxt_kerning_pair>(
        { fnt.kerning_pairs, fnt.num_kerning_pairs }, { first_glyph, second_glyph });
    if (idx >= fnt.num_kerning_pairs) {
        return 0.0f;
    }
    return fnt.kerning_pairs[idx].amount;
}

static uint32_t decode_utf8(utxt_string& s)
{
    if (s.len == 0) {
        return 0;
    }

    const auto u = (const uint8_t*)s.data;

    if (u[0] < 0x80) { // 1-byte sequence
        s = { s.data + 1, s.len - 1 };
        return u[0];
    } else if ((u[0] & 0xE0u) == 0xC0) { // 2-byte sequence
        if (s.len < 2 || (u[1] & 0xC0u) != 0x80)
            return 0;
        s = { s.data + 2, s.len - 2 };
        return ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu);
    } else if ((u[0] & 0xF0u) == 0xE0) { // 3-byte sequence
        if (s.len < 3 || (u[1] & 0xC0u) != 0x80 || (u[2] & 0xC0u) != 0x80)
            return 0;
        s = { s.data + 3, s.len - 3 };
        return ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
    } else if ((u[0] & 0xF8u) == 0xF0) { // 4-byte sequence
        if (s.len < 4 || (u[1] & 0xC0u) != 0x80 || (u[2] & 0xC0u) != 0x80 || (u[3] & 0xC0u) != 0x80)
            return 0;
        s = { s.data + 4, s.len - 4 };
        return ((u[0] & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12) | ((u[2] & 0x3Fu) << 6)
            | (u[3] & 0x3Fu);
    } else {
        return 0; // Invalid start byte
    }
}

static utxt_glyph* decode_glyph(Font& font, utxt_string& s)
{
    const auto cp = decode_utf8(s);
    if (cp == 0) {
        return nullptr;
    }
    return find_glyph(font, cp);
}

EXPORT float utxt_get_text_width(const utxt_font* font, utxt_string text)
{
    auto& fnt = *(Font*)font;

    float cursor = 0.0f;
    uint32_t prev_glyph_idx = 0; // for kerning
    const utxt_glyph* first = nullptr;
    const utxt_glyph* last = nullptr;

    while (text.len) {
        const auto glyph = decode_glyph(fnt, text);
        if (!glyph) {
            // code point invalid or not in font, skip and reset kerning
            prev_glyph_idx = 0;
            continue;
        }

        if (!first) {
            first = glyph;
        }
        last = glyph;

        if (prev_glyph_idx) {
            cursor += utxt_get_kerning(font, prev_glyph_idx, glyph->glyph_index);
        }

        cursor += glyph->advance;
        prev_glyph_idx = glyph->glyph_index;
    }

    if (!first) {
        return 0.0f;
    }

    const auto start = first->bearing_x;
    const auto end = cursor - last->advance + last->bearing_x + last->width;

    return end - start;
}

static size_t count_quads(const utxt_font* font, utxt_string string)
{
    auto& fnt = *(Font*)font;

    size_t quad_count = 0;

    while (string.len) {
        const auto glyph = decode_glyph(fnt, string);
        if (!glyph) {
            continue;
        }
        quad_count++;
    }

    return quad_count;
}

EXPORT size_t utxt_draw_text(
    utxt_quad* quads, size_t num_quads, const utxt_font* font, utxt_string text, float _x, float _y)
{
    auto& fnt = *(Font*)font;

    if (!quads) {
        return count_quads(font, text);
    }

    float cursor_x = _x;
    size_t quad_idx = 0;
    uint32_t prev_glyph_idx = 0; // for kerning

    while (text.len) {
        const auto glyph = decode_glyph(fnt, text);
        if (!glyph) {
            // code point invalid or not in font, skip and reset kerning
            prev_glyph_idx = 0;
            continue;
        }

        if (prev_glyph_idx) {
            cursor_x += utxt_get_kerning(font, prev_glyph_idx, glyph->glyph_index);
        }

        if (quad_idx >= num_quads) {
            return num_quads + 1;
        }

        const auto x = cursor_x + glyph->bearing_x;
        const auto y = _y + glyph->bearing_y;

        quads[quad_idx++]
            = { x, y, glyph->width, glyph->height, glyph->u0, glyph->v0, glyph->u1, glyph->v1 };

        cursor_x += glyph->advance;
        prev_glyph_idx = glyph->glyph_index;
    }

    return quad_idx;
}

struct Layout {
    utxt_alloc alloc;
    utxt_layout_glyph* lglyphs = nullptr;
    size_t num_lglyphs = 0;
    size_t lglyph_idx = 0;
    float wrap_width = 0.0f;
    utxt_text_align align;
    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    size_t line_start_idx = 0;
    float current_line_height = 0.0f;
};

EXPORT utxt_layout* utxt_layout_create(utxt_alloc alloc, uint32_t num_glyphs)
{
    if (!alloc.realloc) {
        alloc = { realloc, nullptr };
    }
    auto layout = allocate<Layout>(alloc);
    layout->alloc = alloc;
    layout->num_lglyphs = num_glyphs;
    if (layout->num_lglyphs) {
        layout->lglyphs = allocate<utxt_layout_glyph>(alloc, num_glyphs);
    }
    return (utxt_layout*)layout;
}

EXPORT void utxt_layout_free(utxt_layout* layout_)
{
    auto layout = (Layout*)layout_;
    deallocate(layout->alloc, layout->lglyphs, layout->num_lglyphs);
    deallocate(layout->alloc, layout);
}

EXPORT void utxt_layout_reset(utxt_layout* layout_, float wrap_width, utxt_text_align align)
{
    auto& layout = *(Layout*)layout_;
    layout.wrap_width = wrap_width;
    layout.align = align;
    layout.lglyph_idx = 0;
    layout.cursor_x = 0;
    layout.cursor_y = 0;
    layout.line_start_idx = 0;
    layout.current_line_height = 0.0f;
}

static bool is_whitespace(uint32_t cp)
{
    return cp == ' ' || cp == '\n' || cp == '\r';
}

// This returns the visual width of a span of layout glyphs, i.e. from the left edge of the first
// to the right edge of the last glyph.
static float get_width(std::span<const utxt_layout_glyph> lglyphs)
{
    assert(lglyphs.size() > 0);
    const auto first = lglyphs[0];
    const auto last = lglyphs[lglyphs.size() - 1];
    // The start position is cursor_x before the first glyph was added
    const auto start_x = first.x - (first.glyph ? first.glyph->bearing_x : 0.0f);
    const auto end_x = last.x + (last.glyph ? last.glyph->width : 0.0f);
    return end_x - start_x;
}

static void shift_glyphs(std::span<utxt_layout_glyph> lglyphs, float shift)
{
    for (auto& g : lglyphs) {
        g.x += shift;
    }
}

static void align_line(Layout& layout)
{
    const auto line = std::span { layout.lglyphs, layout.num_lglyphs }.subspan(
        layout.line_start_idx, layout.lglyph_idx - layout.line_start_idx);
    if (layout.align == UTXT_TEXT_ALIGN_CENTER) {
        shift_glyphs(line, layout.wrap_width / 2.0f - get_width(line) / 2.0f);
    } else if (layout.align == UTXT_TEXT_ALIGN_RIGHT) {
        shift_glyphs(line, layout.wrap_width - get_width(line));
    }
}

static void break_current_line(Layout& layout, Font& font)
{
    layout.cursor_x = 0.0f;
    layout.cursor_y += layout.current_line_height;
    align_line(layout);
    layout.line_start_idx = layout.lglyph_idx;
    layout.current_line_height = font.metrics.line_height;
}

static bool flush_chunk(
    Layout& layout, Font& font, std::span<const utxt_layout_glyph> lglyphs, float chunk_advance)
{
    if (lglyphs.size() == 0) {
        return true;
    }

    if (layout.lglyph_idx + lglyphs.size() > layout.num_lglyphs) {
        return false;
    }
    const auto chunk_width = get_width(lglyphs);

    if (layout.cursor_x > 0.0f && layout.cursor_x + chunk_width > layout.wrap_width) {
        // chunk does not fit in current line, so break first
        break_current_line(layout, font);
    }

    for (size_t i = 0; i < lglyphs.size(); ++i) {
        auto& g = layout.lglyphs[layout.lglyph_idx++] = lglyphs[i];
        g.x += layout.cursor_x;
        g.y += layout.cursor_y;
    }

    // We advance by chunk_advance here, as that is the sum of advances that should actually
    // separate glyphs. If we were to use chunk_width then we would be off by (advance - width).
    layout.cursor_x += chunk_advance;

    return true;
}

EXPORT size_t utxt_layout_add_text(utxt_layout* layout_, const utxt_style* style, utxt_string text)
{
    auto& layout = *(Layout*)layout_;
    auto& font = *(Font*)style->font;

    const auto space_glyph = find_glyph(font, ' ');
    assert(space_glyph);
    const auto space_advance = space_glyph->advance;
    layout.current_line_height = std::fmax(layout.current_line_height, font.metrics.line_height);

    uint32_t prev_glyph_idx = 0; // for kerning
    std::array<utxt_layout_glyph, 128> chunk;
    size_t chunk_idx = 0;
    float chunk_cursor_x = 0.0f;

    while (text.len) {
        const auto cp = decode_utf8(text);
        if (cp == 0) {
            // skip and reset kerning
            prev_glyph_idx = 0;
            continue;
        }

        if (is_whitespace(cp)) {
            if (!flush_chunk(layout, font, std::span { chunk }.first(chunk_idx), chunk_cursor_x)) {
                break;
            }
            chunk_idx = 0;
            chunk_cursor_x = 0.0f;

            if (cp == '\n') {
                break_current_line(layout, font);
            } else if (cp == ' ') {
                // Only advance cursor for space if it's not at the beginning of a line
                if (layout.cursor_x > 0.0f) {
                    layout.cursor_x += space_advance;
                }
            }
            prev_glyph_idx = 0;
            continue;
        }

        const auto glyph = find_glyph(font, cp);
        if (!glyph) {
            // skip and reset kerning
            prev_glyph_idx = 0;
            continue;
        }

        assert(chunk_idx < chunk.size());

        if (prev_glyph_idx) {
            chunk_cursor_x += utxt_get_kerning(style->font, prev_glyph_idx, glyph->glyph_index);
        }
        prev_glyph_idx = glyph->glyph_index;

        chunk[chunk_idx++] = { style, glyph, chunk_cursor_x + glyph->bearing_x, glyph->bearing_y };

        chunk_cursor_x += glyph->advance;
    }

    flush_chunk(layout, font, std::span { chunk }.first(chunk_idx), chunk_cursor_x);

    return layout.lglyph_idx;
}

EXPORT void utxt_layout_compute(utxt_layout* layout_)
{
    auto& layout = *(Layout*)layout_;
    align_line(layout);
}

EXPORT utxt_layout_glyph* utxt_layout_get_glyphs(utxt_layout* layout_, size_t* count)
{
    auto& layout = *(Layout*)layout_;
    *count = layout.lglyph_idx;
    return layout.lglyphs;
}

EXPORT void utxt_layout_glyph_get_quads(
    const utxt_layout_glyph* layout_glyphs, size_t num_glyphs, utxt_quad* quads, float x, float y)
{
    for (size_t i = 0; i < num_glyphs; ++i) {
        const auto& lg = layout_glyphs[i];
        const auto& fg = *lg.glyph;
        quads[i] = { x + lg.x, y + lg.y, fg.width, fg.height, fg.u0, fg.v0, fg.u1, fg.v1 };
    }
}
}
