/*
 * Copyright 2008 Sean Fox <dyntryx@gmail.com>
 * Copyright 2008 James Bursa <james@netsurf-browser.org>
 *
 * This file is part of NetSurf's libnsgif, http://www.netsurf-browser.org/
 * Licenced under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/nsgif.h"

#define BYTES_PER_PIXEL 4

static void *bitmap_create(int width, int height)
{
	/* Ensure a stupidly large bitmap is not created */
	if (width > 4096 || height > 4096) {
		return NULL;
	}

	return calloc(width * height, BYTES_PER_PIXEL);
}

static unsigned char *bitmap_get_buffer(void *bitmap)
{
	return bitmap;
}

static void bitmap_destroy(void *bitmap)
{
	free(bitmap);
}

static uint8_t *load_file(const char *path, size_t *data_size)
{
	FILE *fd;
	struct stat sb;
	unsigned char *buffer;
	size_t size;
	size_t n;

	fd = fopen(path, "rb");
	if (!fd) {
		perror(path);
		exit(EXIT_FAILURE);
	}

	if (stat(path, &sb)) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	size = sb.st_size;

	buffer = malloc(size);
	if (!buffer) {
		fprintf(stderr, "Unable to allocate %lld bytes\n",
			(long long) size);
		exit(EXIT_FAILURE);
	}

	n = fread(buffer, 1, size, fd);
	if (n != size) {
		perror(path);
		exit(EXIT_FAILURE);
	}

	fclose(fd);

	*data_size = size;
	return buffer;
}

static void warning(const char *context, nsgif_result gif_res)
{
	fprintf(stderr, "%s failed: ", context);
	switch (gif_res) {
	case NSGIF_FRAME_DATA_ERROR:
		fprintf(stderr, "NSGIF_FRAME_DATA_ERROR");
		break;
	case NSGIF_INSUFFICIENT_DATA:
		fprintf(stderr, "NSGIF_INSUFFICIENT_DATA");
		break;
	case NSGIF_DATA_ERROR:
		fprintf(stderr, "NSGIF_DATA_ERROR");
		break;
	case NSGIF_INSUFFICIENT_MEMORY:
		fprintf(stderr, "NSGIF_INSUFFICIENT_MEMORY");
		break;
	default:
		fprintf(stderr, "unknown code %i", gif_res);
		break;
	}
	fprintf(stderr, "\n");
}

static void write_ppm(FILE* fh, const char *name, nsgif *gif, bool no_write)
{
	nsgif_result gif_res;
	uint32_t frame_prev = 0;
	const nsgif_info_t *info;

	info = nsgif_get_info(gif);

	if (!no_write) {
		fprintf(fh, "P3\n");
		fprintf(fh, "# %s\n", name);
		fprintf(fh, "# width                %u \n", info->width);
		fprintf(fh, "# height               %u \n", info->height);
		fprintf(fh, "# frame_count          %u \n", info->frame_count);
		fprintf(fh, "# loop_max             %u \n", info->loop_max);
		fprintf(fh, "%u %u 256\n", info->width,
				info->height * info->frame_count);
	}

	/* decode the frames */
	while (true) {
		const uint32_t *buffer;
		const uint8_t *image;
		uint32_t frame_new;
		uint32_t delay_cs;
		nsgif_rect area;

		gif_res = nsgif_frame_prepare(gif, &area,
				&delay_cs, &frame_new);
		if (gif_res != NSGIF_OK) {
			warning("nsgif_frame_prepare", gif_res);
			return;
		}

		if (frame_new < frame_prev) {
			/* Must be an animation that loops. We only care about
			 * decoding each frame once. */
			return;
		}
		frame_prev = frame_new;

		gif_res = nsgif_frame_decode(gif, frame_new, &buffer);
		if (gif_res != NSGIF_OK) {
			warning("nsgif_decode_frame", gif_res);
			return;
		}

		if (!no_write) {
			fprintf(fh, "# frame %u:\n", frame_new);
			image = (const uint8_t *) buffer;
			for (uint32_t y = 0; y != info->height; y++) {
				for (uint32_t x = 0; x != info->width; x++) {
					size_t z = (y * info->width + x) * 4;
					fprintf(fh, "%u %u %u ",
							image[z],
							image[z + 1],
							image[z + 2]);
				}
				fprintf(fh, "\n");
			}
		}
	}
}

int main(int argc, char *argv[])
{
	const nsgif_bitmap_cb_vt bitmap_callbacks = {
		.create     = bitmap_create,
		.destroy    = bitmap_destroy,
		.get_buffer = bitmap_get_buffer,
	};
	nsgif *gif;
	size_t size;
	uint8_t *data;
	FILE *outf = stdout;
	nsgif_result gif_res;
	bool no_write = false;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s image.gif [out]\n", argv[0]);
		fprintf(stderr, "\n");
		fprintf(stderr, "If [out] is NOWRITE, the gif will be docoded "
				"but not output.\n");
		fprintf(stderr, "Otherwise [out] is an output filename.\n");
		fprintf(stderr, "When [out] is unset, output is to stdout.\n");

		return 1;
	}

	if (argc > 2) {
		if (strcmp(argv[2], "NOWRITE") == 0) {
			no_write = true;
		} else {
			outf = fopen(argv[2], "w+");
			if (outf == NULL) {
				fprintf(stderr, "Unable to open %s for writing\n", argv[2]);
				return 2;
			}
		}
	}

	/* create our gif animation */
	gif_res = nsgif_create(&bitmap_callbacks, &gif);
	if (gif_res != NSGIF_OK) {
		return 1;
	}

	/* load file into memory */
	data = load_file(argv[1], &size);

	/* begin decoding */
	do {
		gif_res = nsgif_data_scan(gif, size, data);
		if (gif_res != NSGIF_OK && gif_res != NSGIF_WORKING) {
			warning("nsgif_data_scan", gif_res);
			nsgif_destroy(gif);
			free(data);
			return 1;
		}
	} while (gif_res != NSGIF_OK);

	write_ppm(outf, argv[1], gif, no_write);

	if (argc > 2 && !no_write) {
		fclose(outf);
	}

	/* clean up */
	nsgif_destroy(gif);
	free(data);

	return 0;
}
