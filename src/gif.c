/*
 * Copyright 2004 Richard Wilson <richard.wilson@netsurf-browser.org>
 * Copyright 2008 Sean Fox <dyntryx@gmail.com>
 * Copyright 2013-2021 Michael Drake <tlsa@netsurf-browser.org>
 *
 * This file is part of NetSurf's libnsgif, http://www.netsurf-browser.org/
 * Licenced under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "lzw.h"
#include "nsgif.h"

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
#define NSGIF_MAX_COLOURS 256

/** Internal flag that the colour table needs to be processed */
#define NSGIF_PROCESS_COLOURS 0xaa000000

/** Internal flag that a frame is invalid/unprocessed */
#define NSGIF_INVALID_FRAME -1

/** Transparent colour */
#define NSGIF_TRANSPARENT_COLOUR 0x00

/** No transparency */
#define NSGIF_NO_TRANSPARENCY (0xFFFFFFFFu)

enum nsgif_disposal {
	NSGIF_DISPOSAL_UNSPECIFIED,
	NSGIF_DISPOSAL_NONE,
	NSGIF_DISPOSAL_RESTORE_BG,
	NSGIF_DISPOSAL_RESTORE_PREV,
	NSGIF_DISPOSAL_RESTORE_QUIRK, /**< Alias for NSGIF_DISPOSAL_RESTORE_PREV. */
};

/* GIF Flags */
#define NSGIF_COLOUR_TABLE_MASK 0x80
#define NSGIF_COLOUR_TABLE_SIZE_MASK 0x07
#define NSGIF_BLOCK_TERMINATOR 0x00
#define NSGIF_TRAILER 0x3b

/**
 * Convert an LZW result code to equivalent GIF result code.
 *
 * \param[in]  l_res  LZW response code.
 * \return GIF result code.
 */
