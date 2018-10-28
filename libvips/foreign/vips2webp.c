/* wrap libwebp libray for write
 *
 * 6/8/13
 * 	- from vips2jpeg.c
 * 31/5/16
 * 	- buffer write ignored lossless, thanks aaron42net
 * 2/5/16 Felix Bünemann
 * 	- used advanced encoding API, expose controls 
 * 8/11/16
 * 	- add metadata write
 */

/*

    This file is part of VIPS.
    
    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/*
#define DEBUG
#define VIPS_DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#ifdef HAVE_LIBWEBP

#include <stdlib.h>
#include <string.h>

#include <vips/vips.h>
#include <vips/internal.h>

#include "pforeign.h"

#include <webp/encode.h>
#ifdef HAVE_LIBWEBPMUX
#include <webp/mux.h>
#endif /*HAVE_LIBWEBPMUX*/

typedef int (*webp_import)( WebPPicture *picture,
	const uint8_t *rgb, int stride );

typedef struct {
	VipsImage *image;

	int Q;
	gboolean lossless;
	VipsForeignWebpPreset preset;
	gboolean smart_subsample;
	gboolean near_lossless;
	int alpha_q;
	gboolean strip;

	WebPConfig config;

	/* We can only really support memory write, thanks to the current webp
	 * API. When we add metadata, we may need to go back and rewrite the 
	 * header.
	 */

	uint8_t *mem;

	/* We want to be able to detect >4gb even on machines that have size_t
	 * as uint32.
	 */
	guint64 size;
	guint64 max_size;
} VipsWebPWrite;

static WebPPreset
get_preset( VipsForeignWebpPreset preset )
{
	switch( preset ) {
	case VIPS_FOREIGN_WEBP_PRESET_DEFAULT:
		return( WEBP_PRESET_DEFAULT );
	case VIPS_FOREIGN_WEBP_PRESET_PICTURE:
		return( WEBP_PRESET_PICTURE );
	case VIPS_FOREIGN_WEBP_PRESET_PHOTO:
		return( WEBP_PRESET_PHOTO );
	case VIPS_FOREIGN_WEBP_PRESET_DRAWING:
		return( WEBP_PRESET_DRAWING );
	case VIPS_FOREIGN_WEBP_PRESET_ICON:
		return( WEBP_PRESET_ICON );
	case VIPS_FOREIGN_WEBP_PRESET_TEXT:
		return( WEBP_PRESET_TEXT );

	default:
		g_assert_not_reached();
	}

	/* Keep -Wall happy.
	 */
	return( -1 );
}

static void
vips_webp_write_unset( VipsWebPWrite *write )
{
	VIPS_FREE( write->mem );
}

static int
vips_webp_write_init( VipsWebPWrite *write, VipsImage *image,
	int Q, gboolean lossless, VipsForeignWebpPreset preset,
	gboolean smart_subsample, gboolean near_lossless,
	int alpha_q, gboolean strip )
{
	write->image = image;

	write->Q = Q;
	write->lossless = lossless;
	write->preset = preset;
	write->smart_subsample = smart_subsample;
	write->near_lossless = near_lossless;
	write->alpha_q = alpha_q;
	write->strip = strip;

	write->mem = NULL;
	write->size = 0;
	write->max_size = 0;

	if( !WebPConfigInit( &write->config ) ) {
		vips_webp_write_unset( write );
		vips_error( "vips2webp",
			"%s", _( "config version error" ) );
		return( -1 );
	}

	/* These presets are only for lossy compression. There seems to be
	 * separate API for lossless or near-lossless, see
	 * WebPConfigLosslessPreset().
	 */
	if( !(lossless || near_lossless) &&
		!WebPConfigPreset( &write->config, get_preset( preset ), Q ) ) {
		vips_webp_write_unset( write );
		vips_error( "vips2webp", "%s", _( "config version error" ) );
		return( -1 );
	}

#if WEBP_ENCODER_ABI_VERSION >= 0x0100
	write->config.lossless = lossless || near_lossless;
	write->config.alpha_quality = alpha_q;
#else
	if( lossless || 
		near_lossless )
		g_warning( "%s", _( "lossless unsupported" ) );
	if( alpha_q != 100 )
		g_warning( "%s", _( "alpha_q unsupported" ) );
#endif

#if WEBP_ENCODER_ABI_VERSION >= 0x0209
	if( near_lossless )
		write->config.near_lossless = Q;
	if( smart_subsample )
		write->config.preprocessing |= 4;
#else
	if( near_lossless )
		g_warning( "%s", _( "near_lossless unsupported" ) );
	if( smart_subsample )
		g_warning( "%s", _( "smart_subsample unsupported" ) );
#endif

	if( !WebPValidateConfig( &write->config ) ) {
		vips_webp_write_unset( write );
		vips_error( "vips2webp", "%s", _( "invalid configuration" ) );
		return( -1 );
	}

	return( 0 );
}

