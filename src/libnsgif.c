/*
 * Copyright 2004 Richard Wilson <richard.wilson@netsurf-browser.org>
 * Copyright 2008 Sean Fox <dyntryx@gmail.com>
 *
 * This file is part of NetSurf's libnsgif, http://www.netsurf-browser.org/
 * Licenced under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "libnsgif.h"
#include "utils/log.h"

#include "lzw.h"

/**
 *
 * \file
 * \brief GIF image decoder
 *
 * The GIF format is thoroughly documented; a full description can be found at
 * http://www.w3.org/Graphics/GIF/spec-gif89a.txt
 *
 * \todo Plain text and comment extensions should be implemented.
 */


/** Maximum colour table size */
#define GIF_MAX_COLOURS 256

/** Internal flag that the colour table needs to be processed */
#define GIF_PROCESS_COLOURS 0xaa000000

/** Internal flag that a frame is invalid/unprocessed */
#define GIF_INVALID_FRAME -1

/** Transparent colour */
#define GIF_TRANSPARENT_COLOUR 0x00

/** No transparency */
#define GIF_NO_TRANSPARENCY (0xFFFFFFFFu)

enum gif_disposal {
	GIF_DISPOSAL_UNSPECIFIED,
	GIF_DISPOSAL_NONE,
	GIF_DISPOSAL_RESTORE_BG,
	GIF_DISPOSAL_RESTORE_PREV,
	GIF_DISPOSAL_RESTORE_QUIRK, /**< Alias for GIF_DISPOSAL_RESTORE_PREV. */
};

/* GIF Flags */
#define GIF_INTERLACE_MASK 0x40
#define GIF_COLOUR_TABLE_MASK 0x80
#define GIF_COLOUR_TABLE_SIZE_MASK 0x07
#define GIF_EXTENSION_INTRODUCER 0x21
#define GIF_EXTENSION_GRAPHIC_CONTROL 0xf9
#define GIF_DISPOSAL_MASK 0x1c
#define GIF_TRANSPARENCY_MASK 0x01
#define GIF_EXTENSION_COMMENT 0xfe
#define GIF_EXTENSION_PLAIN_TEXT 0x01
#define GIF_EXTENSION_APPLICATION 0xff
#define GIF_BLOCK_TERMINATOR 0x00
#define GIF_TRAILER 0x3b

/** standard GIF header size */
#define GIF_STANDARD_HEADER_SIZE 13

static gif_result gif_error_from_lzw(lzw_result l_res)
{
	static const gif_result g_res[] = {
		[LZW_OK]	= GIF_OK,
		[LZW_OK_EOD]    = GIF_END_OF_FRAME,
		[LZW_NO_MEM]    = GIF_INSUFFICIENT_MEMORY,
		[LZW_NO_DATA]   = GIF_INSUFFICIENT_FRAME_DATA,
		[LZW_EOI_CODE]  = GIF_FRAME_DATA_ERROR,
		[LZW_BAD_ICODE] = GIF_FRAME_DATA_ERROR,
		[LZW_BAD_CODE]  = GIF_FRAME_DATA_ERROR,
	};
	assert(l_res != LZW_BAD_PARAM);
	assert(l_res != LZW_NO_COLOUR);
	return g_res[l_res];
}

/**
 * Updates the sprite memory size
 *
 * \param gif The animation context
 * \param width The width of the sprite
 * \param height The height of the sprite
 * \return GIF_INSUFFICIENT_MEMORY for a memory error GIF_OK for success
 */
static gif_result gif_initialise_sprite(
		struct gif_animation *gif,
		uint32_t width,
		uint32_t height)
{
	/* Already allocated? */
	if (gif->frame_image) {
		return GIF_OK;
	}

	assert(gif->bitmap_callbacks.bitmap_create);
	gif->frame_image = gif->bitmap_callbacks.bitmap_create(width, height);
	if (gif->frame_image == NULL) {
		return GIF_INSUFFICIENT_MEMORY;
	}

	return GIF_OK;
}

/**
 * Helper to get the rendering bitmap for a gif.
 *
 * \param[in]  gif  The gif object we're decoding.
 * \return Client pixel buffer for rendering into.
 */
static inline uint32_t* gif__bitmap_get(
		struct gif_animation *gif)
{
	gif_result ret;

	/* Make sure we have a buffer to decode to. */
	ret = gif_initialise_sprite(gif, gif->width, gif->height);
	if (ret != GIF_OK) {
		return NULL;
	}

	/* Get the frame data */
	assert(gif->bitmap_callbacks.bitmap_get_buffer);
	return (uint32_t *)gif->bitmap_callbacks.bitmap_get_buffer(
			gif->frame_image);
}

/**
 * Helper to tell the client that their bitmap was modified.
 *
 * \param[in]  gif  The gif object we're decoding.
 */
static inline void gif__bitmap_modified(
		const struct gif_animation *gif)
{
	if (gif->bitmap_callbacks.bitmap_modified) {
		gif->bitmap_callbacks.bitmap_modified(gif->frame_image);
	}
}

/**
 * Helper to tell the client that whether the bitmap is opaque.
 *
 * \param[in]  gif    The gif object we're decoding.
 * \param[in]  frmae  The frame that has been decoded.
 */