static nsgif_result nsgif__error_from_lzw(lzw_result l_res)
{
	static const nsgif_result g_res[] = {
		[LZW_OK]        = NSGIF_OK,
		[LZW_OK_EOD]    = NSGIF_END_OF_FRAME,
		[LZW_NO_MEM]    = NSGIF_INSUFFICIENT_MEMORY,
		[LZW_NO_DATA]   = NSGIF_INSUFFICIENT_DATA,
		[LZW_EOI_CODE]  = NSGIF_FRAME_DATA_ERROR,
		[LZW_BAD_ICODE] = NSGIF_FRAME_DATA_ERROR,
		[LZW_BAD_CODE]  = NSGIF_FRAME_DATA_ERROR,
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
 * \return NSGIF_INSUFFICIENT_MEMORY for a memory error NSGIF_OK for success
 */
static nsgif_result nsgif__initialise_sprite(
		struct nsgif_animation *gif,
		uint32_t width,
		uint32_t height)
{
	/* Already allocated? */
	if (gif->frame_image) {
		return NSGIF_OK;
	}

	assert(gif->bitmap.create);
	gif->frame_image = gif->bitmap.create(width, height);
	if (gif->frame_image == NULL) {
		return NSGIF_INSUFFICIENT_MEMORY;
	}

	return NSGIF_OK;
}

/**
 * Helper to get the rendering bitmap for a gif.
 *
 * \param[in]  gif  The gif object we're decoding.
 * \return Client pixel buffer for rendering into.
 */
static inline uint32_t* nsgif__bitmap_get(
		struct nsgif_animation *gif)
{
	nsgif_result ret;

	/* Make sure we have a buffer to decode to. */
	ret = nsgif__initialise_sprite(gif, gif->width, gif->height);
	if (ret != NSGIF_OK) {
		return NULL;
	}

	/* Get the frame data */
	assert(gif->bitmap.get_buffer);
	return (uint32_t *)gif->bitmap.get_buffer(gif->frame_image);
}

/**
 * Helper to tell the client that their bitmap was modified.
 *
 * \param[in]  gif  The gif object we're decoding.
 */
static inline void nsgif__bitmap_modified(
		const struct nsgif_animation *gif)
{
	if (gif->bitmap.modified) {
		gif->bitmap.modified(gif->frame_image);
	}
}

/**
 * Helper to tell the client that whether the bitmap is opaque.
 *
 * \param[in]  gif    The gif object we're decoding.
 * \param[in]  frame  The frame that has been decoded.
 */
static inline void nsgif__bitmap_set_opaque(
		const struct nsgif_animation *gif,
		const struct nsgif_frame *frame)
{
	if (gif->bitmap.set_opaque) {
		gif->bitmap.set_opaque(
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
static inline bool nsgif__bitmap_get_opaque(
		const struct nsgif_animation *gif)
{
	if (gif->bitmap.test_opaque) {
		return gif->bitmap.test_opaque(
				gif->frame_image);
	}

	return false;
}

static void nsgif__record_frame(
		struct nsgif_animation *gif,
		const uint32_t *bitmap)
{
	bool need_alloc = gif->prev_frame == NULL;
	uint32_t *prev_frame;

	if (gif->decoded_frame == NSGIF_INVALID_FRAME ||
	    gif->decoded_frame == gif->prev_index) {
		/* No frame to copy, or already have this frame recorded. */
		return;
	}

	bitmap = nsgif__bitmap_get(gif);
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

static nsgif_result nsgif__recover_frame(
		const struct nsgif_animation *gif,
		uint32_t *bitmap)
{
	const uint32_t *prev_frame = gif->prev_frame;
	unsigned height = gif->height < gif->prev_height ? gif->height : gif->prev_height;
	unsigned width  = gif->width  < gif->prev_width  ? gif->width  : gif->prev_width;

	if (prev_frame == NULL) {
		return NSGIF_FRAME_DATA_ERROR;
	}

	for (unsigned y = 0; y < height; y++) {
		memcpy(bitmap, prev_frame, width * 4);

		bitmap += gif->width;
		prev_frame += gif->prev_width;
	}

	return NSGIF_OK;
}

/**
 * Get the next line for GIF decode.
 *
 * Note that the step size must be initialised to 24 at the start of the frame
 * (when y == 0).  This is because of the first two passes of the frame have
 * the same step size of 8, and the step size is used to determine the current
 * pass.
 *
 * \param[in]     height     Frame height in pixels.
 * \param[in,out] y          Current row, starting from 0, updated on exit.
 * \param[in,out] step       Current step starting with 24, updated on exit.
 * \return true if there is a row to process, false at the end of the frame.
 */
static inline bool nsgif__deinterlace(uint32_t height, uint32_t *y, uint8_t *step)
{
	*y += *step & 0xf;

	if (*y < height) return true;

	switch (*step) {
	case 24: *y = 4; *step = 8; if (*y < height) return true;
	         /* Fall through. */
	case  8: *y = 2; *step = 4; if (*y < height) return true;
	         /* Fall through. */
	case  4: *y = 1; *step = 2; if (*y < height) return true;
	         /* Fall through. */
	default:
		break;
	}

	return false;
}

/**
 * Get the next line for GIF decode.
 *
 * \param[in]     interlace  Non-zero if the frame is not interlaced.
 * \param[in]     height     Frame height in pixels.
 * \param[in,out] y          Current row, starting from 0, updated on exit.
 * \param[in,out] step       Current step starting with 24, updated on exit.
 * \return true if there is a row to process, false at the end of the frame.
 */
static inline bool nsgif__next_row(uint32_t interlace,
		uint32_t height, uint32_t *y, uint8_t *step)
{
	if (!interlace) {
		return (++*y != height);
	} else {
		return nsgif__deinterlace(height, y, step);
	}
}

/**
 * Get any frame clip adjustment for the image extent.
 *
 * \param[in]  frame_off  Frame's X or Y offset.
 * \param[in]  frame_dim  Frame width or height.
 * \param[in]  image_ext  Image width or height constraint.
 * \return the amount the frame needs to be clipped to fit the image in given
 *         dimension.
 */
static inline uint32_t gif__clip(
		uint32_t frame_off,
		uint32_t frame_dim,
		uint32_t image_ext)
{
	uint32_t frame_ext = frame_off + frame_dim;

	if (frame_ext <= image_ext) {
		return 0;
	}

	return frame_ext - image_ext;
}

/**
 * Perform any jump over decoded data, to accommodate clipped portion of frame.
 *
 * \param[in,out] skip       Number of pixels of data to jump.
 * \param[in,out] available  Number of pixels of data currently available.
 * \param[in,out] pos        Position in decoded pixel value data.
 */
static inline void gif__jump_data(
		uint32_t *skip,
		uint32_t *available,
		const uint8_t **pos)
{
	uint32_t jump = (*skip < *available) ? *skip : *available;

	*skip -= jump;
	*available -= jump;
	*pos += jump;
}

static nsgif_result nsgif__decode_complex(
		struct nsgif_animation *gif,
		uint32_t width,
		uint32_t height,
		uint32_t offset_x,
		uint32_t offset_y,
		uint32_t interlace,
		const uint8_t *data,
		uint32_t transparency_index,
		uint32_t *restrict frame_data,
		uint32_t *restrict colour_table)
{
	lzw_result res;
	nsgif_result ret = NSGIF_OK;
	uint32_t clip_x = gif__clip(offset_x, width, gif->width);
	uint32_t clip_y = gif__clip(offset_y, height, gif->height);
	const uint8_t *uncompressed;
	uint32_t available = 0;
	uint8_t step = 24;
	uint32_t skip = 0;
	uint32_t y = 0;

	if (offset_x >= gif->width ||
	    offset_y >= gif->height) {
		return NSGIF_OK;
	}

	width -= clip_x;
	height -= clip_y;

	if (width == 0 || height == 0) {
		return NSGIF_OK;
	}

	/* Initialise the LZW decoding */
	res = lzw_decode_init(gif->lzw_ctx, data[0],
			gif->nsgif_data, gif->buffer_size,
			data + 1 - gif->nsgif_data);
	if (res != LZW_OK) {
		return nsgif__error_from_lzw(res);
	}

	do {
		uint32_t x;
		uint32_t *frame_scanline;

		frame_scanline = frame_data + offset_x +
				(y + offset_y) * gif->width;

		x = width;
		while (x > 0) {
			unsigned row_available;
			while (available == 0) {
				if (res != LZW_OK) {
					/* Unexpected end of frame, try to recover */
					if (res == LZW_OK_EOD) {
						ret = NSGIF_OK;
					} else {
						ret = nsgif__error_from_lzw(res);
					}
					return ret;
				}
				res = lzw_decode(gif->lzw_ctx,
						&uncompressed, &available);

				if (available == 0) {
					return NSGIF_OK;
				}
				gif__jump_data(&skip, &available, &uncompressed);
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

		skip = clip_x;
		gif__jump_data(&skip, &available, &uncompressed);
	} while (nsgif__next_row(interlace, height, &y, &step));

	return ret;
}

static nsgif_result nsgif__decode_simple(
		struct nsgif_animation *gif,
		uint32_t height,
		uint32_t offset_y,
		const uint8_t *data,
		uint32_t transparency_index,
		uint32_t *restrict frame_data,
		uint32_t *restrict colour_table)
{
	uint32_t pixels = gif->width * height;
	uint32_t written = 0;
	nsgif_result ret = NSGIF_OK;
	lzw_result res;

	if (offset_y >= gif->height) {
		return NSGIF_OK;
	}

	height -= gif__clip(offset_y, height, gif->height);

	if (height == 0) {
		return NSGIF_OK;
	}

	/* Initialise the LZW decoding */
	res = lzw_decode_init_map(gif->lzw_ctx, data[0],
			transparency_index, colour_table,
			gif->nsgif_data, gif->buffer_size,
			data + 1 - gif->nsgif_data);
	if (res != LZW_OK) {
		return nsgif__error_from_lzw(res);
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
				ret = NSGIF_OK;
			} else {
				ret = nsgif__error_from_lzw(res);
			}
			break;
		}
	}

	if (pixels == 0) {
		ret = NSGIF_OK;
	}

	return ret;
}

static inline nsgif_result nsgif__decode(
		struct nsgif_animation *gif,
		struct nsgif_frame *frame,
		const uint8_t *data,
		uint32_t *restrict frame_data)
{
	enum {
		GIF_MASK_INTERLACE = 0x40,
	};

	nsgif_result ret;
	uint32_t width = frame->redraw.w;
	uint32_t height = frame->redraw.h;
	uint32_t offset_x = frame->redraw.x;
	uint32_t offset_y = frame->redraw.y;
	uint32_t interlace = frame->flags & GIF_MASK_INTERLACE;
	uint32_t transparency_index = frame->transparency_index;
	uint32_t *restrict colour_table = gif->colour_table;

	if (interlace == false && width == gif->width && offset_x == 0) {
		ret = nsgif__decode_simple(gif, height, offset_y,
				data, transparency_index,
				frame_data, colour_table);
	} else {
		ret = nsgif__decode_complex(gif, width, height,
				offset_x, offset_y, interlace,
				data, transparency_index,
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
static void nsgif__restore_bg(
		struct nsgif_animation *gif,
		struct nsgif_frame *frame,
		uint32_t *bitmap)
{
	if (frame == NULL) {
		memset(bitmap, NSGIF_TRANSPARENT_COLOUR,
				gif->width * gif->height * sizeof(*bitmap));
	} else {
		uint32_t width = frame->redraw.w;
		uint32_t height = frame->redraw.h;
		uint32_t offset_x = frame->redraw.x;
		uint32_t offset_y = frame->redraw.y;

		width -= gif__clip(offset_x, width, gif->width);
		height -= gif__clip(offset_y, height, gif->height);

		if (frame->display == false || width == 0) {
			return;
		}

		if (frame->transparency) {
			for (uint32_t y = 0; y < height; y++) {
				uint32_t *scanline = bitmap + offset_x +
						(offset_y + y) * gif->width;
				memset(scanline, NSGIF_TRANSPARENT_COLOUR,
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

static nsgif_result nsgif__update_bitmap(
		struct nsgif_animation *gif,
		struct nsgif_frame *frame,
		const uint8_t *data,
		uint32_t frame_idx)
{
	nsgif_result ret;
	uint32_t *bitmap;

	gif->decoded_frame = frame_idx;

	bitmap = nsgif__bitmap_get(gif);
	if (bitmap == NULL) {
		return NSGIF_INSUFFICIENT_MEMORY;
	}

	/* Handle any bitmap clearing/restoration required before decoding this
	 * frame. */
	if (frame_idx == 0 || gif->decoded_frame == NSGIF_INVALID_FRAME) {
		nsgif__restore_bg(gif, NULL, bitmap);

	} else {
		struct nsgif_frame *prev = &gif->frames[frame_idx - 1];

		if (prev->disposal_method == NSGIF_DISPOSAL_RESTORE_BG) {
			nsgif__restore_bg(gif, prev, bitmap);

		} else if (prev->disposal_method == NSGIF_DISPOSAL_RESTORE_PREV) {
			ret = nsgif__recover_frame(gif, bitmap);
			if (ret != NSGIF_OK) {
				nsgif__restore_bg(gif, prev, bitmap);
			}
		}
	}

	if (frame->disposal_method == NSGIF_DISPOSAL_RESTORE_PREV) {
		/* Store the previous frame for later restoration */
		nsgif__record_frame(gif, bitmap);
	}

	ret = nsgif__decode(gif, frame, data, bitmap);

	nsgif__bitmap_modified(gif);

	if (!frame->decoded) {
		frame->opaque = nsgif__bitmap_get_opaque(gif);
		frame->decoded = true;
	}
	nsgif__bitmap_set_opaque(gif, frame);

	return ret;
}

/**
 * Parse the graphic control extension
 *
 * \param[in] frame  The gif object we're decoding.
 * \param[in] data   The data to decode.
 * \param[in] len    Byte length of data.
 * \return NSGIF_INSUFFICIENT_DATA if more data is needed,
 *         NSGIF_OK for success.
 */
static nsgif_result nsgif__parse_extension_graphic_control(
		struct nsgif_frame *frame,
		const uint8_t *data,
		size_t len)
{
	enum {
		GIF_MASK_TRANSPARENCY = 0x01,
		GIF_MASK_DISPOSAL     = 0x1c,
	};

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
		return NSGIF_INSUFFICIENT_DATA;
	}

	frame->frame_delay = data[3] | (data[4] << 8);
	if (data[2] & GIF_MASK_TRANSPARENCY) {
		frame->transparency = true;
		frame->transparency_index = data[5];
	}

	frame->disposal_method = ((data[2] & GIF_MASK_DISPOSAL) >> 2);
	/* I have encountered documentation and GIFs in the
	 * wild that use 0x04 to restore the previous frame,
	 * rather than the officially documented 0x03.  I
	 * believe some (older?)  software may even actually
	 * export this way.  We handle this as a type of
	 * "quirks" mode. */
	if (frame->disposal_method == NSGIF_DISPOSAL_RESTORE_QUIRK) {
		frame->disposal_method = NSGIF_DISPOSAL_RESTORE_PREV;
	}

	/* if we are clearing the background then we need to
	 * redraw enough to cover the previous frame too. */
	frame->redraw_required =
			frame->disposal_method == NSGIF_DISPOSAL_RESTORE_BG ||
			frame->disposal_method == NSGIF_DISPOSAL_RESTORE_PREV;

	return NSGIF_OK;
}

/**
 * Parse the application extension
 *
 * \param[in] gif   The gif object we're decoding.
 * \param[in] data  The data to decode.
 * \param[in] len   Byte length of data.
 * \return NSGIF_INSUFFICIENT_DATA if more data is needed,
 *         NSGIF_OK for success.
 */
static nsgif_result nsgif__parse_extension_application(
		struct nsgif_animation *gif,
		const uint8_t *data,
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
		return NSGIF_INSUFFICIENT_DATA;
	}

	if ((data[1] == 0x0b) &&
	    (strncmp((const char *)data + 2, "NETSCAPE2.0", 11) == 0) &&
	    (data[13] == 0x03) && (data[14] == 0x01)) {
		gif->loop_count = data[15] | (data[16] << 8);
	}

	return NSGIF_OK;
}

/**
 * Parse the frame's extensions
 *
 * \param[in] gif     The gif object we're decoding.
 * \param[in] frame   The frame to parse extensions for.
 * \param[in] pos     Current position in data, updated on exit.
 * \param[in] decode  Whether to decode or skip over the extension.
 * \return NSGIF_INSUFFICIENT_DATA if more data is needed,
 *         NSGIF_OK for success.
 */
static nsgif_result nsgif__parse_frame_extensions(
		struct nsgif_animation *gif,
		struct nsgif_frame *frame,
		const uint8_t **pos,
		bool decode)
{
	enum {
		GIF_EXT_INTRODUCER      = 0x21,
		GIF_EXT_GRAPHIC_CONTROL = 0xf9,
		GIF_EXT_COMMENT         = 0xfe,
		GIF_EXT_PLAIN_TEXT      = 0x01,
		GIF_EXT_APPLICATION     = 0xff,
	};
	const uint8_t *nsgif_data = *pos;
	const uint8_t *nsgif_end = gif->nsgif_data + gif->buffer_size;
	int nsgif_bytes = nsgif_end - nsgif_data;

	/* Initialise the extensions */
	while (nsgif_bytes > 0 && nsgif_data[0] == GIF_EXT_INTRODUCER) {
		bool block_step = true;
		nsgif_result ret;

		nsgif_data++;
		nsgif_bytes--;

		if (nsgif_bytes == 0) {
			return NSGIF_INSUFFICIENT_DATA;
		}

		/* Switch on extension label */
		switch (nsgif_data[0]) {
		case GIF_EXT_GRAPHIC_CONTROL:
			if (decode) {
				ret = nsgif__parse_extension_graphic_control(
						frame, nsgif_data, nsgif_bytes);
				if (ret != NSGIF_OK) {
					return ret;
				}
			}
			break;

		case GIF_EXT_APPLICATION:
			if (decode) {
				ret = nsgif__parse_extension_application(
						gif, nsgif_data, nsgif_bytes);
				if (ret != NSGIF_OK) {
					return ret;
				}
			}
			break;

		case GIF_EXT_COMMENT:
			/* Move the pointer to the first data sub-block Skip 1
			 * byte for the extension label. */
			++nsgif_data;
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
			if (nsgif_bytes < 2) {
				return NSGIF_INSUFFICIENT_DATA;
			}
			nsgif_data += 2 + nsgif_data[1];
		}

		/* Repeatedly skip blocks until we get a zero block or run out
		 * of data.  This data is ignored by this gif decoder. */
		while (nsgif_data < nsgif_end && nsgif_data[0] != NSGIF_BLOCK_TERMINATOR) {
			nsgif_data += nsgif_data[0] + 1;
			if (nsgif_data >= nsgif_end) {
				return NSGIF_INSUFFICIENT_DATA;
			}
		}
		nsgif_data++;
		nsgif_bytes = nsgif_end - nsgif_data;
	}

	if (nsgif_data > nsgif_end) {
		nsgif_data = nsgif_end;
	}

	/* Set buffer position and return */
	*pos = nsgif_data;
	return NSGIF_OK;
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
 * \param[in] gif     The gif object we're decoding.
 * \param[in] frame   The frame to parse an image descriptor for.
 * \param[in] pos     Current position in data, updated on exit.
 * \param[in] decode  Whether to decode the image descriptor.
 * \return NSGIF_OK on success, appropriate error otherwise.
 */
static nsgif_result nsgif__parse_image_descriptor(
		struct nsgif_animation *gif,
		struct nsgif_frame *frame,
		const uint8_t **pos,
		bool decode)
{
	const uint8_t *data = *pos;
	size_t len = gif->nsgif_data + gif->buffer_size - data;
	enum {
		NSGIF_IMAGE_DESCRIPTOR_LEN = 10u,
		NSGIF_IMAGE_SEPARATOR      = 0x2Cu,
	};

	assert(gif != NULL);
	assert(frame != NULL);

	if (len < NSGIF_IMAGE_DESCRIPTOR_LEN) {
		return NSGIF_INSUFFICIENT_DATA;
	}

	if (decode) {
		unsigned x, y, w, h;

		if (data[0] != NSGIF_IMAGE_SEPARATOR) {
			return NSGIF_FRAME_DATA_ERROR;
		}

		x = data[1] | (data[2] << 8);
		y = data[3] | (data[4] << 8);
		w = data[5] | (data[6] << 8);
		h = data[7] | (data[8] << 8);
		frame->flags = data[9];

		frame->redraw.x = x;
		frame->redraw.y = y;
		frame->redraw.w = w;
		frame->redraw.h = h;

		/* Allow first frame to grow image dimensions. */
		if (gif->frame_count == 0) {
			if (x + w > gif->width) {
				gif->width = x + w;
			}
			if (y + h > gif->height) {
				gif->height = y + h;
			}
		}
	}

	*pos += NSGIF_IMAGE_DESCRIPTOR_LEN;
	return NSGIF_OK;
}

/**
 * Extract a GIF colour table into a LibNSGIF colour table buffer.
 *
 * \param[in] gif                   The gif object we're decoding.
 * \param[in] colour_table          The colour table to populate.
 * \param[in] colour_table_entries  The number of colour table entries.
 * \param[in] pos                   Current position in data, updated on exit.
 * \param[in] decode                Whether to decode the colour table.
 * \return NSGIF_OK on success, appropriate error otherwise.
 */
static nsgif_result nsgif__colour_table_extract(
		struct nsgif_animation *gif,
		uint32_t *colour_table,
		size_t colour_table_entries,
		const uint8_t **pos,
		bool decode)
{
	const uint8_t *data = *pos;
	size_t len = gif->nsgif_data + gif->buffer_size - data;

	if (len < colour_table_entries * 3) {
		return NSGIF_INSUFFICIENT_DATA;
	}

	if (decode) {
		int count = colour_table_entries;
		uint8_t *entry = (uint8_t *)colour_table;

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

	*pos += colour_table_entries * 3;
	return NSGIF_OK;
}

/**
 * Get a frame's colour table.
 *
 * Sets up gif->colour_table for the frame.
 *
 * \param[in] gif     The gif object we're decoding.
 * \param[in] frame   The frame to get the colour table for.
 * \param[in] pos     Current position in data, updated on exit.
 * \param[in] decode  Whether to decode the colour table.
 * \return NSGIF_OK on success, appropriate error otherwise.
 */
static nsgif_result nsgif__parse_colour_table(
		struct nsgif_animation *gif,
		struct nsgif_frame *frame,
		const uint8_t **pos,
		bool decode)
{
	nsgif_result ret;

	assert(gif != NULL);
	assert(frame != NULL);

	if ((frame->flags & NSGIF_COLOUR_TABLE_MASK) == 0) {
		gif->colour_table = gif->global_colour_table;
		return NSGIF_OK;
	}

	ret = nsgif__colour_table_extract(gif, gif->local_colour_table,
			2 << (frame->flags & NSGIF_COLOUR_TABLE_SIZE_MASK),
			pos, decode);
	if (ret != NSGIF_OK) {
		return ret;
	}

	gif->colour_table = gif->local_colour_table;
	return NSGIF_OK;
}

/**
 * Parse the image data for a gif frame.
 *
 * Sets up gif->colour_table for the frame.
 *
 * \param[in] gif     The gif object we're decoding.
 * \param[in] frame   The frame to parse image data for.
 * \param[in] pos     Current position in data, updated on exit.
 * \param[in] decode  Whether to decode the image data.
 * \return NSGIF_OK on success, appropriate error otherwise.
 */
static nsgif_result nsgif__parse_image_data(
		struct nsgif_animation *gif,
		struct nsgif_frame *frame,
		const uint8_t **pos,
		bool decode)
{
	const uint8_t *data = *pos;
	size_t len = gif->nsgif_data + gif->buffer_size - data;
	uint32_t frame_idx = frame - gif->frames;
	uint8_t minimum_code_size;
	nsgif_result ret;

	assert(gif != NULL);
	assert(frame != NULL);

	if (!decode) {
		gif->frame_count_partial = frame_idx + 1;
	}

	/* Ensure sufficient data remains.  A gif trailer or a minimum lzw code
	 * followed by a gif trailer is treated as OK, although without any
	 * image data. */
	switch (len) {
		default: if (data[0] == NSGIF_TRAILER) return NSGIF_OK;
			break;
		case 2: if (data[1] == NSGIF_TRAILER) return NSGIF_OK;
			/* Fall through. */
		case 1: if (data[0] == NSGIF_TRAILER) return NSGIF_OK;
			/* Fall through. */
		case 0: return NSGIF_INSUFFICIENT_DATA;
	}

	minimum_code_size = data[0];
	if (minimum_code_size >= LZW_CODE_MAX) {
		return NSGIF_DATA_ERROR;
	}

	if (decode) {
		ret = nsgif__update_bitmap(gif, frame, data, frame_idx);
	} else {
		uint32_t block_size = 0;

		/* Skip the minimum code size. */
		data++;
		len--;

		while (block_size != 1) {
			if (len < 1) return NSGIF_INSUFFICIENT_DATA;
			block_size = data[0] + 1;
			/* Check if the frame data runs off the end of the file	*/
			if (block_size > len) {
				block_size = len;
				return NSGIF_OK;
			}

			len -= block_size;
			data += block_size;
		}

		gif->frame_count = frame_idx + 1;
		gif->frames[frame_idx].display = true;
		*pos = data;

		/* Check if we've finished */
		if (len < 1) {
			return NSGIF_INSUFFICIENT_DATA;
		} else {
			if (data[0] == NSGIF_TRAILER) {
				return NSGIF_OK;
			}
		}

		return NSGIF_WORKING;
	}

	return ret;
}

static struct nsgif_frame *nsgif__get_frame(
		struct nsgif_animation *gif,
		uint32_t frame_idx)
{
	struct nsgif_frame *frame;

	if (gif->frame_holders > frame_idx) {
		frame = &gif->frames[frame_idx];
	} else {
		/* Allocate more memory */
		size_t count = frame_idx + 1;
		struct nsgif_frame *temp;

		temp = realloc(gif->frames, count * sizeof(*frame));
		if (temp == NULL) {
			return NULL;
		}
		gif->frames = temp;
		gif->frame_holders = count;

		frame = &gif->frames[frame_idx];

		frame->transparency = false;
		frame->transparency_index = NSGIF_NO_TRANSPARENCY;
		frame->frame_pointer = gif->buffer_position;
		frame->redraw_required = false;
		frame->disposal_method = 0;
		frame->frame_delay = 100;
		frame->display = false;
		frame->decoded = false;
	}

	return frame;
}

/**
 * Attempts to initialise the next frame
 *
 * \param[in] gif       The animation context
 * \param[in] frame_idx The frame number to decode.
 * \param[in] decode    Whether to decode the graphical image data.
 * \return error code
 *         - NSGIF_INSUFFICIENT_DATA reached unexpected end of source data.
 *         - NSGIF_FRAME_DATA_ERROR for GIF frame data error
 *         - NSGIF_INSUFFICIENT_MEMORY for insufficient memory to process
 *         - NSGIF_DATA_ERROR for GIF error (invalid frame header)
 *         - NSGIF_OK for successful decoding
 *         - NSGIF_WORKING for successful decoding if more frames are expected
*/
static nsgif_result nsgif__process_frame(
		struct nsgif_animation *gif,
		uint32_t frame_idx,
		bool decode)
{
	nsgif_result ret;
	const uint8_t *pos;
	const uint8_t *end;
	struct nsgif_frame *frame;

	frame = nsgif__get_frame(gif, frame_idx);
	if (frame == NULL) {
		return NSGIF_INSUFFICIENT_MEMORY;
	}

	end = gif->nsgif_data + gif->buffer_size;

	if (decode) {
		pos = gif->nsgif_data + frame->frame_pointer;

		/* Ensure this frame is supposed to be decoded */
		if (frame->display == false) {
			return NSGIF_OK;
		}

		/* Ensure the frame is in range to decode */
		if (frame_idx > gif->frame_count_partial) {
			return NSGIF_INSUFFICIENT_DATA;
		}

		/* Done if frame is already decoded */
		if ((int)frame_idx == gif->decoded_frame) {
			return NSGIF_OK;
		}
	} else {
		pos = (uint8_t *)(gif->nsgif_data + gif->buffer_position);

		/* Check if we've finished */
		if (pos < end && pos[0] == NSGIF_TRAILER) {
			return NSGIF_OK;
		}

		/* We could theoretically get some junk data that gives us
		 * millions of frames, so we ensure that we don't have a
		 * silly number. */
		if (frame_idx > 4096) {
			return NSGIF_FRAME_DATA_ERROR;
		}
	}

	ret = nsgif__parse_frame_extensions(gif, frame, &pos, !decode);
	if (ret != NSGIF_OK) {
		goto cleanup;
	}

	ret = nsgif__parse_image_descriptor(gif, frame, &pos, !decode);
	if (ret != NSGIF_OK) {
		goto cleanup;
	}

	ret = nsgif__parse_colour_table(gif, frame, &pos, decode);
	if (ret != NSGIF_OK) {
		goto cleanup;
	}

	ret = nsgif__parse_image_data(gif, frame, &pos, decode);
	if (ret != NSGIF_OK) {
		goto cleanup;
	}

cleanup:
	if (!decode) {
		gif->buffer_position = pos - gif->nsgif_data;
	}

	return ret;
}

/* exported function documented in libnsgif.h */
void nsgif_create(nsgif_animation *gif, nsgif_bitmap_cb_vt *bitmap)
{
	memset(gif, 0, sizeof(nsgif_animation));
	gif->bitmap = *bitmap;
	gif->decoded_frame = NSGIF_INVALID_FRAME;
	gif->prev_index = NSGIF_INVALID_FRAME;
}

/**
 * Read GIF header.
 *
 * 6-byte GIF file header is:
 *
 *  +0   3CHARS   Signature ('GIF')
 *  +3   3CHARS   Version ('87a' or '89a')
 *
 * \param[in]      gif     The GIF object we're decoding.
 * \param[in,out]  pos     The current buffer position, updated on success.
 * \param[in]      strict  Whether to require a known GIF version.
 * \return NSGIF_OK on success, appropriate error otherwise.
 */
static nsgif_result nsgif__parse_header(
		struct nsgif_animation *gif,
		const uint8_t **pos,
		bool strict)
{
	const uint8_t *data = *pos;
	size_t len = gif->nsgif_data + gif->buffer_size - data;

	if (len < 6) {
		return NSGIF_INSUFFICIENT_DATA;
	}

	if (strncmp((const char *) data, "GIF", 3) != 0) {
		return NSGIF_DATA_ERROR;
	}
	data += 3;

	if (strict == true) {
		if ((strncmp((const char *) data, "87a", 3) != 0) &&
		    (strncmp((const char *) data, "89a", 3) != 0)) {
			return NSGIF_DATA_ERROR;
		}
	}
	data += 3;

	*pos = data;
	return NSGIF_OK;
}

/**
 * Read Logical Screen Descriptor.
 *
 * 7-byte Logical Screen Descriptor is:
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
 *
 * \param[in]      gif     The GIF object we're decoding.
 * \param[in,out]  pos     The current buffer position, updated on success.
 * \return NSGIF_OK on success, appropriate error otherwise.
 */
static nsgif_result nsgif__parse_logical_screen_descriptor(
		struct nsgif_animation *gif,
		const uint8_t **pos)
{
	const uint8_t *data = *pos;
	size_t len = gif->nsgif_data + gif->buffer_size - data;

	if (len < 7) {
		return NSGIF_INSUFFICIENT_DATA;
	}

	gif->width = data[0] | (data[1] << 8);
	gif->height = data[2] | (data[3] << 8);
	gif->global_colours = data[4] & NSGIF_COLOUR_TABLE_MASK;
	gif->colour_table_size = 2 << (data[4] & NSGIF_COLOUR_TABLE_SIZE_MASK);
	gif->bg_index = data[5];
	gif->aspect_ratio = data[6];
	gif->loop_count = 1;

	*pos += 7;
	return NSGIF_OK;
}

/* exported function documented in libnsgif.h */
nsgif_result nsgif_initialise(nsgif_animation *gif, size_t size, const uint8_t *data)
{
	const uint8_t *nsgif_data;
	nsgif_result ret;

	/* Initialize values */
	gif->buffer_size = size;
	gif->nsgif_data = data;

	/* Get our current processing position */
	nsgif_data = gif->nsgif_data + gif->buffer_position;

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
		gif->decoded_frame = NSGIF_INVALID_FRAME;

		ret = nsgif__parse_header(gif, &nsgif_data, false);
		if (ret != NSGIF_OK) {
			return ret;
		}

		ret = nsgif__parse_logical_screen_descriptor(gif, &nsgif_data);
		if (ret != NSGIF_OK) {
			return ret;
		}

		/* Remember we've done this now */
		gif->buffer_position = nsgif_data - gif->nsgif_data;

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
		gif->global_colour_table = calloc(NSGIF_MAX_COLOURS, sizeof(uint32_t));
		gif->local_colour_table = calloc(NSGIF_MAX_COLOURS, sizeof(uint32_t));
		if ((gif->global_colour_table == NULL) ||
		    (gif->local_colour_table == NULL)) {
			nsgif_finalise(gif);
			return NSGIF_INSUFFICIENT_MEMORY;
		}

		/* Set the first colour to a value that will never occur in
		 * reality so we know if we've processed it
		*/
		gif->global_colour_table[0] = NSGIF_PROCESS_COLOURS;

		/* Check if the GIF has no frame data (13-byte header + 1-byte
		 * termination block) Although generally useless, the GIF
		 * specification does not expressly prohibit this
		 */
		if (gif->buffer_size == gif->buffer_position + 1) {
			if (nsgif_data[0] == NSGIF_TRAILER) {
				return NSGIF_OK;
			}
		}
	}

	/*  Do the colour map if we haven't already. As the top byte is always
	 *  0xff or 0x00 depending on the transparency we know if it's been
	 *  filled in.
	 */
	if (gif->global_colour_table[0] == NSGIF_PROCESS_COLOURS) {
		/* Check for a global colour map signified by bit 7 */
		if (gif->global_colours) {
			ret = nsgif__colour_table_extract(gif,
					gif->global_colour_table,
					gif->colour_table_size,
					&nsgif_data, true);
			if (ret != NSGIF_OK) {
				return ret;
			}

			gif->buffer_position = (nsgif_data - gif->nsgif_data);
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

	if (gif->lzw_ctx == NULL) {
		lzw_result res = lzw_context_create(
				(struct lzw_ctx **)&gif->lzw_ctx);
		if (res != LZW_OK) {
			return nsgif__error_from_lzw(res);
		}
	}

	/* Repeatedly try to initialise frames */
	do {
		ret = nsgif__process_frame(gif, gif->frame_count, false);
	} while (ret == NSGIF_WORKING);

	return ret;
}

/* exported function documented in libnsgif.h */
nsgif_result nsgif_decode_frame(nsgif_animation *gif, uint32_t frame)
{
	return nsgif__process_frame(gif, frame, true);
}

/* exported function documented in libnsgif.h */
void nsgif_finalise(nsgif_animation *gif)
{
	/* Release all our memory blocks */
	if (gif->frame_image) {
		assert(gif->bitmap.destroy);
		gif->bitmap.destroy(gif->frame_image);
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
