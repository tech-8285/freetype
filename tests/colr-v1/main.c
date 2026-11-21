/* Tests for 'COLR' v1 paint parsing and traversal.
 *
 * The tests use only public FreeType interfaces: the 'COLR' v1 paint API
 * (`FT_Get_Color_Glyph_Paint', `FT_Get_Paint', `FT_Get_Paint_Layers',
 * `FT_Get_Colorline_Stops', `FT_Get_Color_Glyph_ClipBox') and the 'COLR' v0
 * layer API (`FT_Get_Color_Glyph_Layer').  The fixtures are generated at
 * build time by `make-fonts.py'.
 *
 * Distributed under the FreeType project license, LICENSE.TXT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_COLOR_H


#if defined( __unix__ ) || defined( __APPLE__ )
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#define HAVE_GUARD_PAGE  1
#endif


#ifdef HAVE_GUARD_PAGE

static sigjmp_buf  guard_jmp;


static void
guard_handler( int  signal_number )
{
  FT_UNUSED( signal_number );

  siglongjmp( guard_jmp, 1 );
}


/* Read one byte at `address'; return 0 if it is accessible, 1 otherwise.
 * Some systems raise SIGBUS instead of SIGSEGV for an inaccessible page,
 * so both are trapped. */
static int
probe_guard_page( const FT_Byte*  address )
{
  struct sigaction  action;
  struct sigaction  previous_segv;
  struct sigaction  previous_bus;
  volatile FT_Byte  value;
  int               faulted;
  int               have_bus = 0;


  memset( &action, 0, sizeof ( action ) );
  action.sa_handler = guard_handler;
  sigemptyset( &action.sa_mask );
  action.sa_flags = SA_NODEFER;

  if ( sigaction( SIGSEGV, &action, &previous_segv ) != 0 )
    return 1;

#ifdef SIGBUS
  if ( sigaction( SIGBUS, &action, &previous_bus ) != 0 )
  {
    sigaction( SIGSEGV, &previous_segv, NULL );
    return 1;
  }
  have_bus = 1;
#endif

  if ( sigsetjmp( guard_jmp, 1 ) == 0 )
  {
    value   = *address;
    faulted = 0;

    /* Keep the read from being optimized away. */
    (void)value;
  }
  else
    faulted = 1;

  sigaction( SIGSEGV, &previous_segv, NULL );
  if ( have_bus )
    sigaction( SIGBUS, &previous_bus, NULL );

  return faulted;
}

#endif /* HAVE_GUARD_PAGE */


/*
 * The 'COLR' table is the last table of every fixture and is written
 * without trailing padding.  When a guard page is available the font is
 * placed directly before an inaccessible page, so that any read past the
 * end of the 'COLR' table faults instead of silently succeeding.
 */
typedef struct  FontBuffer_
{
  const FT_Byte*  data;
  FT_Long         size;
  void*           mapping;   /* non-NULL if mmap() was used */
  size_t          map_size;

} FontBuffer;