static inline void gif__bitmap_set_opaque(
		const struct gif_animation *gif,
		const struct gif_frame *frame)
{
	if (gif->bitmap_callbacks.bitmap_set_opaque) {
		gif->bitmap_callbacks.bitmap_set_opaque(
				gif->frame_image, frame->opaque);
	}
}

/**
 * Helper to get the client to determine if the bitmap is opaque.
 *
 * \todo: We don't really need to get the client to do this for us.
 *
 * \param[in]  gif    The gif object we're decoding.
 * \return true if the bitmap is opaque, false otherwise.
 */
static inline bool gif__bitmap_get_opaque(
		const struct gif_animation *gif)
{
	if (gif->bitmap_callbacks.bitmap_test_opaque) {
		return gif->bitmap_callbacks.bitmap_test_opaque(
				gif->frame_image);
	}

	return false;
}

static void gif__record_frame(
		struct gif_animation *gif,
		const uint32_t *bitmap)
{
	bool need_alloc = gif->prev_frame == NULL;
	uint32_t *prev_frame;

	if (gif->decoded_frame == GIF_INVALID_FRAME ||
	    gif->decoded_frame == gif->prev_index) {
		/* No frame to copy, or already have this frame recorded. */
		return;
	}

	bitmap = gif__bitmap_get(gif);
	if (bitmap == NULL) {
		return;
	}

	if (gif->prev_frame != NULL &&
	    gif->width * gif->height > gif->prev_width * gif->prev_height) {
		need_alloc = true;
	}

	if (need_alloc) {
		prev_frame = realloc(gif->prev_frame,
				gif->width * gif->height * 4);
		if (prev_frame == NULL) {
			return;
		}
	} else {
		prev_frame = gif->prev_frame;
	}

	memcpy(prev_frame, bitmap, gif->width * gif->height * 4);

	gif->prev_frame  = prev_frame;
	gif->prev_width  = gif->width;
	gif->prev_height = gif->height;
	gif->prev_index  = gif->decoded_frame;
}

static gif_result gif__recover_frame(
		const struct gif_animation *gif,
		uint32_t *bitmap)
{
	const uint32_t *prev_frame = gif->prev_frame;
	unsigned height = gif->height < gif->prev_height ? gif->height : gif->prev_height;
	unsigned width  = gif->width  < gif->prev_width  ? gif->width  : gif->prev_width;

	if (prev_frame == NULL) {
		return GIF_FRAME_DATA_ERROR;
	}

	for (unsigned y = 0; y < height; y++) {
		memcpy(bitmap, prev_frame, width * 4);

		bitmap += gif->width;
		prev_frame += gif->prev_width;
	}

	return GIF_OK;
}

static uint32_t gif_interlaced_line(int height, int y)
{
	if ((y << 3) < height) {
		return (y << 3);
	}
	y -= ((height + 7) >> 3);
	if ((y << 3) < (height - 4)) {
		return (y << 3) + 4;
	}
	y -= ((height + 3) >> 3);
	if ((y << 2) < (height - 2)) {
		return (y << 2) + 2;
	}
	y -= ((height + 1) >> 2);
	return (y << 1) + 1;
}

static gif_result gif__decode_complex(
		struct gif_animation *gif,
		uint32_t width,
		uint32_t height,
		uint32_t offset_x,
		uint32_t offset_y,
		uint32_t interlace,
		uint8_t minimum_code_size,
		uint32_t transparency_index,
		uint32_t *restrict frame_data,
		uint32_t *restrict colour_table)
{
	uint32_t available = 0;
	gif_result ret = GIF_OK;
	lzw_result res;

	/* Initialise the LZW decoding */
	res = lzw_decode_init(gif->lzw_ctx, minimum_code_size,
			gif->gif_data, gif->buffer_size, gif->buffer_position);
	if (res != LZW_OK) {
		return gif_error_from_lzw(res);
	}

	for (uint32_t y = 0; y < height; y++) {
		uint32_t x;
		uint32_t decode_y;
		uint32_t *frame_scanline;

		if (interlace) {
			decode_y = gif_interlaced_line(height, y) + offset_y;
		} else {
			decode_y = y + offset_y;
		}
		frame_scanline = frame_data + offset_x + (decode_y * gif->width);

		x = width;
		while (x > 0) {
			const uint8_t *uncompressed;
			unsigned row_available;
			if (available == 0) {
				if (res != LZW_OK) {
					/* Unexpected end of frame, try to recover */
					if (res == LZW_OK_EOD) {
						ret = GIF_OK;
					} else {
						ret = gif_error_from_lzw(res);
					}
					break;
				}
				res = lzw_decode(gif->lzw_ctx,
						&uncompressed, &available);
			}

			row_available = x < available ? x : available;
			x -= row_available;
			available -= row_available;
			if (transparency_index > 0xFF) {
				while (row_available-- > 0) {
					*frame_scanline++ =
						colour_table[*uncompressed++];
				}
			} else {
				while (row_available-- > 0) {
					register uint32_t colour;
					colour = *uncompressed++;
					if (colour != transparency_index) {
						*frame_scanline =
							colour_table[colour];
					}
					frame_scanline++;
				}
			}
		}
	}
	return ret;
}

