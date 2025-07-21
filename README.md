# utxt

A small engine-agnostic text rendering library. 
It is a basic wrapper above stb_truetype and in the future will likely get support for loading BMFont files plus a basic text shaping library.

I start engine projects every few years and I don't want to redo stuff every time, so I made an engine-agnostic text library. 
The header should explain everything that is necessary to know.

## Notes

This library has a restriction that is copied from stb_truetype in that we have a one-to-one mapping from unicode codepoint to glyph. In general it can be a many-to-many mapping (ligatures, decomposition, contextual shaping).
These cases are pretty advanced and if you need those, you need likely need a proper text shaping library like HarfBuzz.
This also means that utxt is not sufficient for Arabic, Hebrew, Hindi, Thai, etc.
It should be good enough for Latin scripts, Cyrillic and CJK (Chinese, Japanese, Korean).


## To Do
* Allow packing multiple fonts into a single atlas and having multiple atlases for a single font (useful for CJK).
* Allow dynamic glyph caching (add utxt_get_atlas_dirty_rect) and pack glyphs into atlas dynamically.
* use utxt_alloc for stbtt (define STBTT_malloc/STBTT_free)
* add mode that retries packing, double atlas size if atlas too small
* BMFont support (see [fontbm](https://github.com/vladimirgamalyan/fontbm))
