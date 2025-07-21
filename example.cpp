#include <array>
#include <cmath>
#include <cstdio>

#include <utxt.h>

int main()
{
    constexpr int textbuf_width = 110;
    constexpr int textbuf_height = 150;

    // Load the font
    utxt_font* font = utxt_font_load_ttf({}, "NotoSans.ttf",
        { .size = 24, .atlas_size = 256, .oversampling_h = 1, .oversampling_v = 1 });
    if (!font) {
        std::printf("Could not load font: %s\n", utxt_get_last_error().data);
        return 1;
    }

    uint32_t atlas_width, atlas_height, atlas_channels;
    const uint8_t* atlas_data = utxt_get_atlas(font, &atlas_width, &atlas_height, &atlas_channels);

    // Layout the text
    utxt_layout* layout = utxt_layout_create({}, 256);
    const utxt_style style = { .font = font };
    utxt_layout_reset(layout, textbuf_width, UTXT_TEXT_ALIGN_LEFT);
    utxt_layout_add_text(layout, &style, UTXT_LITERAL("Hey, look at this cool text, that"));
    utxt_layout_add_text(layout, &style, UTXT_LITERAL(" is most likely taking up multiple lines."));
    utxt_layout_compute(layout);

    // Loop layout glyphs, get the quad and render them
    size_t num_glyphs = 0;
    const utxt_layout_glyph* glyphs = utxt_layout_get_glyphs(layout, &num_glyphs);
    // Glyphs are placed above the baseline and we can't handle negative positions, so we shift
    // down by the font's ascent so y is always >= 0.
    const float y_offset = utxt_get_font_metrics(font)->ascent;

    uint8_t textbuf[textbuf_width * textbuf_height] = {};
    for (size_t i = 0; i < num_glyphs; ++i) {
        utxt_quad q;
        utxt_layout_glyph_get_quad(&q, &glyphs[i], 0.0f, y_offset);

        const auto textbuf_x = (uint32_t)std::roundf(q.x);
        const auto textbuf_y = (uint32_t)std::roundf(q.y);
        const auto atlas_x = (uint32_t)std::roundf(q.u0 * (float)atlas_width);
        const auto atlas_y = (uint32_t)std::roundf(q.v0 * (float)atlas_height);
        const auto atlas_w = (uint32_t)std::roundf((q.u1 - q.u0) * (float)atlas_width);
        const auto atlas_h = (uint32_t)std::roundf((q.v1 - q.v0) * (float)atlas_height);
        for (uint32_t y = 0; y < atlas_h; ++y) {
            for (uint32_t x = 0; x < atlas_w; ++x) {
                const auto atlas_v = atlas_data[atlas_x + x + (atlas_y + y) * atlas_width];
                textbuf[textbuf_x + x + (textbuf_y + y) * textbuf_width] = atlas_v;
            }
        }
    }

    // Draw the final buffer
    for (int y = 0; y < textbuf_height; ++y) {
        for (int x = 0; x < textbuf_width; ++x) {
            // This cool trick is borrowed from stb_truetype
            putchar(" .:ioVM@"[textbuf[x + y * textbuf_width] >> 5]);
        }
        putchar('\n');
    }
}
