/*
 * Copyright 2004 Richard Wilson <richard.wilson@netsurf-browser.org>
 * Copyright 2008 Sean Fox <dyntryx@gmail.com>
 * Copyright 2013-2021 Michael Drake <tlsa@netsurf-browser.org>
 *
 * This file is part of NetSurf's libnsgif, http://www.netsurf-browser.org/
 * Licenced under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 */

/**
 * \file
 * Interface to progressive animated GIF file decoding.
 */

#ifndef NSNSGIF_H
#define NSNSGIF_H

#include <stdint.h>
#include <stdbool.h>

/* Error return values */
typedef enum {
	NSGIF_WORKING = 1,
	NSGIF_OK = 0,
	NSGIF_INSUFFICIENT_DATA = -1,
	NSGIF_INSUFFICIENT_FRAME_DATA = NSGIF_INSUFFICIENT_DATA,
	NSGIF_FRAME_DATA_ERROR = -2,
	NSGIF_DATA_ERROR = -4,
	NSGIF_INSUFFICIENT_MEMORY = -5,
	NSGIF_FRAME_NO_DISPLAY = -6,
	NSGIF_END_OF_FRAME = -7
} nsgif_result;

/** GIF rectangle structure. */
typedef struct nsgif_rect {
	/** x co-ordinate of redraw rectangle */
	uint32_t x;
	/** y co-ordinate of redraw rectangle */
	uint32_t y;
	/** width of redraw rectangle */
	uint32_t w;
	/** height of redraw rectangle */
	uint32_t h;
} nsgif_rect;

/** GIF frame data */
typedef struct nsgif_frame {
	/** whether the frame should be displayed/animated */
	bool display;
	/** delay (in cs) before animating the frame */
	uint32_t frame_delay;

	/* Internal members are listed below */

	/** offset (in bytes) to the GIF frame data */
	uint32_t frame_pointer;
	/** whether the frame has previously been decoded. */
	bool decoded;
	/** whether the frame is totally opaque */
	bool opaque;
	/** whether a full image redraw is required */
	bool redraw_required;
	/** how the previous frame should be disposed; affects plotting */
	uint8_t disposal_method;
	/** whether we acknowledge transparency */
	bool transparency;
	/** the index designating a transparent pixel */
	uint32_t transparency_index;
	/* Frame flags */
	uint32_t flags;

	/** Frame's redraw rectangle. */
	nsgif_rect redraw;
} nsgif_frame;

/* API for Bitmap callbacks */
typedef void* (*nsgif_bitmap_cb_create)(int width, int height);
typedef void (*nsgif_bitmap_cb_destroy)(void *bitmap);
typedef uint8_t* (*nsgif_bitmap_cb_get_buffer)(void *bitmap);
typedef void (*nsgif_bitmap_cb_set_opaque)(void *bitmap, bool opaque);
typedef bool (*nsgif_bitmap_cb_test_opaque)(void *bitmap);
typedef void (*nsgif_bitmap_cb_modified)(void *bitmap);

/** Bitmap callbacks function table */
typedef struct nsgif_bitmap_cb_vt {
	/** Create a bitmap. */
	nsgif_bitmap_cb_create create;
	/** Free a bitmap. */
	nsgif_bitmap_cb_destroy destroy;
	/** Return a pointer to the pixel data in a bitmap. */
	nsgif_bitmap_cb_get_buffer get_buffer;

	/* Members below are optional */

	/** Sets whether a bitmap should be plotted opaque. */
	nsgif_bitmap_cb_set_opaque set_opaque;
	/** Tests whether a bitmap has an opaque alpha channel. */
	nsgif_bitmap_cb_test_opaque test_opaque;
	/** The bitmap image has changed, so flush any persistent cache. */
	nsgif_bitmap_cb_modified modified;
} nsgif_bitmap_cb_vt;

/** GIF animation data */
typedef struct nsgif {
	/** LZW decode context */
	void *lzw_ctx;
	/** callbacks for bitmap functions */
	nsgif_bitmap_cb_vt bitmap;
	/** pointer to GIF data */
	const uint8_t *nsgif_data;
	/** width of GIF (may increase during decoding) */
	uint32_t width;
	/** height of GIF (may increase during decoding) */
	uint32_t height;
	/** number of frames decoded */
	uint32_t frame_count;
	/** number of frames partially decoded */
	uint32_t frame_count_partial;
	/** decoded frames */
	nsgif_frame *frames;
	/** current frame decoded to bitmap */
	int decoded_frame;
	/** currently decoded image; stored as bitmap from bitmap_create callback */
	void *frame_image;
	/** number of times to loop animation */
	int loop_count;

	/* Internal members are listed below */

	/** current index into GIF data */
	uint32_t buffer_position;
	/** total number of bytes of GIF data available */
	uint32_t buffer_size;
	/** current number of frame holders */
	uint32_t frame_holders;
	/** background index */
	uint32_t bg_index;
	/** background colour */
	uint32_t bg_colour;
	/** image aspect ratio (ignored) */
	uint32_t aspect_ratio;
	/** size of colour table (in entries) */
	uint32_t colour_table_size;
	/** whether the GIF has a global colour table */
	bool global_colours;
	/** global colour table */
	uint32_t *global_colour_table;
	/** local colour table */
	uint32_t *local_colour_table;
	/** current colour table */
	uint32_t *colour_table;

	/** previous frame for NSGIF_FRAME_RESTORE */
	void *prev_frame;
	/** previous frame index */
	int prev_index;
	/** previous frame width */
	unsigned prev_width;
	/** previous frame height */
	unsigned prev_height;
} nsgif;

/**
 * Initialises necessary nsgif members.
 */
void nsgif_create(nsgif *gif, nsgif_bitmap_cb_vt *bitmap_callbacks);

/**
 * Initialises any workspace held by the animation and attempts to decode
 * any information that hasn't already been decoded.
 * If an error occurs, all previously decoded frames are retained.
 *
 * \return Error return value.
 *         - NSGIF_FRAME_DATA_ERROR for GIF frame data error
 *         - NSGIF_INSUFFICIENT_DATA reached unexpected end of source data
 *         - NSGIF_INSUFFICIENT_MEMORY for memory error
 *         - NSGIF_DATA_ERROR for GIF error
 *         - NSGIF_OK for successful decoding
 *         - NSGIF_WORKING for successful decoding if more frames are expected
 */
nsgif_result nsgif_initialise(nsgif *gif, size_t size, const uint8_t *data);

/**
 * Decodes a GIF frame.
 *
 * \return Error return value.
 *         - NSGIF_FRAME_DATA_ERROR for GIF frame data error
 *         - NSGIF_DATA_ERROR for GIF error (invalid frame header)
 *         - NSGIF_INSUFFICIENT_DATA reached unexpected end of source data
 *         - NSGIF_INSUFFICIENT_MEMORY for insufficient memory to process
 *         - NSGIF_OK for successful decoding
 */
nsgif_result nsgif_decode_frame(nsgif *gif, uint32_t frame);

/**
 * Releases any workspace held by a gif
 */
void nsgif_finalise(nsgif *gif);

#endif
