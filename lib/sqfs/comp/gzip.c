/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gzip.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#define SQFS_BUILDING_DLL
#include "config.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <zlib.h>

#include "internal.h"

typedef struct {
	uint32_t level;
	uint16_t window;
	uint16_t strategies;
} gzip_options_t;

typedef struct {
	sqfs_compressor_t base;

	z_stream strm;
	bool compress;

	size_t block_size;
	gzip_options_t opt;
} gzip_compressor_t;

static void gzip_destroy(sqfs_compressor_t *base)
{
	gzip_compressor_t *gzip = (gzip_compressor_t *)base;

	if (gzip->compress) {
		deflateEnd(&gzip->strm);
	} else {
		inflateEnd(&gzip->strm);
	}

	free(gzip);
}

static int gzip_write_options(sqfs_compressor_t *base, int fd)
{
	gzip_compressor_t *gzip = (gzip_compressor_t *)base;
	gzip_options_t opt;

	if (gzip->opt.level == SQFS_GZIP_DEFAULT_LEVEL &&
	    gzip->opt.window == SQFS_GZIP_DEFAULT_WINDOW &&
	    gzip->opt.strategies == 0) {
		return 0;
	}

	opt.level = htole32(gzip->opt.level);
	opt.window = htole16(gzip->opt.window);
	opt.strategies = htole16(gzip->opt.strategies);

	return sqfs_generic_write_options(fd, &opt, sizeof(opt));
}

static int gzip_read_options(sqfs_compressor_t *base, int fd)
{
	gzip_compressor_t *gzip = (gzip_compressor_t *)base;
	gzip_options_t opt;

	if (sqfs_generic_read_options(fd, &opt, sizeof(opt)))
		return -1;

	gzip->opt.level = le32toh(opt.level);
	gzip->opt.window = le16toh(opt.window);
	gzip->opt.strategies = le16toh(opt.strategies);

	if (gzip->opt.level < 1 || gzip->opt.level > 9) {
		fprintf(stderr, "Invalid gzip compression level '%d'.\n",
			gzip->opt.level);
		return -1;
	}

	if (gzip->opt.window < 8 || gzip->opt.window > 15) {
		fprintf(stderr, "Invalid gzip window size '%d'.\n",
			gzip->opt.window);
		return -1;
	}

	if (gzip->opt.strategies & ~SQFS_COMP_FLAG_GZIP_ALL) {
		fputs("Unknown gzip strategies selected.\n", stderr);
		return -1;
	}

	return 0;
}

static int flag_to_zlib_strategy(int flag)
{
	switch (flag) {
	case SQFS_COMP_FLAG_GZIP_DEFAULT:
		return Z_DEFAULT_STRATEGY;
	case SQFS_COMP_FLAG_GZIP_FILTERED:
		return Z_FILTERED;
	case SQFS_COMP_FLAG_GZIP_HUFFMAN:
		return Z_HUFFMAN_ONLY;
	case SQFS_COMP_FLAG_GZIP_RLE:
		return Z_RLE;
	case SQFS_COMP_FLAG_GZIP_FIXED:
		return Z_FIXED;
	}

	return 0;
}

static int find_strategy(gzip_compressor_t *gzip, const uint8_t *in,
			 size_t size, uint8_t *out, size_t outsize)
{
	int ret, strategy, selected = Z_DEFAULT_STRATEGY;
	size_t i, length, minlength = 0;

	for (i = 0x01; i & SQFS_COMP_FLAG_GZIP_ALL; i <<= 1) {
		if ((gzip->opt.strategies & i) == 0)
			continue;

		ret = deflateReset(&gzip->strm);
		if (ret != Z_OK) {
			fputs("resetting zlib stream failed\n",
			      stderr);
			return -1;
		}

		strategy = flag_to_zlib_strategy(i);

		gzip->strm.next_in = (void *)in;
		gzip->strm.avail_in = size;
		gzip->strm.next_out = out;
		gzip->strm.avail_out = outsize;

		ret = deflateParams(&gzip->strm, gzip->opt.level, strategy);
		if (ret != Z_OK) {
			fputs("setting deflate parameters failed\n",
			      stderr);
			return -1;
		}

		ret = deflate(&gzip->strm, Z_FINISH);

		if (ret == Z_STREAM_END) {
			length = gzip->strm.total_out;

			if (minlength == 0 || length < minlength) {
				minlength = length;
				selected = strategy;
			}
		} else if (ret != Z_OK && ret != Z_BUF_ERROR) {
			fputs("gzip block processing failed\n", stderr);
			return -1;
		}
	}

	return selected;
}

