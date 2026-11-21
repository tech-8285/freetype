/* Regression test for variable-font instance semantics.
 *
 * Switching between named instances, design coordinates, and blend
 * coordinates must always leave the axis values, the variation flags, and
 * the loaded glyph metrics describing the same instance.  In particular:
 *
 *   - resetting to the default instance must not retain the previous
 *     instance's coordinates;
 *   - out-of-range design coordinates are clamped to the axis range while
 *     the normalized-coordinate range checks stay untouched;
 *   - partial design-coordinate input keeps the unspecified axes at the
 *     named-instance or default values for both design and normalized
 *     coordinates, including the FT_IS_VARIATION flag.
 *
 * Distributed under the FreeType project license, LICENSE.TXT.
 */

#include <stdio.h>
#include <stdlib.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H


#define D( x )  ( (FT_Fixed)( (x) * 65536L ) )


  static int  failures = 0;


  static FT_Long
  advance_of( FT_Face  face )
  {
    FT_UInt  glyph_index = FT_Get_Char_Index( face, 0x41 );


    if ( FT_Load_Glyph( face, glyph_index,
                        FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING ) )
    {
      fprintf( stderr, "glyph load failed\n" );
      exit( 1 );
    }

    return face->glyph->metrics.horiAdvance;
  }


  static void
  check( FT_Face    face,
         const char*  tag,
         FT_Fixed   expected_design0,
         FT_Fixed   expected_design1,
         FT_Long    expected_advance )
  {
    FT_Fixed  design[2]     = { 0, 0 };
    FT_Fixed  normalized[2] = { 0, 0 };


    if ( FT_Get_Var_Design_Coordinates( face, 2, design )  ||
         FT_Get_Var_Blend_Coordinates( face, 2, normalized ) )
    {
      fprintf( stderr, "[%s] coordinate getters failed\n", tag );
      failures++;
      return;
    }

    FT_Long  advance = advance_of( face );

    if ( design[0] != expected_design0                    ||
         design[1] != expected_design1                    ||
         ( expected_advance >= 0 && advance != expected_advance ) )
    {
      fprintf( stderr,
               "FAIL [%s] design=(%.3f,%.3f) blend=(%.4f,%.4f) "
               "NI=%d V=%d advance=%ld, expected design=(%.3f,%.3f) "
               "advance=%ld\n",
               tag,
               (double)design[0] / 65536,
               (double)design[1] / 65536,
               (double)normalized[0] / 65536,
               (double)normalized[1] / 65536,
               FT_IS_NAMED_INSTANCE( face ),
               FT_IS_VARIATION( face ),
               advance,
               (double)expected_design0 / 65536,
               (double)expected_design1 / 65536,
               expected_advance );
      failures++;
    }
    else
      printf( "PASS [%s]\n", tag );
  }


  int
  main( int    argc,
        char** argv )
  {
    FT_Library  library;
    FT_Face     face;
    FT_Error    error;


    if ( argc != 2 )
    {
      fprintf( stderr, "usage: var-instance font.ttf\n" );
      return 2;
    }

    if ( FT_Init_FreeType( &library ) )
      return 2;

    error = FT_New_Face( library, argv[1], 0, &face );
    if ( error )
    {
      fprintf( stderr, "FT_New_Face failed: %d\n", error );
      return 1;
    }

    /* default instance: (400, 100), advance 600 */
    check( face, "initial default", D( 400 ), D( 100 ), 600 );

    /* named instance 1: Thin (100, 100), advance 500 */
    error = FT_Set_Named_Instance( face, 1 );
    if ( error )
    {
      fprintf( stderr, "FT_Set_Named_Instance(1) failed: %d\n", error );
      return 1;
    }
    check( face, "named instance 1", D( 100 ), D( 100 ), 500 );

    /* named instance 5: BlackCondensed (900, 75), advance 810 */
    FT_Set_Named_Instance( face, 5 );
    check( face, "named instance 5", D( 900 ), D( 75 ), 810 );

    /* reset to the default instance: no residue of (900, 75) */
    FT_Set_Named_Instance( face, 0 );
    check( face, "reset to default", D( 400 ), D( 100 ), 600 );
    if ( FT_IS_NAMED_INSTANCE( face ) || FT_IS_VARIATION( face ) )
    {
      fprintf( stderr, "FAIL [reset flags]: NI=%d V=%d, expected 0/0\n",
               FT_IS_NAMED_INSTANCE( face ), FT_IS_VARIATION( face ) );
      failures++;
    }
    else
      printf( "PASS [reset flags]\n" );

    /* out-of-range design coordinates must be clamped; the normalized
     * coordinate error rule ([-1, 1]) is exercised separately below.
     */
    FT_Fixed  above[2] = { D( 1500 ), D( 200 ) };

    FT_Set_Var_Design_Coordinates( face, 2, above );
    check( face, "design above maximum", D( 900 ), D( 125 ), 900 );

    FT_Fixed  below[2] = { D( 50 ), D( 50 ) };

    FT_Set_Var_Design_Coordinates( face, 2, below );
    check( face, "design below minimum", D( 100 ), D( 75 ), 450 );

    /* normalized coordinates outside [-1, 1] are rejected and must not
     * alter the current instance state.
     */
    FT_Fixed  bad_blend[2] = { D( 1.5 ), 0 };

    error = FT_Set_Var_Blend_Coordinates( face, 2, bad_blend );
    if ( !error )
    {
      fprintf( stderr, "FAIL [blend range]: expected an error\n" );
      failures++;
    }
    else
      printf( "PASS [blend range]\n" );
    check( face, "state after rejected blend", D( 100 ), D( 75 ), 450 );

    /* valid normalized extremes */
    FT_Fixed  min_blend[2] = { D( -1.0 ), D( -1.0 ) };

    FT_Set_Var_Blend_Coordinates( face, 2, min_blend );
    check( face, "blend minimum", D( 100 ), D( 75 ), 450 );

    FT_Fixed  zero_blend[2] = { 0, 0 };

    FT_Set_Var_Blend_Coordinates( face, 2, zero_blend );
    check( face, "blend default", D( 400 ), D( 100 ), 600 );

    /* partial design input under a named instance: the unspecified axis
     * keeps the named-instance value, and the face is reported as a
     * variation because the remaining axis is non-default.
     */
    FT_Set_Named_Instance( face, 5 );                 /* (900, 75) */
    FT_Fixed  one_axis[1] = { D( 100 ) };

    FT_Set_Var_Design_Coordinates( face, 1, one_axis );
    check( face, "partial design under instance", D( 100 ), D( 75 ), 450 );
    if ( !FT_IS_VARIATION( face ) )
    {
      fprintf( stderr,
               "FAIL [partial design flag]: expected FT_IS_VARIATION\n" );
      failures++;
    }
    else
      printf( "PASS [partial design flag]\n" );

    /* the same partial input without a named instance fills the
     * unspecified axis from the axis default.
     */
    FT_Set_Named_Instance( face, 0 );
    FT_Set_Var_Design_Coordinates( face, 1, one_axis );
    check( face, "partial design without instance",
           D( 100 ), D( 100 ), 500 );

    /* repeated switching must converge on the default instance */
    for ( int i = 0; i < 3; i++ )
    {
      FT_Set_Named_Instance( face, 5 );
      FT_Set_Named_Instance( face, 0 );
    }
    check( face, "repeated switching stable", D( 400 ), D( 100 ), 600 );

    FT_Done_Face( face );
    FT_Done_FreeType( library );

    if ( failures )
    {
      printf( "FAILED: %d check(s)\n", failures );
      return 1;
    }

    printf( "ALL OK\n" );
    return 0;
  }