static gboolean
vips_webp_write_append( VipsWebPWrite *write, 
	const uint8_t *data, guint64 data_size )
{
	guint64 next_size;

	next_size = write->size + data_size;

	if( next_size > write->max_size ) {
		uint8_t *new_mem;
		const guint64 next_max_size =
			VIPS_MAX( 8192, VIPS_MAX( next_size, 
				write->max_size * 2 ) );

		/* We should let it creep up to 4gb rather than just
		 * blocking when max goes over, but no one will make a >2gb
		 * webp image.
		 */
		if( next_max_size > UINT_MAX ) {
			vips_error( "webp", 
				"%s", _( "output webp image too large" ) );
			return( 0 );
		}

		if( !(new_mem = (uint8_t *) 
			g_try_realloc( write->mem, next_max_size )) ) {
			vips_error( "webp", "%s", _( "out of memory" ) );
			return( 0 );
		}

		write->mem = new_mem;
		write->max_size = next_max_size;
	}

	if( data_size > 0 ) {
		memcpy( write->mem + write->size, data, data_size );
		write->size += data_size;
	}

	return( 1 );
}

/* We don't actually use libwebpmux here, but we shouldn't attach metadata we
 * can't read back because we'll be unable to test ourselves.
 *
 * Only attach metadata if we have something to read it back, otherwise
 * lots of our tests start failing.
 */
#ifdef HAVE_LIBWEBPMUX
static gboolean
vips_webp_write_appendle( VipsWebPWrite *write, uint32_t val, int n )
{
	unsigned char buf[4];
	int i; 

	g_assert( n <= 4 );

	for( i = 0; i < n; i++ ) {
		buf[i] = (unsigned char) (val & 0xff);
		val >>= 8;
	}

	return( vips_webp_write_append( write, buf, n ) ); 
}

static gboolean
vips_webp_write_appendle32( VipsWebPWrite *write, uint32_t val )
{
	return( vips_webp_write_appendle( write, val, 4 ) ); 
}

static gboolean
vips_webp_write_appendle24( VipsWebPWrite *write, uint32_t val )
{
	return( vips_webp_write_appendle( write, val, 3 ) ); 
}

static gboolean
vips_webp_write_appendcc( VipsWebPWrite *write, const char buf[4] )
{
	return( vips_webp_write_append( write, (const uint8_t *) buf, 4 ) ); 
}

static gboolean
vips_webp_write_appendc( VipsWebPWrite *write, 
	const char fourcc[4], const uint8_t *data, guint64 data_size )
{
	const int zero = 0;
	gboolean need_padding = (data_size & 1) != 0;

	if( !vips_webp_write_appendcc( write, fourcc ) ||
		!vips_webp_write_appendle32( write, data_size ) ||
		!vips_webp_write_append( write, data, data_size ) )
		return( 0 );

	if( need_padding &&
		!vips_webp_write_append( write, (const uint8_t *) &zero, 1 ) )
		return( 0 );

	return( 1 );
}
#endif /*HAVE_LIBWEBPMUX*/

