# 'COLR' v1 paint parsing and traversal

The test covers the parsing and traversal contract of the public 'COLR' v1
API: a well-formed, deeply nested paint graph must be reported in a stable
order with the source and backdrop relationship of `PaintComposite`
preserved, while malformed offsets and cyclic references must produce a
recognizable failure instead of an out-of-bounds read.

`make-fonts.py` uses only Python's standard library and generates five
small TrueType fixtures at build time.  Every fixture contains a 'CPAL'
table and a 'COLR' version 1 table, and the 'COLR' table is always the last
table of the file, written without trailing padding.

`colr-v1-valid.ttf`
: One base glyph with four root layers:

  * a `PaintGlyph` with a `PaintSolid` fill,
  * a `PaintComposite` whose source is a transformed glyph and whose
    backdrop is a different glyph,
  * a `PaintLinearGradient` with two color stops,
  * a `PaintGlyph` that reuses the very paint table that the composite's
    source transform installs.

  The test compares a depth-first trace against an expected string, so the
  order of layers, composite source/backdrop subtrees, and color stops is
  pinned.  It also checks the root transform returned by
  `FT_Get_Color_Glyph_Paint`, the stability of repeated root lookups, and
  that the shared paint table is reported through one and the same opaque
  pointer on both paths.

`colr-v1-invalid.ttf`
: One base glyph per malformation.  Each is classified by which public
  call fails first: the root lookup, `FT_Get_Paint`, `FT_Get_Paint_Layers`,
  or `FT_Get_Colorline_Stops`.  The cases are a paint offset equal to the
  end of the table, an offset into the base glyph list, paint and color
  line offsets far outside the table, a layer offset outside the layer
  list, an out-of-range layer index, an overflowing color stop count, an
  unknown paint format, an invalid composite mode, an invalid extend mode,
  and a zero child offset.

`colr-v1-oob-layer.ttf`
: The layer list precedes the base glyph list and its only entry points
  below the paint region.  The layer lookup must fail and must leave the
  iterator untouched.

`colr-v1-cycle.ttf`
: Three cycles.  Glyph 1 refers to itself through `PaintColrGlyph`, glyphs
  2 and 3 refer to each other, and glyph 4's only layer points back at
  glyph 4's root `PaintColrLayers` table.  The library never recurses into
  referenced glyphs, so the cycles surface as a repeated opaque paint; the
  test checks that the repeat is stable and the layer iterator terminates.

`colr-v1-truncated.ttf`
: A `PaintSolid` truncated to four of its five bytes, ending exactly at the
  last byte of the 'COLR' table.

Because the 'COLR' table ends with the file, the test maps each fixture
directly before an inaccessible guard page (on platforms with `mmap` and
`mprotect`) and proves the page faults before relying on it.  An over-read
past the end of the table then raises `SIGSEGV` or `SIGBUS` instead of
silently succeeding.  On other platforms the fixtures are loaded from
ordinary memory, where a sanitizer build still catches an over-read.

From the source root:

```sh
meson setup out -Dtests=enabled
meson test -C out colr-v1 --print-errorlogs
```

The generator, driver, and generated data are provided under the FreeType
project license, `LICENSE.TXT`.