static gif_result gif__decode_simple(
		struct gif_animation *gif,
		uint32_t height,
		uint32_t offset_y,
		uint8_t minimum_code_size,
		uint32_t transparency_index,
		uint32_t *restrict frame_data,
		uint32_t *restrict colour_table)
{
	uint32_t pixels = gif->width * height;
	uint32_t written = 0;
	gif_result ret = GIF_OK;
	lzw_result res;

	/* Initialise the LZW decoding */
	res = lzw_decode_init_map(gif->lzw_ctx,
			minimum_code_size, transparency_index, colour_table,
			gif->gif_data, gif->buffer_size, gif->buffer_position);
	if (res != LZW_OK) {
		return gif_error_from_lzw(res);
	}

	frame_data += (offset_y * gif->width);

	while (pixels > 0) {
		res = lzw_decode_map(gif->lzw_ctx,
				frame_data, pixels, &written);
		pixels -= written;
		frame_data += written;
		if (res != LZW_OK) {
			/* Unexpected end of frame, try to recover */
			if (res == LZW_OK_EOD) {
				ret = GIF_OK;
			} else {
				ret = gif_error_from_lzw(res);
			}
			break;
		}
	}

	if (pixels == 0) {
		ret = GIF_OK;
	}

	return ret;
}

static inline gif_result gif__decode(
		struct gif_animation *gif,
		struct gif_frame *frame,
		uint8_t minimum_code_size,
		uint32_t *restrict frame_data)
{
	gif_result ret;
	uint32_t offset_x = frame->redraw_x;
	uint32_t offset_y = frame->redraw_y;
	uint32_t width = frame->redraw_width;
	uint32_t height = frame->redraw_height;
	uint32_t interlace = frame->flags & GIF_INTERLACE_MASK;
	uint32_t transparency_index = frame->transparency_index;
	uint32_t *restrict colour_table = gif->colour_table;

	if (interlace == false && width == gif->width && offset_x == 0) {
		ret = gif__decode_simple(gif, height, offset_y,
				minimum_code_size, transparency_index,
				frame_data, colour_table);
	} else {
		ret = gif__decode_complex(gif, width, height,
				offset_x, offset_y, interlace,
				minimum_code_size, transparency_index,
				frame_data, colour_table);
	}

	return ret;
}

/**
 * Restore a GIF to the background colour.
 *
 * \param[in] gif     The gif object we're decoding.
 * \param[in] frame   The frame to clear, or NULL.
 * \param[in] bitmap  The bitmap to clear the frame in.
 */
static void gif__restore_bg(
		struct gif_animation *gif,
		struct gif_frame *frame,
		uint32_t *bitmap)
{
	if (frame == NULL) {
		memset(bitmap, GIF_TRANSPARENT_COLOUR,
				gif->width * gif->height * sizeof(*bitmap));
	} else {
		uint32_t offset_x = frame->redraw_x;
		uint32_t offset_y = frame->redraw_y;
		uint32_t width = frame->redraw_width;
		uint32_t height = frame->redraw_height;

		if (frame->display == false) {
			return;
		}

		if (frame->transparency) {
			for (uint32_t y = 0; y < height; y++) {
				uint32_t *scanline = bitmap + offset_x +
						(offset_y + y) * gif->width;
				memset(scanline, GIF_TRANSPARENT_COLOUR,
						width * sizeof(*bitmap));
			}
		} else {
			for (uint32_t y = 0; y < height; y++) {
				uint32_t *scanline = bitmap + offset_x +
						(offset_y + y) * gif->width;
				for (uint32_t x = 0; x < width; x++) {
					scanline[x] = gif->bg_colour;
				}
			}
		}
	}
}

static gif_result gif__update_bitmap(
		struct gif_animation *gif,
		struct gif_frame *frame,
		uint8_t minimum_code_size,
		uint32_t frame_idx)
{
	gif_result ret;
	uint32_t *bitmap;

	gif->decoded_frame = frame_idx;

	bitmap = gif__bitmap_get(gif);
	if (bitmap == NULL) {
		return GIF_INSUFFICIENT_MEMORY;
	}

	/* Handle any bitmap clearing/restoration required before decoding this
	 * frame. */
	if (frame_idx == 0 || gif->decoded_frame == GIF_INVALID_FRAME) {
		gif__restore_bg(gif, NULL, bitmap);

	} else {
		struct gif_frame *prev = &gif->frames[frame_idx - 1];

		if (prev->disposal_method == GIF_DISPOSAL_RESTORE_BG) {
			gif__restore_bg(gif, prev, bitmap);

		} else if (prev->disposal_method == GIF_DISPOSAL_RESTORE_PREV) {
			ret = gif__recover_frame(gif, bitmap);
			if (ret != GIF_OK) {
				gif__restore_bg(gif, prev, bitmap);
			}
		}
	}

	if (frame->disposal_method == GIF_DISPOSAL_RESTORE_PREV) {
		/* Store the previous frame for later restoration */
		gif__record_frame(gif, bitmap);
	}

	ret = gif__decode(gif, frame, minimum_code_size, bitmap);

	gif__bitmap_modified(gif);

	if (frame->virgin) {
		frame->opaque = gif__bitmap_get_opaque(gif);
		frame->virgin = false;
	}
	gif__bitmap_set_opaque(gif, frame);

	return ret;
}

/**
 * Parse the application extension
 *
 * \param[in] frame  The gif object we're decoding.
 * \param[in] data   The data to decode.
 * \param[in] len    Byte length of data.
 * \return GIF_INSUFFICIENT_FRAME_DATA if more data is needed,
 *         GIF_OK for success.
 */