static gboolean
memory_writer( const uint8_t *data, size_t data_size,
	const WebPPicture *picture ) 
{
	VipsWebPWrite * const write = 
		(VipsWebPWrite *) picture->custom_ptr;

	if( !write )
		return( 0 );

	return( vips_webp_write_append( write, data, data_size ) ); 
}

static gboolean
vips_webp_pic_init( VipsWebPWrite *write, WebPPicture *pic )
{
	if( !WebPPictureInit( pic ) ) {
		vips_error( "vips2webp", "%s", _( "picture version error" ) );
		return( FALSE );
	}
	pic->writer = memory_writer;
	pic->custom_ptr = write;

#if WEBP_ENCODER_ABI_VERSION >= 0x0100
	/* Smart subsampling needs use_argb because it is applied during 
	 * RGB to YUV conversion.
	 */
	pic->use_argb = write->lossless || 
		write->near_lossless || 
		write->smart_subsample;
#endif

	return( TRUE );
}

/* Write a VipsImage into a webp stream.
 */
static int
write_webp( VipsWebPWrite *write, VipsImage *image ) 
{
	VipsImage *memory;
	webp_import import;
	WebPPicture pic;

	if( !vips_webp_pic_init( write, &pic ) ) 
		return( -1 );

	if( !(memory = vips_image_copy_memory( image )) )
		return( -1 );

	pic.width = memory->Xsize;
	pic.height = memory->Ysize;

	if( memory->Bands == 4 )
		import = WebPPictureImportRGBA;
	else
		import = WebPPictureImportRGB;

	if( !import( &pic, VIPS_IMAGE_ADDR( memory, 0, 0 ),
		VIPS_IMAGE_SIZEOF_LINE( memory ) ) ) {
		VIPS_UNREF( memory );
		WebPPictureFree( &pic );
		vips_error( "vips2webp", "%s", _( "picture memory error" ) );
		return( -1 );
	}

	if( !WebPEncode( &write->config, &pic ) ) {
		VIPS_UNREF( memory );
		WebPPictureFree( &pic );
		vips_error( "vips2webp", "%s", _( "unable to encode" ) );
		return( -1 );
	}

	VIPS_UNREF( memory );

	WebPPictureFree( &pic );

	return( 0 );
}

#ifdef HAVE_WEBPANIMENCODEROPTIONSINIT
static int
write_webp_anim( WebPPicture *pic, VipsImage *in )
{
	WebPAnimEncoderOptions anim_config;
	WebPAnimEncoder *enc;

	if( !WebPAnimEncoderOptionsInit( &anim_config ) ) {
		vips_error( "vips2webp",
			"%s", _( "config version error" ) );
		return( -1 );
	}
	enc = NULL;

	return( 0 );
}
#endif /*HAVE_WEBPANIMENCODEROPTIONSINIT*/

#ifdef HAVE_LIBWEBPMUX
static int
vips_webp_add_chunk( VipsWebPWrite *write, VipsImage *image, 
	const char *vips, const char webp[4] )
{
	if( vips_image_get_typeof( image, vips ) ) {
		void *data;
		size_t length;

		/* We've done this before, it can't fail now.
		 */
		(void) vips_image_get_blob( image, vips, &data, &length );

		if( !vips_webp_write_appendc( write, webp, data, length ) )
			return( -1 ); 
	}

	return( 0 );
}

