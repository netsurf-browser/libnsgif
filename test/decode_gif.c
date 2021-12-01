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

#include "../include/libnsgif.h"

#define BYTES_PER_PIXEL 4
#define MAX_IMAGE_BYTES (48 * 1024 * 1024)

static void *bitmap_create(int width, int height)
{
	/* ensure a stupidly large bitmap is not created */
	if (((long long)width * (long long)height) > (MAX_IMAGE_BYTES/BYTES_PER_PIXEL)) {
		return NULL;
	}
	return calloc(width * height, BYTES_PER_PIXEL);
}

static void bitmap_set_opaque(void *bitmap, bool opaque)
{
	(void) opaque;  /* unused */
	(void) bitmap;  /* unused */
	assert(bitmap);
}

static bool bitmap_test_opaque(void *bitmap)
{
	(void) bitmap;  /* unused */
	assert(bitmap);
	return false;
}

static unsigned char *bitmap_get_buffer(void *bitmap)
{
	assert(bitmap);
	return bitmap;
}

static void bitmap_destroy(void *bitmap)
{
	assert(bitmap);
	free(bitmap);
}

static void bitmap_modified(void *bitmap)
{
	(void) bitmap;  /* unused */
	assert(bitmap);
	return;
}

static unsigned char *load_file(const char *path, size_t *data_size)
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

static void warning(const char *context, nsgif_result code)
{
	fprintf(stderr, "%s failed: ", context);
	switch (code)
	{
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
		fprintf(stderr, "unknown code %i", code);
		break;
	}
	fprintf(stderr, "\n");
}

static void write_ppm(FILE* fh, const char *name, nsgif_animation *gif,
		bool no_write)
{
	unsigned int i;
	nsgif_result code;

	if (!no_write) {
		fprintf(fh, "P3\n");
		fprintf(fh, "# %s\n", name);
		fprintf(fh, "# width                %u \n", gif->width);
		fprintf(fh, "# height               %u \n", gif->height);
		fprintf(fh, "# frame_count          %u \n", gif->frame_count);
		fprintf(fh, "# frame_count_partial  %u \n", gif->frame_count_partial);
		fprintf(fh, "# loop_count           %u \n", gif->loop_count);
		fprintf(fh, "%u %u 256\n", gif->width, gif->height * gif->frame_count);
	}

	/* decode the frames */
	for (i = 0; i != gif->frame_count; i++) {
		unsigned int row, col;
		unsigned char *image;

		code = nsgif_decode_frame(gif, i);
		if (code != NSGIF_OK)
			warning("nsgif_decode_frame", code);

		if (!gif->frames[i].display) {
			continue;
		}

		if (!no_write) {
			fprintf(fh, "# frame %u:\n", i);
			image = (unsigned char *) gif->frame_image;
			for (row = 0; row != gif->height; row++) {
				for (col = 0; col != gif->width; col++) {
					size_t z = (row * gif->width + col) * 4;
					fprintf(fh, "%u %u %u ",
						(unsigned char) image[z],
						(unsigned char) image[z + 1],
						(unsigned char) image[z + 2]);
				}
				fprintf(fh, "\n");
			}
		}
	}
}

int main(int argc, char *argv[])
{
	nsgif_bitmap_cb_vt bitmap_callbacks = {
		bitmap_create,
		bitmap_destroy,
		bitmap_get_buffer,
		bitmap_set_opaque,
		bitmap_test_opaque,
		bitmap_modified
	};
	nsgif_animation gif;
	size_t size;
	nsgif_result code;
	unsigned char *data;
	FILE *outf = stdout;
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
	nsgif_create(&gif, &bitmap_callbacks);

	/* load file into memory */
	data = load_file(argv[1], &size);

	/* begin decoding */
	do {
		code = nsgif_initialise(&gif, size, data);
		if (code != NSGIF_OK && code != NSGIF_WORKING) {
			warning("nsgif_initialise", code);
			nsgif_finalise(&gif);
			free(data);
			return 1;
		}
	} while (code != NSGIF_OK);

	write_ppm(outf, argv[1], &gif, no_write);

	if (argc > 2 && !no_write) {
		fclose(outf);
	}

	/* clean up */
	nsgif_finalise(&gif);
	free(data);

	return 0;
}