static gif_result gif__parse_extension_graphic_control(
		struct gif_frame *frame,
		uint8_t *data,
		size_t len)
{
	/* 6-byte Graphic Control Extension is:
	 *
	 *  +0  CHAR    Graphic Control Label
	 *  +1  CHAR    Block Size
	 *  +2  CHAR    __Packed Fields__
	 *              3BITS   Reserved
	 *              3BITS   Disposal Method
	 *              1BIT    User Input Flag
	 *              1BIT    Transparent Color Flag
	 *  +3  SHORT   Delay Time
	 *  +5  CHAR    Transparent Color Index
	 */
	if (len < 6) {
		return GIF_INSUFFICIENT_FRAME_DATA;
	}

	frame->frame_delay = data[3] | (data[4] << 8);
	if (data[2] & GIF_TRANSPARENCY_MASK) {
		frame->transparency = true;
		frame->transparency_index = data[5];
	}

	frame->disposal_method = ((data[2] & GIF_DISPOSAL_MASK) >> 2);
	/* I have encountered documentation and GIFs in the
	 * wild that use 0x04 to restore the previous frame,
	 * rather than the officially documented 0x03.  I
	 * believe some (older?)  software may even actually
	 * export this way.  We handle this as a type of
	 * "quirks" mode. */
	if (frame->disposal_method == GIF_DISPOSAL_RESTORE_QUIRK) {
		frame->disposal_method = GIF_DISPOSAL_RESTORE_PREV;
	}

	/* if we are clearing the background then we need to
	 * redraw enough to cover the previous frame too. */
	frame->redraw_required =
			frame->disposal_method == GIF_DISPOSAL_RESTORE_BG ||
			frame->disposal_method == GIF_DISPOSAL_RESTORE_PREV;

	return GIF_OK;
}

/**
 * Parse the application extension
 *
 * \param[in] gif   The gif object we're decoding.
 * \param[in] data  The data to decode.
 * \param[in] len   Byte length of data.
 * \return GIF_INSUFFICIENT_FRAME_DATA if more data is needed,
 *         GIF_OK for success.
 */
static gif_result gif__parse_extension_application(
		struct gif_animation *gif,
		uint8_t *data,
		size_t len)
{
	/* 14-byte+ Application Extension is:
	 *
	 *  +0    CHAR    Application Extension Label
	 *  +1    CHAR    Block Size
	 *  +2    8CHARS  Application Identifier
	 *  +10   3CHARS  Appl. Authentication Code
	 *  +13   1-256   Application Data (Data sub-blocks)
	 */
	if (len < 17) {
		return GIF_INSUFFICIENT_FRAME_DATA;
	}

	if ((data[1] == 0x0b) &&
	    (strncmp((const char *)data + 2, "NETSCAPE2.0", 11) == 0) &&
	    (data[13] == 0x03) && (data[14] == 0x01)) {
		gif->loop_count = data[15] | (data[16] << 8);
	}

	return GIF_OK;
}

/**
 * Parse the frame's extensions
 *
 * \param[in] gif     The gif object we're decoding.
 * \param[in] frame   The frame to parse extensions for.
 * \param[in] decode  Whether to decode or skip over the extension.
 * \return GIF_INSUFFICIENT_FRAME_DATA if more data is needed,
 *         GIF_OK for success.
 */
static gif_result gif__parse_frame_extensions(
		struct gif_animation *gif,
		struct gif_frame *frame,
		bool decode)
{
	uint8_t *gif_data, *gif_end;
	int gif_bytes;

	/* Get our buffer position etc.	*/
	gif_data = gif->gif_data + gif->buffer_position;
	gif_end = gif->gif_data + gif->buffer_size;
	gif_bytes = gif_end - gif_data;

	/* Initialise the extensions */
	while (gif_bytes > 0 && gif_data[0] == GIF_EXTENSION_INTRODUCER) {
		bool block_step = true;
		gif_result ret;

		gif_data++;
		gif_bytes--;

		if (gif_bytes == 0) {
			return GIF_INSUFFICIENT_FRAME_DATA;
		}

		/* Switch on extension label */
		switch (gif_data[0]) {
		case GIF_EXTENSION_GRAPHIC_CONTROL:
			if (decode) {
				ret = gif__parse_extension_graphic_control(
						frame, gif_data, gif_bytes);
				if (ret != GIF_OK) {
					return ret;
				}
			}
			break;

		case GIF_EXTENSION_APPLICATION:
			if (decode) {
				ret = gif__parse_extension_application(
						gif, gif_data, gif_bytes);
				if (ret != GIF_OK) {
					return ret;
				}
			}
			break;

		case GIF_EXTENSION_COMMENT:
			/* Move the pointer to the first data sub-block Skip 1
			 * byte for the extension label. */
			++gif_data;
			block_step = false;
			break;

		default:
			break;
		}

		if (block_step) {
			/* Move the pointer to the first data sub-block Skip 2
			 * bytes for the extension label and size fields Skip
			 * the extension size itself
			 */
			if (gif_bytes < 2) {
				return GIF_INSUFFICIENT_FRAME_DATA;
			}
			gif_data += 2 + gif_data[1];
		}

		/* Repeatedly skip blocks until we get a zero block or run out
		 * of data.  This data is ignored by this gif decoder. */
		while (gif_data < gif_end && gif_data[0] != GIF_BLOCK_TERMINATOR) {
			gif_data += gif_data[0] + 1;
			if (gif_data >= gif_end) {
				return GIF_INSUFFICIENT_FRAME_DATA;
			}
		}
		gif_data++;
		gif_bytes = gif_end - gif_data;
	}

	if (gif_data > gif_end) {
		gif_data = gif_end;
	}

	/* Set buffer position and return */
	gif->buffer_position = gif_data - gif->gif_data;
	return GIF_OK;
}

