#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* data;
    size_t len;
} utxt_string;

#define UTXT_LITERAL(s) { s, sizeof(s) - 1 }
utxt_string utxt_zstr(const char* str); // calls strlen for length

utxt_string utxt_get_last_error();

typedef void* (*utxt_realloc)(void* ptr, size_t old_size, size_t new_size, void* ctx);

typedef struct {
    utxt_realloc realloc;
    void* ctx;
} utxt_alloc;

/*
   This library takes a sequence of utf8 encoded unicode code points (text), maps them to glyphs
   in a font and tells you where to draw those glyphs as quads with texture coordinates.

   When drawing a glyph is at (x, y), the given position only specifies the origin.
   The y coordinate of the origin determines the baseline, which is the bottom of uppercase
   characters. The bounding box of the glyph itself can extend as shown in the diagram below.

   Positive y is DOWN.

    |                width
    |        |--------------------|
    |
    |bearing_x
    |--------|
    |
    |         --------------------   -            -
    |        |   ggggggggg   ggggg|  |            |
    |        |  g:::::::::ggg::::g|  |            |
    |        | g:::::::::::::::::g|  |            |
    |        |g::::::ggggg::::::gg|  |            |
    |        |g:::::g     g:::::g |  |            |
    |        |g:::::g     g:::::g |  | bearing_y  |
    |        |g:::::g     g:::::g |  |            |
    |        |g::::::g    g:::::g |  |            |
    |        |g:::::::ggggg:::::g |  |            |
    |        | g::::::::::::::::g |  |            |
    |        |  gg::::::::::::::g |  |            |
    |        |    gggggggg::::::g |  |            |
    o--------|------------g-----g-|-----o--       |
    | origin |gggggg      g:::::g |               |
    |        |g:::::gg   gg:::::g |        height |
    |        | g::::::ggg:::::::g |               |
    |        |  gg:::::::::::::g  |               |
    |        |    ggg::::::ggg    |               |
    |        |       gggggg       |               |
    |         --------------------                -
    |
    |-----------------------------------|
    |              advance

   Note that it is possible for the bounding box to extend to the left of the origin.

   Since the glyphs will extend to the top and bottom of the baseline, you should likely
   use the font's ascent and descent (see below) to position the font vertically.
 */

typedef struct {
    uint32_t codepoint;
    // the index of the glyph in the font file, not in the array returned by get_glyphs
    uint32_t glyph_index;
    float bearing_x, bearing_y;
    float width, height;
    float advance;
    float u0, v0, u1, v1;
} utxt_glyph;

typedef struct {
    float ascent; // maximum height above baseline of all glyphs
    float descent; // maximum height below baseline of all glyphs
    float line_gap; // spacing between one row's descent and the next row's ascent
    float line_height; // ascent - descent + line_gap; the baseline distance between lines
} utxt_font_metrics;

typedef struct {
    uint32_t first_glyph;
    uint32_t second_glyph;
    float amount;
} utxt_kerning_pair;

typedef struct utxt_font utxt_font;

typedef struct {
    float size; // The target vertical extent in pixels (ascent - descent)
    uint32_t atlas_size; // must be power-of-two
    uint32_t font_index;
    uint32_t oversampling_h; // default: 2
    uint32_t oversampling_v; // default: 2
    // Default range is Basic Latin (0x20-0x7F) and Latin-1 Supplement (0xA0-0xFF)
    // The ranges must be sorted and must not overlap
    const uint32_t* code_point_ranges; // pairs of unicode codepoints
    size_t num_code_point_ranges; // number of pairs
} utxt_load_ttf_params;

utxt_font* utxt_font_load_ttf_buffer(
    utxt_alloc alloc, const uint8_t* data, size_t size, utxt_load_ttf_params params);

utxt_font* utxt_font_load_ttf(utxt_alloc alloc, const char* path, utxt_load_ttf_params params);