static int
font_buffer_load( FontBuffer*  buffer,
                  const char*  path )
{
  FILE*    file = NULL;
  FT_Byte*  data = NULL;
  long      size;
  int       ret = 1;


  memset( buffer, 0, sizeof ( *buffer ) );

  file = fopen( path, "rb" );
  if ( !file )
  {
    fprintf( stderr, "could not open %s\n", path );
    return 1;
  }

  if ( fseek( file, 0, SEEK_END ) != 0 ||
       ( size = ftell( file ) ) < 0  ||
       fseek( file, 0, SEEK_SET ) != 0 )
    goto Exit;

  data = (FT_Byte*)malloc( (size_t)size );
  if ( !data || fread( data, 1, (size_t)size, file ) != (size_t)size )
    goto Exit;

#ifdef HAVE_GUARD_PAGE
  {
    long    page  = sysconf( _SC_PAGESIZE );
    size_t  pages = ( (size_t)size + (size_t)page - 1 ) / (size_t)page + 1;
    size_t  total = pages * (size_t)page;
    FT_Byte*  region;


    region = (FT_Byte*)mmap( NULL, total, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    if ( region == MAP_FAILED )
      goto Exit;

    if ( mprotect( region + total - (size_t)page, (size_t)page,
                   PROT_NONE ) != 0 )
    {
      munmap( region, total );
      goto Exit;
    }

    /* The font ends exactly where the inaccessible page begins. */
    memcpy( region + total - (size_t)page - (size_t)size,
            data, (size_t)size );

    /* Make sure the guard page really faults before relying on it. */
    if ( !probe_guard_page( region + total - (size_t)page ) )
    {
      munmap( region, total );
      goto Exit;
    }

    buffer->mapping  = region;
    buffer->map_size = total;
    buffer->data     = region + total - (size_t)page - (size_t)size;

    /* The mapping owns the copy from here on. */
    free( data );
    data = NULL;
  }
#else
  buffer->data = data;
  data         = NULL;  /* ownership moved to the buffer */
#endif

  buffer->size = size;
  ret          = 0;

Exit:
  free( data );
  fclose( file );
  return ret;
}


static void
font_buffer_done( FontBuffer*  buffer )
{
#ifdef HAVE_GUARD_PAGE
  if ( buffer->mapping )
    munmap( buffer->mapping, buffer->map_size );
#else
  free( (void*)buffer->data );
#endif
  memset( buffer, 0, sizeof ( *buffer ) );
}


/* Verify that the fixture ends with its 'COLR' table and that the table
 * covers the last `size' bytes of the file.  This is what makes the guard
 * page meaningful: an over-read at the end of the table is an over-read at
 * the end of the buffer. */
static int
check_colr_is_last( const FT_Byte*  data,
                    FT_Long         size,
                    const char*     filename )
{
  FT_ULong  num_tables;
  FT_ULong  colr_offset = 0;
  FT_ULong  colr_length = 0;
  FT_ULong  i;


  if ( size < 12 )
    goto Invalid;

  num_tables = ( (FT_ULong)data[4] << 8 ) | data[5];
  if ( 12 + 16 * num_tables > (FT_ULong)size )
    goto Invalid;

  for ( i = 0; i < num_tables; i++ )
  {
    const FT_Byte*  record = data + 12 + 16 * i;


    if ( record[0] == 'C' && record[1] == 'O' &&
         record[2] == 'L' && record[3] == 'R' )
    {
      colr_offset = ( (FT_ULong)record[8]  << 24 ) |
                    ( (FT_ULong)record[9]  << 16 ) |
                    ( (FT_ULong)record[10] <<  8 ) |
                      (FT_ULong)record[11];
      colr_length = ( (FT_ULong)record[12] << 24 ) |
                    ( (FT_ULong)record[13] << 16 ) |
                    ( (FT_ULong)record[14] <<  8 ) |
                      (FT_ULong)record[15];
      break;
    }
  }

  if ( !colr_offset || colr_offset + colr_length != (FT_ULong)size )
    goto Invalid;

  return 0;

Invalid:
  fprintf( stderr, "%s: 'COLR' is not the last table\n", filename );
  return 1;
}


static FT_Face
open_buffer( FT_Library   library,
             FontBuffer*  buffer,
             const char*  filename )
{
  FT_Face  face = NULL;


  if ( font_buffer_load( buffer, filename ) )
    return NULL;

  if ( check_colr_is_last( buffer->data, buffer->size, filename ) )
  {
    font_buffer_done( buffer );
    return NULL;
  }

  if ( FT_New_Memory_Face( library, buffer->data, buffer->size, 0, &face ) )
  {
    fprintf( stderr, "could not open face from %s\n", filename );
    font_buffer_done( buffer );
    return NULL;
  }

  return face;
}


/**************************************************************************
 *
 * Valid paint graph.
 *
 */

/* Textual trace of a depth-first traversal; the expected result pins the
 * order in which paints are reported. */
typedef struct  Trace_
{
  char    buffer[2048];
  size_t  length;

} Trace;


static void
trace_add( Trace*       trace,
           const char*  format,
           ... )
{
  va_list  args;
  int      written;


  va_start( args, format );
  written = vsnprintf( trace->buffer + trace->length,
                       sizeof ( trace->buffer ) - trace->length,
                       format, args );
  va_end( args );

  if ( written < 0                                      ||
       (size_t)written >= sizeof ( trace->buffer ) - trace->length )
    trace->length = sizeof ( trace->buffer ) - 1;
  else
    trace->length += (size_t)written;
}


static int
trace_paint( FT_Face        face,
             FT_OpaquePaint  opaque_paint,
             int             depth,
             Trace*          trace )
{
  FT_COLR_Paint  paint;


  if ( depth > 12 )
  {
    trace_add( trace, "depth-limit\n" );
    return 1;
  }

  if ( !FT_Get_Paint( face, opaque_paint, &paint ) )
  {
    trace_add( trace, "paint-failed\n" );
    return 1;
  }

  switch ( paint.format )
  {
  case FT_COLR_PAINTFORMAT_COLR_LAYERS:
    {
      FT_LayerIterator  iterator = paint.u.colr_layers.layer_iterator;
      FT_OpaquePaint    layer;
      FT_UInt           count = 0;


      trace_add( trace, "LAYERS\n" );

      while ( FT_Get_Paint_Layers( face, &iterator, &layer ) )
      {
        count++;
        if ( trace_paint( face, layer, depth + 1, trace ) )
          return 1;
      }

      /* Re-querying an exhausted iterator must keep failing. */
      if ( FT_Get_Paint_Layers( face, &iterator, &layer ) )
      {
        trace_add( trace, "layers-revived\n" );
        return 1;
      }

      trace_add( trace, "LAYERS-END %u\n", count );
    }
    break;

  case FT_COLR_PAINTFORMAT_SOLID:
    trace_add( trace, "SOLID %u %ld\n",
               paint.u.solid.color.palette_index,
               (long)paint.u.solid.color.alpha );
    break;

  case FT_COLR_PAINTFORMAT_GLYPH:
    trace_add( trace, "GLYPH %u\n", paint.u.glyph.glyphID );
    if ( trace_paint( face, paint.u.glyph.paint, depth + 1, trace ) )
      return 1;
    break;

  case FT_COLR_PAINTFORMAT_COLR_GLYPH:
    trace_add( trace, "COLR-GLYPH %u\n", paint.u.colr_glyph.glyphID );
    break;

  case FT_COLR_PAINTFORMAT_TRANSFORM:
    trace_add( trace, "TRANSFORM %ld %ld %ld %ld %ld %ld\n",
               (long)paint.u.transform.affine.xx,
               (long)paint.u.transform.affine.yx,
               (long)paint.u.transform.affine.xy,
               (long)paint.u.transform.affine.yy,
               (long)paint.u.transform.affine.dx,
               (long)paint.u.transform.affine.dy );
    if ( trace_paint( face, paint.u.transform.paint, depth + 1, trace ) )
      return 1;
    break;

  case FT_COLR_PAINTFORMAT_COMPOSITE:
    {
      FT_OpaquePaint  source   = paint.u.composite.source_paint;
      FT_OpaquePaint  backdrop = paint.u.composite.backdrop_paint;


      trace_add( trace, "COMPOSITE %d source=%d backdrop=%d\n",
                 (int)paint.u.composite.composite_mode,
                 source.p != NULL, backdrop.p != NULL );

      /* Source and backdrop must be distinct subtrees.  Reporting them
       * separately keeps the relationship from being lost. */
      if ( !source.p || !backdrop.p || source.p == backdrop.p )
      {
        trace_add( trace, "composite-paints-invalid\n" );
        return 1;
      }

      trace_add( trace, "SOURCE\n" );
      if ( trace_paint( face, source, depth + 1, trace ) )
        return 1;

      trace_add( trace, "BACKDROP\n" );
      if ( trace_paint( face, backdrop, depth + 1, trace ) )
        return 1;
    }
    break;

  case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
    {
      FT_ColorLine        colorline = paint.u.linear_gradient.colorline;
      FT_ColorStopIterator  iterator = colorline.color_stop_iterator;
      FT_ColorStop        stop;
      FT_UInt             count = 0;


      trace_add( trace, "LINEAR %ld %ld %ld %ld %ld %ld extend=%d\n",
                 (long)paint.u.linear_gradient.p0.x,
                 (long)paint.u.linear_gradient.p0.y,
                 (long)paint.u.linear_gradient.p1.x,
                 (long)paint.u.linear_gradient.p1.y,
                 (long)paint.u.linear_gradient.p2.x,
                 (long)paint.u.linear_gradient.p2.y,
                 (int)colorline.extend );

      while ( FT_Get_Colorline_Stops( face, &stop, &iterator ) )
      {
        count++;
        trace_add( trace, "STOP %ld %u %ld\n",
                   (long)stop.stop_offset,
                   stop.color.palette_index,
                   (long)stop.color.alpha );
      }

      if ( FT_Get_Colorline_Stops( face, &stop, &iterator ) )
      {
        trace_add( trace, "stops-revived\n" );
        return 1;
      }

      trace_add( trace, "STOPS-END %u\n", count );
    }
    break;

  default:
    trace_add( trace, "UNEXPECTED-FORMAT %d\n", (int)paint.format );
    return 1;
  }

  return 0;
}


static int
test_valid( FT_Library   library,
            const char*  filename )
{
  static const char  expected[] =
    "LAYERS\n"
    "GLYPH 5\n"
    "SOLID 3 16384\n"
    "COMPOSITE 3 source=1 backdrop=1\n"
    "SOURCE\n"
    "TRANSFORM 65536 2048 -1024 65536 32768 -16384\n"
    "GLYPH 6\n"
    "SOLID 4 8192\n"
    "BACKDROP\n"
    "GLYPH 7\n"
    "SOLID 5 4096\n"
    "LINEAR 655360 1310720 1966080 2621440 3276800 3932160 extend=0\n"
    "STOP 0 1 16384\n"
    "STOP 65536 2 8192\n"
    "STOPS-END 2\n"
    "GLYPH 6\n"
    "SOLID 4 8192\n"
    "LAYERS-END 4\n";

  FontBuffer     buffer;
  FT_Face        face;
  FT_OpaquePaint  root;
  FT_OpaquePaint  again;
  FT_COLR_Paint  paint;
  FT_ClipBox     clip_box;
  FT_LayerIterator  layer_iterator;
  FT_OpaquePaint  layer[8];
  FT_COLR_Paint  composite;
  FT_UInt        num_layers = 0;
  FT_UInt        glyph_index;
  FT_UInt        color_index;
  Trace          trace;
  int            ret = 1;


  face = open_buffer( library, &buffer, filename );
  if ( !face )
    return 1;

  if ( !FT_HAS_COLOR( face ) || face->num_glyphs != 16 )
  {
    fprintf( stderr, "%s: unexpected face properties\n", filename );
    goto Exit;
  }

  /* The fixture has no 'COLR' v0 layer records and no clip list. */
  layer_iterator.p = NULL;
  if ( FT_Get_Color_Glyph_Layer( face, 1, &glyph_index, &color_index,
                                 &layer_iterator )                 )
  {
    fprintf( stderr, "%s: unexpected 'COLR' v0 layer\n", filename );
    goto Exit;
  }
  if ( FT_Get_Color_Glyph_ClipBox( face, 1, &clip_box ) )
  {
    fprintf( stderr, "%s: unexpected clip box\n", filename );
    goto Exit;
  }

  root.p                     = NULL;
  root.insert_root_transform = 0;
  if ( !FT_Get_Color_Glyph_Paint( face, 1, FT_COLOR_NO_ROOT_TRANSFORM,
                                  &root ) )
  {
    fprintf( stderr, "%s: no root paint\n", filename );
    goto Exit;
  }

  /* The root paint must be stable across repeated lookups. */
  again.p                     = NULL;
  again.insert_root_transform = 0;
  if ( !FT_Get_Color_Glyph_Paint( face, 1, FT_COLOR_NO_ROOT_TRANSFORM,
                                  &again ) ||
       again.p != root.p                                )
  {
    fprintf( stderr, "%s: root paint is not stable\n", filename );
    goto Exit;
  }

  /* A pre-set opaque paint is rejected instead of being overwritten. */
  again.p                     = (FT_Byte*)&expected;
  again.insert_root_transform = 0;
  if ( FT_Get_Color_Glyph_Paint( face, 1, FT_COLOR_NO_ROOT_TRANSFORM,
                                 &again ) )
  {
    fprintf( stderr, "%s: accepted a pre-set opaque paint\n", filename );
    goto Exit;
  }

  /* Glyph indices that are not in the base glyph list have no root. */
  if ( FT_Get_Color_Glyph_Paint( face, 16, FT_COLOR_NO_ROOT_TRANSFORM,
                                 &again ) )
  {
    fprintf( stderr, "%s: found a root paint for an unrelated glyph\n",
             filename );
    goto Exit;
  }

  trace.buffer[0] = '\0';
  trace.length    = 0;
  if ( trace_paint( face, root, 0, &trace ) ||
       strcmp( trace.buffer, expected ) != 0 )
  {
    fprintf( stderr, "%s: unexpected traversal\n--- got ---\n%s"
                     "--- expected ---\n%s", filename,
             trace.buffer, expected );
    goto Exit;
  }

  /* Layer 3 must reference the very table used below the transform that
   * the composite source installs in layer 1, i.e. the DAG is shared, not
   * duplicated.  Verify the identity through the public pointers. */
  if ( !FT_Get_Paint( face, root, &paint ) ||
       paint.format != FT_COLR_PAINTFORMAT_COLR_LAYERS )
  {
    fprintf( stderr, "%s: root is not a layer paint\n", filename );
    goto Exit;
  }

  layer_iterator = paint.u.colr_layers.layer_iterator;
  while ( num_layers < 8 &&
          FT_Get_Paint_Layers( face, &layer_iterator,
                               &layer[num_layers] ) )
    num_layers++;

  if ( num_layers != 4 )
  {
    fprintf( stderr, "%s: expected 4 root layers, got %u\n",
             filename, num_layers );
    goto Exit;
  }

  if ( !FT_Get_Paint( face, layer[1], &composite ) ||
       composite.format != FT_COLR_PAINTFORMAT_COMPOSITE )
  {
    fprintf( stderr, "%s: layer 1 is not a composite paint\n", filename );
    goto Exit;
  }

  {
    FT_COLR_Paint  transform;


    if ( !FT_Get_Paint( face, composite.u.composite.source_paint,
                        &transform )                    ||
         transform.format != FT_COLR_PAINTFORMAT_TRANSFORM )
    {
      fprintf( stderr, "%s: composite source is not a transform\n",
               filename );
      goto Exit;
    }

    /* The shared table is the transform's child and layer 3. */
    if ( transform.u.transform.paint.p != layer[3].p )
    {
      fprintf( stderr, "%s: shared paint table was not reused\n", filename );
      goto Exit;
    }
  }

  /* The root transform is reported before the actual root paint and its
   * child is exactly the paint returned without a root transform. */
  again.p                     = NULL;
  again.insert_root_transform = 0;
  if ( !FT_Get_Color_Glyph_Paint( face, 1, FT_COLOR_INCLUDE_ROOT_TRANSFORM,
                                  &again )                            ||
       !FT_Get_Paint( face, again, &paint )                           ||
       paint.format != FT_COLR_PAINTFORMAT_TRANSFORM                  ||
       paint.u.transform.paint.p != root.p                            ||
       paint.u.transform.paint.insert_root_transform                 )
  {
    fprintf( stderr, "%s: root transform is not the root paint's parent\n",
             filename );
    goto Exit;
  }

  if ( paint.u.transform.affine.dx != 0 ||
       paint.u.transform.affine.dy != 0 )
  {
    fprintf( stderr, "%s: root transform has a translation\n", filename );
    goto Exit;
  }

  ret = 0;

Exit:
  FT_Done_Face( face );
  font_buffer_done( &buffer );
  return ret;
}


/**************************************************************************
 *
 * Out-of-bounds offsets.
 *
 */

typedef enum  Malformed_
{
  MALFORMED_WELL_FORMED = 0,  /* unexpectedly parsed without error       */
  MALFORMED_NO_ROOT,          /* FT_Get_Color_Glyph_Paint fails          */
  MALFORMED_PAINT,            /* root resolves, FT_Get_Paint fails       */
  MALFORMED_LAYERS,           /* FT_Get_Paint_Layers fails               */
  MALFORMED_STOPS             /* FT_Get_Colorline_Stops fails            */

} Malformed;


static Malformed
classify_glyph( FT_Face  face,
                FT_UInt  glyph )
{
  FT_OpaquePaint  paint;
  FT_COLR_Paint   details;


  paint.p                     = NULL;
  paint.insert_root_transform = 0;

  if ( !FT_Get_Color_Glyph_Paint( face, glyph, FT_COLOR_NO_ROOT_TRANSFORM,
                                  &paint ) )
    return MALFORMED_NO_ROOT;

  if ( !FT_Get_Paint( face, paint, &details ) )
    return MALFORMED_PAINT;

  if ( details.format == FT_COLR_PAINTFORMAT_COLR_LAYERS )
  {
    FT_LayerIterator  iterator = details.u.colr_layers.layer_iterator;
    FT_OpaquePaint    layer;


    if ( !FT_Get_Paint_Layers( face, &iterator, &layer ) )
      return MALFORMED_LAYERS;

    if ( FT_Get_Paint( face, layer, &details ) )
      return MALFORMED_WELL_FORMED;

    return MALFORMED_PAINT;
  }

  if ( details.format == FT_COLR_PAINTFORMAT_LINEAR_GRADIENT )
  {
    FT_ColorStopIterator  iterator =
      details.u.linear_gradient.colorline.color_stop_iterator;
    FT_ColorStop  stop;


    if ( !FT_Get_Colorline_Stops( face, &stop, &iterator ) )
      return MALFORMED_STOPS;
  }

  return MALFORMED_WELL_FORMED;
}


static int
test_invalid( FT_Library   library,
              const char*  filename )
{
  static const Malformed  expected[] =
  {
    MALFORMED_NO_ROOT,  /* 1: record offset == end of table        */
    MALFORMED_PAINT,    /* 2: record offset inside base glyph list */
    MALFORMED_LAYERS,   /* 3: layer offset past the table          */
    MALFORMED_PAINT,    /* 4: child paint offset past the table    */
    MALFORMED_PAINT,    /* 5: color line offset past the table     */
    MALFORMED_PAINT,    /* 6: backdrop offset past the table       */
    MALFORMED_PAINT,    /* 7: layer index range exceeds the list   */
    MALFORMED_STOPS,    /* 8: color stop count overflows           */
    MALFORMED_PAINT,    /* 9: unknown paint format                 */
    MALFORMED_PAINT,    /* 10: composite mode out of range         */
    MALFORMED_PAINT,    /* 11: extend mode out of range            */
    MALFORMED_PAINT     /* 12: zero child offset                   */
  };

  FontBuffer  buffer;
  FT_Face     face;
  FT_UInt     i;
  int         ret = 1;


  face = open_buffer( library, &buffer, filename );
  if ( !face )
    return 1;

  if ( !FT_HAS_COLOR( face ) )
  {
    fprintf( stderr, "%s: face is not marked as colored\n", filename );
    goto Exit;
  }

  for ( i = 0; i < sizeof ( expected ) / sizeof ( expected[0] ); i++ )
  {
    Malformed  actual = classify_glyph( face, i + 1 );


    if ( actual != expected[i] )
    {
      fprintf( stderr, "%s: glyph %u classified as %d, expected %d\n",
               filename, i + 1, (int)actual, (int)expected[i] );
      goto Exit;
    }
  }

  /* No glyph outside the malformed set must expose a root paint. */
  for ( i = sizeof ( expected ) / sizeof ( expected[0] ) + 1; i < 16; i++ )
  {
    FT_OpaquePaint  paint;


    paint.p                     = NULL;
    paint.insert_root_transform = 0;

    if ( FT_Get_Color_Glyph_Paint( face, i, FT_COLOR_NO_ROOT_TRANSFORM,
                                   &paint ) )
    {
      fprintf( stderr, "%s: glyph %u unexpectedly has a root paint\n",
               filename, i );
      goto Exit;
    }
  }

  ret = 0;

Exit:
  FT_Done_Face( face );
  font_buffer_done( &buffer );
  return ret;
}


static int
test_oob_layer( FT_Library   library,
                const char*  filename )
{
  FontBuffer      buffer;
  FT_Face         face;
  FT_OpaquePaint  root;
  FT_COLR_Paint   paint;
  FT_LayerIterator  iterator;
  FT_Byte*        layers_start;
  FT_OpaquePaint  layer;
  int             ret = 1;


  face = open_buffer( library, &buffer, filename );
  if ( !face )
    return 1;

  root.p                     = NULL;
  root.insert_root_transform = 0;

  if ( !FT_Get_Color_Glyph_Paint( face, 1, FT_COLOR_NO_ROOT_TRANSFORM,
                                  &root )                           ||
       !FT_Get_Paint( face, root, &paint )                          ||
       paint.format != FT_COLR_PAINTFORMAT_COLR_LAYERS )
  {
    fprintf( stderr, "%s: expected a layer root paint\n", filename );
    goto Exit;
  }

  iterator = paint.u.colr_layers.layer_iterator;
  layers_start = iterator.p;

  /* The single layer offset points below the paint region, into the layer
   * list itself.  It must be rejected, not followed as a cycle. */
  if ( FT_Get_Paint_Layers( face, &iterator, &layer ) )
  {
    fprintf( stderr, "%s: followed a layer offset below the paint region\n",
             filename );
    goto Exit;
  }

  /* A rejected call must leave the iterator untouched. */
  if ( iterator.layer != 0 || iterator.p != layers_start )
  {
    fprintf( stderr, "%s: a failed layer lookup advanced the iterator\n",
             filename );
    goto Exit;
  }

  ret = 0;

Exit:
  FT_Done_Face( face );
  font_buffer_done( &buffer );
  return ret;
}


/**************************************************************************
 *
 * Cycles.
 *
 */

/* Follow a 'PaintColrGlyph' chain by looking up the referenced glyph
 * again.  Returns 1 when the pointer sequence revisits an earlier paint. */
static int
colr_glyph_cycle_revisits( FT_Face  face,
                           FT_UInt  glyph,
                           FT_UInt  steps )
{
  FT_OpaquePaint  start;
  FT_OpaquePaint  current;
  FT_COLR_Paint   paint;
  FT_UInt         i;
  int             revisited = 0;


  start.p                     = NULL;
  start.insert_root_transform = 0;
  if ( !FT_Get_Color_Glyph_Paint( face, glyph, FT_COLOR_NO_ROOT_TRANSFORM,
                                  &start ) )
    return 0;

  current = start;

  for ( i = 0; i < steps; i++ )
  {
    if ( !FT_Get_Paint( face, current, &paint ) ||
         paint.format != FT_COLR_PAINTFORMAT_COLR_GLYPH )
      return 0;

    current.p                     = NULL;
    current.insert_root_transform = 0;

    if ( !FT_Get_Color_Glyph_Paint( face, paint.u.colr_glyph.glyphID,
                                    FT_COLOR_NO_ROOT_TRANSFORM,
                                    &current ) )
      return 0;

    if ( current.p == start.p )
    {
      revisited = 1;
      break;
    }
  }

  return revisited;
}


static int
test_cycle( FT_Library   library,
            const char*  filename )
{
  FontBuffer      buffer;
  FT_Face         face;
  FT_OpaquePaint  root;
  FT_OpaquePaint  layer;
  FT_OpaquePaint  repeated;
  FT_COLR_Paint   paint;
  FT_LayerIterator  iterator;
  int             ret = 1;


  face = open_buffer( library, &buffer, filename );
  if ( !face )
    return 1;

  /* Glyph 1 refers to itself, glyphs 2 and 3 to each other.  The library
   * does not recurse into referenced glyphs and therefore never loops;
   * the cycle is visible to the client as a repeated opaque paint. */
  if ( !colr_glyph_cycle_revisits( face, 1, 8 ) ||
       !colr_glyph_cycle_revisits( face, 2, 8 ) )
  {
    fprintf( stderr, "%s: 'PaintColrGlyph' cycle was not observable\n",
             filename );
    goto Exit;
  }

  /* Glyph 4's only layer points back at glyph 4's root paint.  The layer
   * iterator is finite, so retrieving layers terminates; following the
   * returned layer reaches the same 'PaintColrLayers' table, which is how
   * a client detects the cycle. */
  root.p                     = NULL;
  root.insert_root_transform = 0;

  if ( !FT_Get_Color_Glyph_Paint( face, 4, FT_COLOR_NO_ROOT_TRANSFORM,
                                  &root )                           ||
       !FT_Get_Paint( face, root, &paint )                          ||
       paint.format != FT_COLR_PAINTFORMAT_COLR_LAYERS              ||
       paint.u.colr_layers.layer_iterator.num_layers != 1 )
  {
    fprintf( stderr, "%s: glyph 4 is not a single-layer paint\n", filename );
    goto Exit;
  }

  iterator = paint.u.colr_layers.layer_iterator;

  if ( !FT_Get_Paint_Layers( face, &iterator, &layer ) )
  {
    fprintf( stderr, "%s: could not retrieve the cyclic layer\n", filename );
    goto Exit;
  }

  /* The layer's offset deliberately points at the root paint table, so the
   * returned opaque paint is the very one we started from.  Clients use
   * this identity to detect the cycle; the library itself never follows
   * it, so no traversal loops. */
  if ( layer.p != root.p )
  {
    fprintf( stderr, "%s: cyclic layer did not return the root paint\n",
             filename );
    goto Exit;
  }

  if ( !FT_Get_Paint( face, layer, &paint )                ||
       paint.format != FT_COLR_PAINTFORMAT_COLR_LAYERS     ||
       paint.u.colr_layers.layer_iterator.num_layers != 1   )
  {
    fprintf( stderr, "%s: cyclic layer did not resolve again\n", filename );
    goto Exit;
  }

  /* Re-deriving the layer from the freshly read paint yields the same
   * paint once more -- a stable, bounded repeat, never an out-of-table
   * read. */
  repeated = layer;
  iterator = paint.u.colr_layers.layer_iterator;

  if ( !FT_Get_Paint_Layers( face, &iterator, &layer ) ||
       layer.p != repeated.p )
  {
    fprintf( stderr, "%s: cycle is not stable\n", filename );
    goto Exit;
  }

  /* The finite iterator must terminate after its one layer. */
  if ( FT_Get_Paint_Layers( face, &iterator, &layer ) )
  {
    fprintf( stderr, "%s: cyclic layer iterator did not terminate\n",
             filename );
    goto Exit;
  }

  ret = 0;

Exit:
  FT_Done_Face( face );
  font_buffer_done( &buffer );
  return ret;
}


/**************************************************************************
 *
 * Paint truncated at the end of the table.
 *
 */

static int
test_truncated( FT_Library   library,
                const char*  filename )
{
  FontBuffer      buffer;
  FT_Face         face;
  FT_OpaquePaint  root;
  FT_COLR_Paint   paint;
  int             ret = 1;


  face = open_buffer( library, &buffer, filename );
  if ( !face )
    return 1;

  /* Glyph 1's root is a 'PaintSolid' table that is one byte too short to
   * hold the alpha value.  Parsing must stop at the table boundary rather
   * than read the byte that would follow it. */
  root.p                     = NULL;
  root.insert_root_transform = 0;

  if ( !FT_Get_Color_Glyph_Paint( face, 1, FT_COLOR_NO_ROOT_TRANSFORM,
                                  &root ) )
  {
    fprintf( stderr, "%s: expected a root paint\n", filename );
    goto Exit;
  }

  if ( FT_Get_Paint( face, root, &paint ) )
  {
    fprintf( stderr, "%s: parsed a paint that overruns the table\n",
             filename );
    goto Exit;
  }

  ret = 0;

Exit:
  FT_Done_Face( face );
  font_buffer_done( &buffer );
  return ret;
}


int
main( int     argc,
      char**  argv )
{
  FT_Library  library;
  int         ret;


  if ( argc != 6 )
  {
    fprintf( stderr, "usage: %s valid.ttf invalid.ttf oob-layer.ttf "
                     "cycle.ttf truncated.ttf\n", argv[0] );
    return 1;
  }

  if ( FT_Init_FreeType( &library ) )
  {
    fprintf( stderr, "could not initialize FreeType\n" );
    return 1;
  }

  ret = test_valid( library, argv[1] );
  ret |= test_invalid( library, argv[2] );
  ret |= test_oob_layer( library, argv[3] );
  ret |= test_cycle( library, argv[4] );
  ret |= test_truncated( library, argv[5] );

  FT_Done_FreeType( library );
  return ret;
}

/* EOF */