/**
 * Parse a GIF Image Descriptor.
 *
 * The format is:
 *
 *  +0   CHAR   Image Separator (0x2c)
 *  +1   SHORT  Image Left Position
 *  +3   SHORT  Image Top Position
 *  +5   SHORT  Width
 *  +7   SHORT  Height
 *  +9   CHAR   __Packed Fields__
 *              1BIT    Local Colour Table Flag
 *              1BIT    Interlace Flag
 *              1BIT    Sort Flag
 *              2BITS   Reserved
 *              3BITS   Size of Local Colour Table
 *
 * \param[in] gif    The gif object we're decoding.
 * \param[in] frame  The frame to parse an image descriptor for.
 * \return GIF_OK on success, appropriate error otherwise.
 */
static gif_result gif__parse_image_descriptor(
		struct gif_animation *gif,
		struct gif_frame *frame,
		bool decode)
{
	const uint8_t *data = gif->gif_data + gif->buffer_position;
	size_t len = gif->buffer_size - gif->buffer_position;
	enum {
		GIF_IMAGE_DESCRIPTOR_LEN = 10u,
		GIF_IMAGE_SEPARATOR      = 0x2Cu,
	};

	assert(gif != NULL);
	assert(frame != NULL);

	if (len < GIF_IMAGE_DESCRIPTOR_LEN) {
		return GIF_INSUFFICIENT_FRAME_DATA;
	}

	if (decode) {
		unsigned x, y, w, h;

		if (data[0] != GIF_IMAGE_SEPARATOR) {
			return GIF_FRAME_DATA_ERROR;
		}

		x = data[1] | (data[2] << 8);
		y = data[3] | (data[4] << 8);
		w = data[5] | (data[6] << 8);
		h = data[7] | (data[8] << 8);
		frame->flags = data[9];

		frame->redraw_x      = x;
		frame->redraw_y      = y;
		frame->redraw_width  = w;
		frame->redraw_height = h;

		/* Frame size may have grown. */
		gif->width  = (x + w > gif->width ) ? x + w : gif->width;
		gif->height = (y + h > gif->height) ? y + h : gif->height;
	}

	gif->buffer_position += GIF_IMAGE_DESCRIPTOR_LEN;
	return GIF_OK;
}

/**
 * Get a frame's colour table.
 *
 * Sets up gif->colour_table for the frame.
 *
 * \param[in] gif    The gif object we're decoding.
 * \param[in] frame  The frame to get the colour table for.
 * \return GIF_OK on success, appropriate error otherwise.
 */
static gif_result gif__parse_colour_table(
		struct gif_animation *gif,
		struct gif_frame *frame,
		bool decode)
{
	const uint8_t *data = gif->gif_data + gif->buffer_position;
	size_t len = gif->buffer_size - gif->buffer_position;
	unsigned colour_table_size;

	assert(gif != NULL);
	assert(frame != NULL);

	if ((frame->flags & GIF_COLOUR_TABLE_MASK) == 0) {
		gif->colour_table = gif->global_colour_table;
		return GIF_OK;
	}

	colour_table_size = 2 << (frame->flags & GIF_COLOUR_TABLE_SIZE_MASK);
	if (len < colour_table_size * 3) {
		return GIF_INSUFFICIENT_FRAME_DATA;
	}

	if (decode) {
		int count = colour_table_size;
		uint8_t *entry = (uint8_t *)gif->local_colour_table;

		while (count--) {
			/* Gif colour map contents are r,g,b.
			 *
			 * We want to pack them bytewise into the
			 * colour table, such that the red component
			 * is in byte 0 and the alpha component is in
			 * byte 3.
			 */

			*entry++ = *data++; /* r */
			*entry++ = *data++; /* g */
			*entry++ = *data++; /* b */
			*entry++ = 0xff;    /* a */
		}
	}

	gif->buffer_position += colour_table_size * 3;
	gif->colour_table = gif->local_colour_table;
	return GIF_OK;
}

/**
 * Parse the image data for a gif frame.
 *
 * Sets up gif->colour_table for the frame.
 *
 * \param[in] gif    The gif object we're decoding.
 * \param[in] frame  The frame to get the colour table for.
 * \return GIF_OK on success, appropriate error otherwise.
 */