static ssize_t gzip_do_block(sqfs_compressor_t *base, const uint8_t *in,
			     size_t size, uint8_t *out, size_t outsize)
{
	gzip_compressor_t *gzip = (gzip_compressor_t *)base;
	int ret, strategy = 0;
	size_t written;

	if (gzip->compress && gzip->opt.strategies != 0) {
		strategy = find_strategy(gzip, in, size, out, outsize);
		if (strategy < 0)
			return -1;
	}

	if (gzip->compress) {
		ret = deflateReset(&gzip->strm);
	} else {
		ret = inflateReset(&gzip->strm);
	}

	if (ret != Z_OK) {
		fputs("resetting zlib stream failed\n", stderr);
		return -1;
	}

	gzip->strm.next_in = (void *)in;
	gzip->strm.avail_in = size;
	gzip->strm.next_out = out;
	gzip->strm.avail_out = outsize;

	if (gzip->compress && gzip->opt.strategies != 0) {
		ret = deflateParams(&gzip->strm, gzip->opt.level, strategy);
		if (ret != Z_OK) {
			fputs("setting selcted deflate parameters failed\n",
			      stderr);
			return -1;
		}
	}

	if (gzip->compress) {
		ret = deflate(&gzip->strm, Z_FINISH);
	} else {
		ret = inflate(&gzip->strm, Z_FINISH);
	}

	if (ret == Z_STREAM_END) {
		written = gzip->strm.total_out;

		if (gzip->compress && written >= size)
			return 0;

		return (ssize_t)written;
	}

	if (ret != Z_OK && ret != Z_BUF_ERROR) {
		fputs("gzip block processing failed\n", stderr);
		return -1;
	}

	return 0;
}

static sqfs_compressor_t *gzip_create_copy(sqfs_compressor_t *cmp)
{
	gzip_compressor_t *gzip = malloc(sizeof(*gzip));
	int ret;

	if (gzip == NULL) {
		perror("creating additional gzip compressor");
		return NULL;
	}

	memcpy(gzip, cmp, sizeof(*gzip));
	memset(&gzip->strm, 0, sizeof(gzip->strm));

	if (gzip->compress) {
		ret = deflateInit2(&gzip->strm, gzip->opt.level, Z_DEFLATED,
				   gzip->opt.window, 8, Z_DEFAULT_STRATEGY);
	} else {
		ret = inflateInit(&gzip->strm);
	}

	if (ret != Z_OK) {
		fputs("internal error creating additional zlib stream\n",
		      stderr);
		free(gzip);
		return NULL;
	}

	return (sqfs_compressor_t *)gzip;
}

sqfs_compressor_t *gzip_compressor_create(const sqfs_compressor_config_t *cfg)
{
	gzip_compressor_t *gzip;
	sqfs_compressor_t *base;
	int ret;

	if (cfg->flags & ~(SQFS_COMP_FLAG_GZIP_ALL |
			   SQFS_COMP_FLAG_GENERIC_ALL)) {
		fputs("creating gzip compressor: unknown compressor flags\n",
		      stderr);
		return NULL;
	}

	if (cfg->opt.gzip.level < SQFS_GZIP_MIN_LEVEL ||
	    cfg->opt.gzip.level > SQFS_GZIP_MAX_LEVEL) {
		fprintf(stderr, "creating gzip compressor: compression level"
			"must be between %d and %d inclusive\n",
			SQFS_GZIP_MIN_LEVEL, SQFS_GZIP_MAX_LEVEL);
		return NULL;
	}

	if (cfg->opt.gzip.window_size < SQFS_GZIP_MIN_WINDOW ||
	    cfg->opt.gzip.window_size > SQFS_GZIP_MAX_WINDOW) {
		fprintf(stderr, "creating gzip compressor: window size"
			"must be between %d and %d inclusive\n",
			SQFS_GZIP_MIN_WINDOW, SQFS_GZIP_MAX_WINDOW);
		return NULL;
	}

	gzip = calloc(1, sizeof(*gzip));
	base = (sqfs_compressor_t *)gzip;

	if (gzip == NULL) {
		perror("creating gzip compressor");
		return NULL;
	}

	gzip->opt.level = cfg->opt.gzip.level;
	gzip->opt.window = cfg->opt.gzip.window_size;
	gzip->opt.strategies = cfg->flags & SQFS_COMP_FLAG_GZIP_ALL;
	gzip->compress = (cfg->flags & SQFS_COMP_FLAG_UNCOMPRESS) == 0;
	gzip->block_size = cfg->block_size;
	base->do_block = gzip_do_block;
	base->destroy = gzip_destroy;
	base->write_options = gzip_write_options;
	base->read_options = gzip_read_options;
	base->create_copy = gzip_create_copy;

	if (gzip->compress) {
		ret = deflateInit2(&gzip->strm, cfg->opt.gzip.level,
				   Z_DEFLATED, cfg->opt.gzip.window_size, 8,
				   Z_DEFAULT_STRATEGY);
	} else {
		ret = inflateInit(&gzip->strm);
	}

	if (ret != Z_OK) {
		fputs("internal error creating zlib stream\n", stderr);
		free(gzip);
		return NULL;
	}

	return base;
}