/****************************************************************************
 *
 * main.c
 *
 *   Regression test for OT-SVG glyph document life cycle and for the
 *   error handling of the rendering hooks.
 *
 *   The test installs a set of mock OT-SVG hooks via the `svg-hooks'
 *   driver property, loads SVG glyphs from the synthetic font produced
 *   by `make-font.py', and checks that
 *
 *   - a copied `FT_Glyph' keeps its document after the originating glyph
 *     slot is reused or destroyed,
 *   - the metrics seen by the hooks match the current size,
 *   - a failing init or preset hook aborts rendering instead of leaving
 *     the slot in an inconsistent state, and
 *   - a missing hook set yields `FT_Err_Missing_SVG_Hooks' without
 *     calling into invalid function pointers.
 *
 *   Distributed under the FreeType project license, LICENSE.TXT.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_MODULE_H
#include FT_OTSVG_H


  static const char  doc_glyph1[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1000 1000\">"
    "<rect width=\"1000\" height=\"1000\" fill=\"#0f0\"/></svg>";

  static const char  doc_glyph2[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1000 1000\">"
    "<circle cx=\"500\" cy=\"500\" r=\"400\" fill=\"#00f\"/></svg>";


  static char            rendered_document[4096];
  static size_t          rendered_length;
  static FT_Size_Metrics rendered_metrics;
  static FT_Matrix       rendered_matrix;

  static int  render_calls;
  static int  init_fails;
  static int  preset_fails;


  static FT_Error
  test_init_svg( FT_Pointer*  data_pointer )
  {
    *data_pointer = NULL;

    return init_fails ? FT_Err_Invalid_Argument : FT_Err_Ok;
  }


  static void
  test_free_svg( FT_Pointer*  data_pointer )
  {
    *data_pointer = NULL;
  }


  static FT_Error
  test_preset_slot( FT_GlyphSlot  slot,
                    FT_Bool       cache,
                    FT_Pointer*   state )
  {
    FT_SVG_Document  document = (FT_SVG_Document)slot->other;

    FT_UNUSED( state );

    if ( preset_fails )
      return FT_Err_Invalid_Argument;

    if ( document )
    {
      rendered_metrics = document->metrics;
      rendered_matrix  = document->transform;
    }

    if ( cache )
      return FT_Err_Ok;

    slot->bitmap_left       = 0;
    slot->bitmap_top        = 100;
    slot->bitmap.width      = 32;
    slot->bitmap.rows       = 32;
    slot->bitmap.pitch      = 32;
    slot->bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
    slot->bitmap.num_grays  = 256;

    return FT_Err_Ok;
  }


  static FT_Error
  test_render_svg( FT_GlyphSlot  slot,
                   FT_Pointer*   state )
  {
    FT_SVG_Document  document = (FT_SVG_Document)slot->other;


    FT_UNUSED( state );

    render_calls++;

    if ( !document || !document->svg_document )
      return FT_Err_Invalid_SVG_Document;

    rendered_length = document->svg_document_length;
    if ( rendered_length > sizeof ( rendered_document ) )
      rendered_length = sizeof ( rendered_document );
    memcpy( rendered_document, document->svg_document, rendered_length );

    rendered_metrics = document->metrics;
    rendered_matrix  = document->transform;

    if ( slot->bitmap.buffer && slot->bitmap.pitch > 0 )
      memset( slot->bitmap.buffer, 0x80,
              (size_t)slot->bitmap.pitch * slot->bitmap.rows );

    slot->format = FT_GLYPH_FORMAT_BITMAP;

    return FT_Err_Ok;
  }


  static int
  document_matches( const char*  expected )
  {
    size_t  length = strlen( expected );


    return rendered_length == length &&
           memcmp( rendered_document, expected, length ) == 0;
  }


  static void
  reset_capture( void )
  {
    memset( rendered_document, 0, sizeof ( rendered_document ) );
    rendered_length = 0;
    render_calls    = 0;
  }


  static int
  test_copy_lifetime( FT_Library  library,
                      const char* filepath )
  {
    FT_Face   face = NULL;
    FT_Glyph  original = NULL;
    FT_Glyph  copy     = NULL;
    int       failed = 1;


    if ( FT_New_Face( library, filepath, 0, &face ) )
    {
      fprintf( stderr, "could not open file: %s\n", filepath );
      goto Exit;
    }

    FT_Set_Pixel_Sizes( face, 0, 32 );

    /* Load the first (uncompressed) document and extract a glyph. */
    if ( FT_Load_Glyph( face, 1, FT_LOAD_COLOR | FT_LOAD_DEFAULT ) )
      goto Exit;

    if ( FT_Get_Glyph( face->glyph, &original ) )
      goto Exit;

    if ( FT_Glyph_Copy( original, &copy ) )
      goto Exit;

    /* Reuse the slot with the second (compressed) document. */
    if ( FT_Load_Glyph( face, 2, FT_LOAD_COLOR | FT_LOAD_DEFAULT ) )
      goto Exit;

    reset_capture();
    if ( FT_Glyph_To_Bitmap( &original, FT_RENDER_MODE_NORMAL, NULL, 1 ) )
      goto Exit;

    if ( render_calls != 1 || !document_matches( doc_glyph1 ) )
    {
      fprintf( stderr, "copy does not survive slot reuse\n" );
      goto Exit;
    }

    reset_capture();
    if ( FT_Glyph_To_Bitmap( &copy, FT_RENDER_MODE_NORMAL, NULL, 1 ) )
      goto Exit;

    if ( render_calls != 1 || !document_matches( doc_glyph1 ) )
    {
      fprintf( stderr, "second copy does not survive slot reuse\n" );
      goto Exit;
    }

    failed = 0;

  Exit:
    if ( original )
      FT_Done_Glyph( original );
    if ( copy )
      FT_Done_Glyph( copy );
    if ( face )
      FT_Done_Face( face );

    return failed;
  }


  static int
  test_destroy_face_lifetime( FT_Library  library,
                              const char* filepath )
  {
    FT_Face   face = NULL;
    FT_Glyph  glyph = NULL;
    FT_Error  error;
    int       failed = 1;


    if ( FT_New_Face( library, filepath, 0, &face ) )
      goto Exit;

    FT_Set_Pixel_Sizes( face, 0, 32 );

    if ( FT_Load_Glyph( face, 2, FT_LOAD_COLOR | FT_LOAD_DEFAULT ) )
      goto Exit;

    /* The compressed document is only usable with zlib support; otherwise */
    /* the driver reports `Unimplemented_Feature' and this test is moot.   */
    if ( face->glyph->format != FT_GLYPH_FORMAT_SVG )
    {
      fprintf( stderr, "compressed SVG documents are not supported, "
                       "skipping\n" );
      failed = 0;
      goto Exit;
    }

    if ( FT_Get_Glyph( face->glyph, &glyph ) )
      goto Exit;

    /* The glyph copies the compressed document while loading it. */
    FT_Done_Face( face );
    face = NULL;

    reset_capture();
    error = FT_Glyph_To_Bitmap( &glyph, FT_RENDER_MODE_NORMAL, NULL, 1 );
    if ( error )
    {
      fprintf( stderr, "rendering after face destruction failed: 0x%x\n",
               error );
      goto Exit;
    }

    if ( render_calls != 1 || !document_matches( doc_glyph2 ) )
    {
      fprintf( stderr, "glyph does not survive face destruction\n" );
      goto Exit;
    }

    failed = 0;

  Exit:
    if ( glyph )
      FT_Done_Glyph( glyph );
    if ( face )
      FT_Done_Face( face );

    return failed;
  }


  static int
  test_metrics( FT_Library  library,
                const char* filepath )
  {
    FT_Face   face = NULL;
    FT_Glyph  glyph = NULL;
    int       failed = 1;


    if ( FT_New_Face( library, filepath, 0, &face ) )
      goto Exit;

    /* Load at a small size... */
    FT_Set_Pixel_Sizes( face, 0, 20 );
    if ( FT_Load_Glyph( face, 1, FT_LOAD_COLOR | FT_LOAD_DEFAULT ) )
      goto Exit;

    /* ... and render after the size has changed: the hooks must see the */
    /* current size, not the one that was active at load time.           */
    FT_Set_Pixel_Sizes( face, 0, 40 );

    reset_capture();
    if ( FT_Render_Glyph( face->glyph, FT_RENDER_MODE_NORMAL ) )
      goto Exit;

    if ( rendered_metrics.x_ppem != face->size->metrics.x_ppem ||
         rendered_metrics.y_ppem != face->size->metrics.y_ppem )
    {
      fprintf( stderr, "hooks saw a stale size: %u/%u, expected %u/%u\n",
               rendered_metrics.x_ppem, rendered_metrics.y_ppem,
               face->size->metrics.x_ppem, face->size->metrics.y_ppem );
      goto Exit;
    }

    /* A transform set on the face has to reach the hooks as well. */
    {
      FT_Matrix  matrix = { 0x10000L, 0x2000L, 0, 0x10000L };


      if ( FT_Load_Glyph( face, 1, FT_LOAD_COLOR | FT_LOAD_DEFAULT ) )
        goto Exit;

      FT_Set_Transform( face, &matrix, NULL );
      if ( FT_Load_Glyph( face, 1, FT_LOAD_COLOR | FT_LOAD_DEFAULT ) )
        goto Exit;

      reset_capture();
      if ( FT_Render_Glyph( face->glyph, FT_RENDER_MODE_NORMAL ) )
        goto Exit;

      if ( rendered_matrix.xy != 0x2000L )
      {
        fprintf( stderr, "hooks saw a wrong transform\n" );
        goto Exit;
      }

      FT_Set_Transform( face, NULL, NULL );
    }

    failed = 0;

  Exit:
    if ( glyph )
      FT_Done_Glyph( glyph );
    if ( face )
      FT_Done_Face( face );

    return failed;
  }


  static int
  test_hook_errors( FT_Library             library,
                    const char*            filepath,
                    const SVG_RendererHooks*  hooks,
                    int                    fail_init,
                    int                    fail_preset )
  {
    FT_Face   face = NULL;
    FT_Glyph  glyph = NULL;
    FT_Error  error;
    int       failed = 1;


    init_fails   = fail_init;
    preset_fails = fail_preset;

    /* Reinstalling the hooks also discards any state left over from an */
    /* earlier test, so that the init hook is called again.             */
    if ( FT_Property_Set( library, "ot-svg", "svg-hooks", hooks ) )
      goto Exit;

    if ( FT_New_Face( library, filepath, 0, &face ) )
      goto Exit;

    FT_Set_Pixel_Sizes( face, 0, 32 );

    if ( FT_Load_Glyph( face, 1, FT_LOAD_COLOR | FT_LOAD_DEFAULT ) )
      goto Exit;

    if ( FT_Get_Glyph( face->glyph, &glyph ) )
      goto Exit;

    reset_capture();
    error = FT_Glyph_To_Bitmap( &glyph, FT_RENDER_MODE_NORMAL, NULL, 1 );

    if ( !error )
    {
      fprintf( stderr, "failing hooks did not abort rendering\n" );
      goto Exit;
    }

    if ( render_calls != 0 )
    {
      fprintf( stderr,
               "render hook was called although a previous hook failed\n" );
      goto Exit;
    }

    failed = 0;

  Exit:
    init_fails   = 0;
    preset_fails = 0;

    if ( glyph )
      FT_Done_Glyph( glyph );
    if ( face )
      FT_Done_Face( face );

    return failed;
  }


  static int
  test_missing_hooks( FT_Library  library,
                      const char* filepath )
  {
    FT_Face   face = NULL;
    FT_Glyph  glyph = NULL;
    FT_Error  error;
    int       failed = 1;


    /* A fresh library without the `svg-hooks' property set. */
    if ( FT_New_Face( library, filepath, 0, &face ) )
      goto Exit;

    FT_Set_Pixel_Sizes( face, 0, 32 );

    if ( FT_Load_Glyph( face, 1, FT_LOAD_COLOR | FT_LOAD_DEFAULT ) )
      goto Exit;

    if ( FT_Get_Glyph( face->glyph, &glyph ) )
      goto Exit;

    reset_capture();
    error = FT_Glyph_To_Bitmap( &glyph, FT_RENDER_MODE_NORMAL, NULL, 1 );
    if ( error != FT_Err_Missing_SVG_Hooks )
    {
      fprintf( stderr, "expected FT_Err_Missing_SVG_Hooks, got 0x%x\n",
               error );
      goto Exit;
    }

    if ( render_calls != 0 )
    {
      fprintf( stderr, "render hook called without hooks being set\n" );
      goto Exit;
    }

    /* The same has to hold for the direct rendering path. */
    error = FT_Load_Glyph( face, 1, FT_LOAD_COLOR | FT_LOAD_DEFAULT );
    if ( !error )
      error = FT_Render_Glyph( face->glyph, FT_RENDER_MODE_NORMAL );

    if ( error != FT_Err_Missing_SVG_Hooks )
    {
      fprintf( stderr, "direct rendering without hooks gave 0x%x\n", error );
      goto Exit;
    }

    failed = 0;

  Exit:
    if ( glyph )
      FT_Done_Glyph( glyph );
    if ( face )
      FT_Done_Face( face );

    return failed;
  }


  int
  main( int     argc,
        char**  argv )
  {
    static const SVG_RendererHooks  hooks =
    {
      test_init_svg,
      test_free_svg,
      test_render_svg,
      test_preset_slot
    };

    const char*  testdata_dir = getenv( "FREETYPE_TESTS_DATA_DIR" );
    char         filepath[FILENAME_MAX];

    FT_Library  library   = NULL;
    FT_Library  bare_library = NULL;
    FT_Error    error;
    int         failures = 0;


    if ( argc > 1 )
      snprintf( filepath, sizeof ( filepath ), "%s", argv[1] );
    else
      snprintf( filepath, sizeof ( filepath ), "%s/%s",
                testdata_dir ? testdata_dir : "../tests/data",
                "svg-test.ttf" );

    if ( FT_Init_FreeType( &library ) )
      return 1;

    error = FT_Property_Set( library, "ot-svg", "svg-hooks", &hooks );
    if ( error )
    {
      fprintf( stderr, "could not set SVG hooks: 0x%x\n", error );
      FT_Done_FreeType( library );
      return 1;
    }

    failures += test_copy_lifetime( library, filepath );
    failures += test_destroy_face_lifetime( library, filepath );
    failures += test_metrics( library, filepath );
    failures += test_hook_errors( library, filepath, &hooks, 1, 0 );
    failures += test_hook_errors( library, filepath, &hooks, 0, 1 );

    /* Run the missing-hook test on a library without any hooks set. */
    if ( !FT_Init_FreeType( &bare_library ) )
    {
      failures += test_missing_hooks( bare_library, filepath );
    }
    else
      failures++;

    if ( bare_library )
      FT_Done_FreeType( bare_library );
    FT_Done_FreeType( library );

    if ( failures )
      fprintf( stderr, "%d OT-SVG test(s) failed\n", failures );

    return failures ? 1 : 0;
  }

/* END */
