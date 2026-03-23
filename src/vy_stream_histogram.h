#ifndef TARANTOOL_VY_STREAM_HISTOGRAM_H_INCLUDED
#define TARANTOOL_VY_STREAM_HISTOGRAM_H_INCLUDED
/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the
 *    disclaimer in any other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL AUTHORS
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

#if defined(__cplusplus)
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/** One bin: observation coordinate @a p and total weight @a count. */
struct vy_stream_histogram_bin {
	double p;
	uint64_t count;
};

/**
 * Streaming histogram: bins kept sorted by @a p (ascending).
 * Based on Ben-Haim & Tom-Tov, JMLR 2010 (see ScyllaDB streaming_histogram).
 */
struct vy_stream_histogram {
	/** Maximum number of bins after merges; must be >= 1. */
	uint32_t max_bin_size;
	/** Current bin count (<= max_bin_size, except transiently during update). */
	uint32_t bin_count;
	/** Allocated slots (>= max_bin_size + 1 for one insert before merge). */
	uint32_t capacity;
	struct vy_stream_histogram_bin bins[];
};

/**
 * Allocate a histogram. @a max_bin_size must be >= 1.
 * Returns NULL on invalid args or OOM.
 */
struct vy_stream_histogram *
vy_stream_histogram_new(uint32_t max_bin_size);

void
vy_stream_histogram_delete(struct vy_stream_histogram *h);

/** Add one observation at @a p (weight 1). */
void
vy_stream_histogram_update(struct vy_stream_histogram *h, double p);

/** Add weight @a m at coordinate @a p. */
void
vy_stream_histogram_update_weight(struct vy_stream_histogram *h, double p,
				  uint64_t m);

/** Merge @a other into @a h (same semantics as Scylla merge). */
void
vy_stream_histogram_merge(struct vy_stream_histogram *h,
			  const struct vy_stream_histogram *other);

/**
 * Estimated cumulative count in (-inf, b] (see Scylla streaming_histogram::sum).
 */
double
vy_stream_histogram_sum(const struct vy_stream_histogram *h, double b);

/** Msgpack array: max_bin_size, bin_count, then (p, count) pairs. */
size_t
vy_stream_histogram_msgpack_size(const struct vy_stream_histogram *h);

char *
vy_stream_histogram_msgpack_encode(const struct vy_stream_histogram *h,
				   char *pos);

/** Returns NULL on format error. Advances *data past the value. */
struct vy_stream_histogram *
vy_stream_histogram_msgpack_decode(const char **data);

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* TARANTOOL_VY_STREAM_HISTOGRAM_H_INCLUDED */