static gif_result gif__parse_image_data(
		struct gif_animation *gif,
		struct gif_frame *frame,
		bool decode)
{
	uint8_t *data = gif->gif_data + gif->buffer_position;
	size_t len = gif->buffer_size - gif->buffer_position;
	uint32_t frame_idx = frame - gif->frames;
	uint8_t minimum_code_size;
	gif_result ret;

	assert(gif != NULL);
	assert(frame != NULL);

	if (!decode) {
		gif->frame_count_partial = frame_idx + 1;
	}

	/* Ensure sufficient data remains.  A gif trailer or a minimum lzw code
	 * followed by a gif trailer is treated as OK, although without any
	 * image data. */
	switch (len) {
		default: if (data[0] == GIF_TRAILER) return GIF_OK;
			break;
		case 2: if (data[1] == GIF_TRAILER) return GIF_OK;
			/* Fall through. */
		case 1: if (data[0] == GIF_TRAILER) return GIF_OK;
			/* Fall through. */
		case 0: return GIF_INSUFFICIENT_FRAME_DATA;
	}

	minimum_code_size = data[0];
	if (minimum_code_size >= LZW_CODE_MAX) {
		return GIF_DATA_ERROR;
	}
	gif->buffer_position++;
	data++;
	len--;

	if (decode) {
		ret = gif__update_bitmap(gif, frame, minimum_code_size,
				frame_idx);
	} else {
		uint32_t block_size = 0;

		while (block_size != 1) {
			if (len < 1) return GIF_INSUFFICIENT_FRAME_DATA;
			block_size = data[0] + 1;
			/* Check if the frame data runs off the end of the file	*/
			if ((int)(len - block_size) < 0) {
				/* Try to recover by signaling the end of the gif.
				 * Once we get garbage data, there is no logical way to
				 * determine where the next frame is.  It's probably
				 * better to partially load the gif than not at all.
				 */
				if (len >= 2) {
					data[0] = 0;
					data[1] = GIF_TRAILER;
					len = 1;
					data++;
					break;
				} else {
					return GIF_INSUFFICIENT_FRAME_DATA;
				}
			} else {
				len -= block_size;
				data += block_size;
			}
		}

		gif->buffer_position = data - gif->gif_data;
		gif->frame_count = frame_idx + 1;
		gif->frames[frame_idx].display = true;

		/* Check if we've finished */
		if (len < 1) {
			return GIF_INSUFFICIENT_FRAME_DATA;
		} else {
			if (data[0] == GIF_TRAILER) {
				return GIF_OK;
			}
		}

		return GIF_WORKING;
	}

	return ret;
}

static struct gif_frame *gif__get_frame(
		struct gif_animation *gif,
		uint32_t frame_idx)
{
	struct gif_frame *frame;

	if (gif->frame_holders > frame_idx) {
		frame = &gif->frames[frame_idx];
	} else {
		/* Allocate more memory */
		size_t count = frame_idx + 1;
		struct gif_frame *temp;

		temp = realloc(gif->frames, count * sizeof(*frame));
		if (temp == NULL) {
			return NULL;
		}
		gif->frames = temp;
		gif->frame_holders = count;

		frame = &gif->frames[frame_idx];

		frame->transparency = false;
		frame->transparency_index = GIF_NO_TRANSPARENCY;
		frame->frame_pointer = gif->buffer_position;
		frame->redraw_required = false;
		frame->disposal_method = 0;
		frame->frame_delay = 100;
		frame->display = false;
		frame->virgin = true;
	}

	return frame;
}

/**
 * Attempts to initialise the next frame
 *
 * \param[in] gif       The animation context
 * \param[in] frame_idx The frame number to decode.
 * \return error code
 *         - GIF_INSUFFICIENT_DATA for insufficient data to do anything
 *         - GIF_FRAME_DATA_ERROR for GIF frame data error
 *         - GIF_INSUFFICIENT_MEMORY for insufficient memory to process
 *         - GIF_INSUFFICIENT_FRAME_DATA for insufficient data to complete the frame
 *         - GIF_DATA_ERROR for GIF error (invalid frame header)
 *         - GIF_OK for successful decoding
 *         - GIF_WORKING for successful decoding if more frames are expected
*/
static gif_result gif_initialise_frame(
		struct gif_animation *gif,
		uint32_t frame_idx)
{
	gif_result ret;
	struct gif_frame *frame;
	uint8_t *gif_data, *gif_end;
	int gif_bytes;

	/* Get our buffer position etc. */
	gif_data = (uint8_t *)(gif->gif_data + gif->buffer_position);
	gif_end = (uint8_t *)(gif->gif_data + gif->buffer_size);
	gif_bytes = (gif_end - gif_data);

	/* Check if we've finished */
	if ((gif_bytes > 0) && (gif_data[0] == GIF_TRAILER)) {
		return GIF_OK;
	}

	/* We could theoretically get some junk data that gives us millions of
	 * frames, so we ensure that we don't have a silly number
	 */
	if (frame_idx > 4096) {
		return GIF_FRAME_DATA_ERROR;
	}

	frame = gif__get_frame(gif, frame_idx);
	if (frame == NULL) {
		return GIF_INSUFFICIENT_MEMORY;
	}

	/* Initialise any extensions */
	gif->buffer_position = gif_data - gif->gif_data;
	ret = gif__parse_frame_extensions(gif, &gif->frames[frame_idx], true);
	if (ret != GIF_OK) {
		return ret;
	}

	ret = gif__parse_image_descriptor(gif, &gif->frames[frame_idx], true);
	if (ret != GIF_OK) {
		return ret;
	}

	ret = gif__parse_colour_table(gif, &gif->frames[frame_idx], false);
	if (ret != GIF_OK) {
		return ret;
	}

	ret = gif__parse_image_data(gif, &gif->frames[frame_idx], false);
	if (ret != GIF_OK) {
		return ret;
	}

	return GIF_OK;
}

/**
 * decode a gif frame
 *
 * \param gif gif animation context.
 * \param frame The frame number to decode.
 * \param clear_image flag for image data being cleared instead of plotted.
 */