typedef struct {
    const uint8_t* atlas_data; // may be NULL
    uint32_t atlas_width;
    uint32_t atlas_height;
    uint32_t atlas_channels; // default: 1, if atlas_data is not NULL
    utxt_font_metrics metrics;
    const utxt_glyph* glyphs; // must be sorted by codepoint
    size_t num_glyphs;
    const utxt_kerning_pair* kerning_pairs; // may be NULL, must be sorted by first the second
    size_t num_kerning_pairs;
} utxt_font_create_params;

utxt_font* utxt_font_create(utxt_alloc alloc, utxt_font_create_params params);

void utxt_font_free(utxt_font* font);

const uint8_t* utxt_get_atlas(
    const utxt_font* font, uint32_t* width, uint32_t* height, uint32_t* channels);

const utxt_font_metrics* utxt_get_font_metrics(const utxt_font* font);

const utxt_glyph* utxt_get_glyphs(const utxt_font* font, size_t* count);
const utxt_glyph* utxt_find_glyph(const utxt_font* font, uint32_t codepoint);

const utxt_kerning_pair* utxt_get_kerning_pairs(const utxt_font* font, size_t* count);
float utxt_get_kerning(const utxt_font* font, uint32_t first_glyph, uint32_t second_glyph);

// This returns the visual width of the text, i.e. from the left edge of the first glyph's bounding
// box to the right edge of the last glyph's bounding box.
// This means the function is not linear (i.e. get_width(a + b) != get_width(a) + get_width(b)).
float utxt_get_text_width(const utxt_font* font, utxt_string string);

typedef struct {
    float x, y, w, h;
    float u0, v0, u1, v1;
} utxt_quad;

// This function generates quads for a single line of text. It does not handle wrapping or newline
// chracters. Use utxt_layout API for everything else.
// Returns number of quads. If quads is NULL, returns the number of quads that would have been
// generated.
size_t utxt_draw_text(
    utxt_quad* quads, size_t num_quads, const utxt_font* font, utxt_string text, float x, float y);

// Fancy text layouting API for all sorts of stuff (dialogue boxes, embedding symbols in text,
// embedded markup, etc.).
// Note that you only have to (and want to) layout the text when it changes, not every frame.

typedef struct utxt_layout utxt_layout;

utxt_layout* utxt_layout_create(utxt_alloc alloc, uint32_t num_glyphs);
void utxt_layout_free(utxt_layout* layout);

typedef enum {
    UTXT_TEXT_ALIGN_LEFT = 0,
    UTXT_TEXT_ALIGN_CENTER = 1,
    UTXT_TEXT_ALIGN_RIGHT = 2,
} utxt_text_align;

// Use user_data to point to a struct with e.g. color information or text effects.
// Then after layouting use that information and x, y from utxt_layout_glyph to render.
typedef struct {
    const utxt_font* font;
    void* user_data;
} utxt_style;

void utxt_layout_reset(utxt_layout* layout, float wrap_width, utxt_text_align align);
// returns number of added glyphs, text is utf8.
// No kerning will be added for subsequent calls of this function.
// This function will wrap individiual words (i.e. by whitespace).
size_t utxt_layout_add_text(utxt_layout* layout, const utxt_style* style, utxt_string text);
// This function will wrap individual glyphs. Use this for e.g. CJK.
size_t utxt_layout_add_glyphs(utxt_layout* layout, const utxt_style* style, utxt_string text);
// Computes the final positions of all added glyphs (e.g. applies text alignment).
// It should be called after all text has been added and before getting the layout glyphs.
void utxt_layout_compute(utxt_layout* layout);

typedef struct {
    const utxt_style* style;
    const utxt_glyph* glyph;
    float x, y;
} utxt_layout_glyph;

// The returned pointer is valid until the next add_*, compute, reset or free.
// You get to modify these before turning them into quads, so you can apply text effects that
// displace glyphs (like wave or shake).
utxt_layout_glyph* utxt_layout_get_glyphs(utxt_layout* layout, size_t* count);

void utxt_layout_glyph_get_quad(utxt_quad* quad, const utxt_layout_glyph* glyph, float x, float y);

#ifdef __cplusplus
}
#endif