/* Turn @write into a VP8X image with metadata from @image.
 *
 * Based (partly) on cwep.c
 *
 * We need to know some detail about the webp file format. The encoder will
 * make something like this:
 *
 * 0  - 3  RIFF
 * 4  - 7  size of this data chunk (byte 8 to end of file)
 * 8  - 11 WEBP
 * 12 - 15 VP8L (L for lossless, space for lossy, possibly 8 for alpha)
 * 16 - 19 size of chunk (10 for VP8X, something else otherwise)
 * 20 - 23 flags (VP8L has alpha in bit 29 of this; byte 20 has flags for VP8X)
 * 24 -
 *
 * If there is metadata to add, we make a VP8X image, which looks like this:
 *
 * 0  - 3  RIFF
 * 4  - 7  size of this data chunk (byte 8 to end of file)
 * 8  - 11 WEBP
 * 12 - 15 VP8X
 * 16 - 19 10 (size of vp8x chunk)
 * 20 - 23 flags 
 * 24 - 26 width - 1 (note: only 3 bytes)
 * 27 - 29 height - 1 (note: only 3 bytes)
 * 30 -
 *
 * Followed by ICCP, ANIM, image, EXIF and XMP chunks, in that order.
 *
 * See:
 *     https://developers.google.com/speed/webp/docs/riff_container
 */
static int
vips_webp_add_metadata( VipsWebPWrite *write, VipsImage *image )
{
	/* The image in @write may be VP8X already.
	 */
	gboolean is_vp8x = !memcmp( write->mem + 12, "VP8X", 4 );
	gboolean is_lossless = !memcmp( write->mem + 12, "VP8L", 4 );

	guint64 metadata_size;
	uint32_t flags;
	int i;

	guint64 new_size;
	uint8_t *old_mem;
	guint64 old_size;

	/* Rebuild the EXIF block, if any, ready for writing. 
	 */
	if( vips__exif_update( image ) )
		return( -1 ); 

	/* We have to find the size of the block we will write before we can
	 * start to write it.
	 */
	metadata_size = 0;

	/* If there are any flags there already, we add to them.
	 */
	flags = is_vp8x ? write->mem[20] : 0;

	for( i = 0; i < vips__n_webp_names; i++ ) { 
		const char *vips = vips__webp_names[i].vips;
		uint32_t flag = vips__webp_names[i].flag;

		if( vips_image_get_typeof( image, vips ) ) {
			void *data;
			size_t length;

			if( vips_image_get_blob( image, vips, &data, &length ) )
				return( -1 );

			/* +8 since we have to prepend each chunk with a type
			 * char[4] and a length guint32. 
			 *
			 * Chunks are always rounded up to an even size.
			 */
			metadata_size += length + 8 + (length & 1);

			flags |= flag;
		}
	}

	if( !metadata_size ) 
		/* No metadata to write, so we can just leave the image alone.
		 */
		return( 0 );

	/* If it's not already vp8x, we'll need to add a vp8x header, and
	 * that'll add 18 bytes. -8 since size includes the RIFF header.
	 */
	new_size = write->size - 8 + (is_vp8x ? 0 : 18) + metadata_size;

	/* This is really terrible.
	 *
	 * Note the old memory buffer and reset writer to be empty. We write
	 * the entire image again, pasting bits together from the previous
	 * write.
	 */

	old_size = write->size;
	old_mem = write->mem;

	write->mem = NULL;
	write->size = 0;
	write->max_size = 0;

	if( !vips_webp_write_appendcc( write, "RIFF" ) ||
		!vips_webp_write_appendle32( write, new_size ) ||
		!vips_webp_write_appendcc( write, "WEBP" ) ||
		!vips_webp_write_appendcc( write, "VP8X" ) ||
		!vips_webp_write_appendle32( write, 10 ) ) { 
		VIPS_FREE( old_mem );
		return( -1 );
	}

	if( is_vp8x ) {
		/* Copy the existing VP8X body and update the flag bits.
		 */
		if( !vips_webp_write_append( write, old_mem + 20, 10 ) ) {
			VIPS_FREE( old_mem );
			return( -1 );
		}
		write->mem[20] = flags;
	}
	else {
		/* We have to make a new vp8x header.
		 */

		/* Presence of alpha is stored in the 29th bit of VP8L 
		 * data. 
		 *
		 * +12 gets us to the VP8L cc, 4 to skip that, 
		 * another 4 to skip the length, then bit 8 - 3 == 5
		 */
		if( is_lossless &&
			(old_mem[12 + 8 + 3] & (1 << 5)) )
			flags |= 0x010;

		/* 10 is the length of the VPX8X header chunk.
		 */
		if( !vips_webp_write_appendle32( write, flags ) ||
			!vips_webp_write_appendle24( write, 
				image->Xsize - 1 ) ||
			!vips_webp_write_appendle24( write, 
				image->Ysize - 1 ) ) {
			VIPS_FREE( old_mem );
			return( -1 );
		}
	}

	/* Extra chunks have to be in this order.
	 */
	if( vips_webp_add_chunk( write, image, VIPS_META_ICC_NAME, "ICCP" ) ) {
		VIPS_FREE( old_mem );
		return( -1 );
	}

	/* The image chunk must come here.
	 */
	if( is_vp8x ) {
		if( !vips_webp_write_append( write, 
			old_mem + 30, old_size - 30 ) ) {
			VIPS_FREE( old_mem );
			return( -1 );
		}
	}
	else {
		if( !vips_webp_write_append( write, 
			old_mem + 12, old_size - 12 ) ) {
			VIPS_FREE( old_mem );
			return( -1 );
		}
	}

	VIPS_FREE( old_mem );

	if( vips_webp_add_chunk( write, image, VIPS_META_EXIF_NAME, "EXIF" ) ) 
		return( -1 );

	if( vips_webp_add_chunk( write, image, VIPS_META_XMP_NAME, "XMP " ) ) 
		return( -1 );

	return( 0 );
}
#endif /*HAVE_LIBWEBPMUX*/