static gif_result gif_internal_decode_frame(
		struct gif_animation *gif,
		uint32_t frame_idx)
{
	gif_result ret;
	uint8_t *gif_data;
	struct gif_frame *frame;
	uint32_t save_buffer_position;

	/* Ensure the frame is in range to decode */
	if (frame_idx > gif->frame_count_partial) {
		return GIF_INSUFFICIENT_DATA;
	}

	/* Done if frame is already decoded */
	if (((int)frame_idx == gif->decoded_frame)) {
		return GIF_OK;
	}

	frame = gif__get_frame(gif, frame_idx);
	if (frame == NULL) {
		return GIF_INSUFFICIENT_MEMORY;
	}

	/* Ensure this frame is supposed to be decoded */
	if (frame->display == false) {
		return GIF_OK;
	}

	/* Get the start of our frame data and the end of the GIF data */
	gif_data = gif->gif_data + frame->frame_pointer;

	/* Save the buffer position */
	save_buffer_position = gif->buffer_position;
	gif->buffer_position = gif_data - gif->gif_data;

	/* Skip any extensions because they have already been processed */
	ret = gif__parse_frame_extensions(gif, frame, false);
	if (ret != GIF_OK) {
		goto gif_decode_frame_exit;
	}

	ret = gif__parse_image_descriptor(gif, frame, false);
	if (ret != GIF_OK) {
		goto gif_decode_frame_exit;
	}

	ret = gif__parse_colour_table(gif, frame, true);
	if (ret != GIF_OK) {
		goto gif_decode_frame_exit;
	}

	ret = gif__parse_image_data(gif, frame, true);
	if (ret != GIF_OK) {
		goto gif_decode_frame_exit;
	}

gif_decode_frame_exit:

	/* Restore the buffer position */
	gif->buffer_position = save_buffer_position;

	return ret;
}


/* exported function documented in libnsgif.h */
void gif_create(gif_animation *gif, gif_bitmap_callback_vt *bitmap_callbacks)
{
	memset(gif, 0, sizeof(gif_animation));
	gif->bitmap_callbacks = *bitmap_callbacks;
	gif->decoded_frame = GIF_INVALID_FRAME;
	gif->prev_index = GIF_INVALID_FRAME;
}