int
vips__webp_write_file( VipsImage *in, const char *filename, 
	int Q, gboolean lossless, VipsForeignWebpPreset preset,
	gboolean smart_subsample, gboolean near_lossless,
	int alpha_q, gboolean strip )
{
	VipsWebPWrite write;
	FILE *fp;

	if( vips_webp_write_init( &write, in,
		Q, lossless, preset, smart_subsample, near_lossless,
		alpha_q, strip ) )
		return( -1 );

	if( write_webp( &write, in ) ) { 
		vips_webp_write_unset( &write );
		return( -1 );
	}

#ifdef HAVE_LIBWEBPMUX
	if( !strip &&
		vips_webp_add_metadata( &write, in ) ) {
		vips_webp_write_unset( &write );
		return( -1 );
	}
#endif /*HAVE_LIBWEBPMUX*/

	if( !(fp = vips__file_open_write( filename, FALSE )) ) {
		vips_webp_write_unset( &write );
		return( -1 );
	}

	if( vips__file_write( write.mem, write.size, 1, fp ) ) {
		fclose( fp );
		vips_webp_write_unset( &write );
		return( -1 );
	}

	fclose( fp );

	vips_webp_write_unset( &write );

	return( 0 );
}

int
vips__webp_write_buffer( VipsImage *in, void **obuf, size_t *olen, 
	int Q, gboolean lossless, VipsForeignWebpPreset preset,
	gboolean smart_subsample, gboolean near_lossless,
	int alpha_q, gboolean strip )
{
	VipsWebPWrite write;

	if( vips_webp_write_init( &write, in,
		Q, lossless, preset, smart_subsample, near_lossless,
		alpha_q, strip ) )
		return( -1 );

	if( write_webp( &write, in ) ) {
		vips_webp_write_unset( &write );
		return( -1 );
	}

#ifdef HAVE_LIBWEBPMUX
	if( !strip &&
		vips_webp_add_metadata( &write, in ) ) {
		vips_webp_write_unset( &write );
		return( -1 );
	}
#endif /*HAVE_LIBWEBPMUX*/

	*obuf = write.mem;
	write.mem = NULL;
	*olen = write.size;

	vips_webp_write_unset( &write );

	return( 0 );
}

#endif /*HAVE_LIBWEBP*/