/* exported function documented in libnsgif.h */
gif_result gif_initialise(gif_animation *gif, size_t size, unsigned char *data)
{
	uint8_t *gif_data;
	uint32_t index;
	gif_result ret;

	/* Initialize values */
	gif->buffer_size = size;
	gif->gif_data = data;

	if (gif->lzw_ctx == NULL) {
		lzw_result res = lzw_context_create(
				(struct lzw_ctx **)&gif->lzw_ctx);
		if (res != LZW_OK) {
			return gif_error_from_lzw(res);
		}
	}

	/* Check for sufficient data to be a GIF (6-byte header + 7-byte
	 * logical screen descriptor)
	 */
	if (gif->buffer_size < GIF_STANDARD_HEADER_SIZE) {
		return GIF_INSUFFICIENT_DATA;
	}

	/* Get our current processing position */
	gif_data = gif->gif_data + gif->buffer_position;

	/* See if we should initialise the GIF */
	if (gif->buffer_position == 0) {
		/* We want everything to be NULL before we start so we've no
		 * chance of freeing bad pointers (paranoia)
		 */
		gif->frame_image = NULL;
		gif->frames = NULL;
		gif->frame_holders = 0;
		gif->local_colour_table = NULL;
		gif->global_colour_table = NULL;

		/* The caller may have been lazy and not reset any values */
		gif->frame_count = 0;
		gif->frame_count_partial = 0;
		gif->decoded_frame = GIF_INVALID_FRAME;

		/* 6-byte GIF file header is:
		 *
		 *  +0   3CHARS   Signature ('GIF')
		 *  +3   3CHARS   Version ('87a' or '89a')
		 */
		if (strncmp((const char *) gif_data, "GIF", 3) != 0) {
			return GIF_DATA_ERROR;
		}
		gif_data += 3;

		/* Ensure GIF reports version 87a or 89a */
		/*
		if ((strncmp(gif_data, "87a", 3) != 0) &&
		    (strncmp(gif_data, "89a", 3) != 0))
			       LOG(("Unknown GIF format - proceeding anyway"));
		*/
		gif_data += 3;

		/* 7-byte Logical Screen Descriptor is:
		 *
		 *  +0   SHORT   Logical Screen Width
		 *  +2   SHORT   Logical Screen Height
		 *  +4   CHAR    __Packed Fields__
		 *               1BIT    Global Colour Table Flag
		 *               3BITS   Colour Resolution
		 *               1BIT    Sort Flag
		 *               3BITS   Size of Global Colour Table
		 *  +5   CHAR    Background Colour Index
		 *  +6   CHAR    Pixel Aspect Ratio
		 */
		gif->width = gif_data[0] | (gif_data[1] << 8);
		gif->height = gif_data[2] | (gif_data[3] << 8);
		gif->global_colours = (gif_data[4] & GIF_COLOUR_TABLE_MASK);
		gif->colour_table_size = (2 << (gif_data[4] & GIF_COLOUR_TABLE_SIZE_MASK));
		gif->bg_index = gif_data[5];
		gif->aspect_ratio = gif_data[6];
		gif->loop_count = 1;
		gif_data += 7;

		/* Some broken GIFs report the size as the screen size they
		 * were created in. As such, we detect for the common cases and
		 * set the sizes as 0 if they are found which results in the
		 * GIF being the maximum size of the frames.
		 */
		if (((gif->width == 640) && (gif->height == 480)) ||
		    ((gif->width == 640) && (gif->height == 512)) ||
		    ((gif->width == 800) && (gif->height == 600)) ||
		    ((gif->width == 1024) && (gif->height == 768)) ||
		    ((gif->width == 1280) && (gif->height == 1024)) ||
		    ((gif->width == 1600) && (gif->height == 1200)) ||
		    ((gif->width == 0) || (gif->height == 0)) ||
		    ((gif->width > 2048) || (gif->height > 2048))) {
			gif->width = 1;
			gif->height = 1;
		}

		/* Allocate some data irrespective of whether we've got any
		 * colour tables. We always get the maximum size in case a GIF
		 * is lying to us. It's far better to give the wrong colours
		 * than to trample over some memory somewhere.
		*/
		gif->global_colour_table = calloc(GIF_MAX_COLOURS, sizeof(uint32_t));
		gif->local_colour_table = calloc(GIF_MAX_COLOURS, sizeof(uint32_t));
		if ((gif->global_colour_table == NULL) ||
		    (gif->local_colour_table == NULL)) {
			gif_finalise(gif);
			return GIF_INSUFFICIENT_MEMORY;
		}

		/* Set the first colour to a value that will never occur in
		 * reality so we know if we've processed it
		*/
		gif->global_colour_table[0] = GIF_PROCESS_COLOURS;

		/* Check if the GIF has no frame data (13-byte header + 1-byte
		 * termination block) Although generally useless, the GIF
		 * specification does not expressly prohibit this
		 */
		if (gif->buffer_size == (GIF_STANDARD_HEADER_SIZE + 1)) {
			if (gif_data[0] == GIF_TRAILER) {
				return GIF_OK;
			} else {
				return GIF_INSUFFICIENT_DATA;
			}
		}

		/* Remember we've done this now */
		gif->buffer_position = gif_data - gif->gif_data;
	}

	/*  Do the colour map if we haven't already. As the top byte is always
	 *  0xff or 0x00 depending on the transparency we know if it's been
	 *  filled in.
	 */
	if (gif->global_colour_table[0] == GIF_PROCESS_COLOURS) {
		/* Check for a global colour map signified by bit 7 */
		if (gif->global_colours) {
			if (gif->buffer_size < (gif->colour_table_size * 3 + GIF_STANDARD_HEADER_SIZE)) {
				return GIF_INSUFFICIENT_DATA;
			}
			for (index = 0; index < gif->colour_table_size; index++) {
				/* Gif colour map contents are r,g,b.
				 *
				 * We want to pack them bytewise into the
				 * colour table, such that the red component
				 * is in byte 0 and the alpha component is in
				 * byte 3.
				 */
				uint8_t *entry = (uint8_t *) &gif->
						       global_colour_table[index];

				entry[0] = gif_data[0]; /* r */
				entry[1] = gif_data[1]; /* g */
				entry[2] = gif_data[2]; /* b */
				entry[3] = 0xff;        /* a */

				gif_data += 3;
			}
			gif->buffer_position = (gif_data - gif->gif_data);
		} else {
			/* Create a default colour table with the first two
			 * colours as black and white
			 */
			uint32_t *entry = gif->global_colour_table;

			entry[0] = 0x00000000;
			/* Force Alpha channel to opaque */
			((uint8_t *) entry)[3] = 0xff;

			entry[1] = 0xffffffff;
		}

		if (gif->global_colours &&
		    gif->bg_index < gif->colour_table_size) {
			size_t bg_idx = gif->bg_index;
			gif->bg_colour = gif->global_colour_table[bg_idx];
		} else {
			gif->bg_colour = gif->global_colour_table[0];
		}
	}

	/* Repeatedly try to initialise frames */
	while ((ret = gif_initialise_frame(gif, gif->frame_count)) == GIF_WORKING);

	/* If there was a memory error tell the caller */
	if ((ret == GIF_INSUFFICIENT_MEMORY) ||
	    (ret == GIF_DATA_ERROR)) {
		return ret;
	}

	/* If we didn't have some frames then a GIF_INSUFFICIENT_DATA becomes a
	 * GIF_INSUFFICIENT_FRAME_DATA
	 */
	if (ret == GIF_INSUFFICIENT_DATA && gif->frame_count_partial > 0) {
		return GIF_INSUFFICIENT_FRAME_DATA;
	}

	/* Return how many we got */
	return ret;
}


/* exported function documented in libnsgif.h */
gif_result gif_decode_frame(gif_animation *gif, unsigned int frame)
{
	return gif_internal_decode_frame(gif, frame);
}


/* exported function documented in libnsgif.h */
void gif_finalise(gif_animation *gif)
{
	/* Release all our memory blocks */
	if (gif->frame_image) {
		assert(gif->bitmap_callbacks.bitmap_destroy);
		gif->bitmap_callbacks.bitmap_destroy(gif->frame_image);
	}

	gif->frame_image = NULL;
	free(gif->frames);
	gif->frames = NULL;
	free(gif->local_colour_table);
	gif->local_colour_table = NULL;
	free(gif->global_colour_table);
	gif->global_colour_table = NULL;

	free(gif->prev_frame);
	gif->prev_frame = NULL;

	lzw_context_destroy(gif->lzw_ctx);
	gif->lzw_ctx = NULL;
}
