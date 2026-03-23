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

#include "vy_stream_histogram.h"

#include "trivia/util.h"

#include <msgpuck.h>

#include <assert.h>
#include <float.h>

struct vy_stream_histogram *
vy_stream_histogram_new(uint32_t max_bin_size)
{
	if (max_bin_size < 1)
		return NULL;
	/*
	 * Need room for one extra bin before merging down to max_bin_size.
	 */
	uint32_t cap = max_bin_size + 1;
	size_t bytes = sizeof(struct vy_stream_histogram) +
		       cap * sizeof(struct vy_stream_histogram_bin);
	struct vy_stream_histogram *h = xmalloc(bytes);
	h->max_bin_size = max_bin_size;
	h->bin_count = 0;
	h->capacity = cap;
	return h;
}

void
vy_stream_histogram_delete(struct vy_stream_histogram *h)
{
	free(h);
}

/** Merge adjacent bins @a i and @a i + 1 (indices valid, bin_count >= 2). */
static void
vy_stream_histogram_merge_adjacent(struct vy_stream_histogram *h, uint32_t i)
{
	assert(h->bin_count >= 2 && i + 1 < h->bin_count);
	double q1 = h->bins[i].p;
	uint64_t k1 = h->bins[i].count;
	double q2 = h->bins[i + 1].p;
	uint64_t k2 = h->bins[i + 1].count;
	uint64_t k = k1 + k2;
	assert(k > 0);
	double new_p = (q1 * (double)k1 + q2 * (double)k2) / (double)k;
	h->bins[i].p = new_p;
	h->bins[i].count = k;
	memmove(&h->bins[i + 1], &h->bins[i + 2],
		(h->bin_count - i - 2) * sizeof(h->bins[0]));
	h->bin_count--;
}

static void
vy_stream_histogram_trim(struct vy_stream_histogram *h)
{
	while (h->bin_count > h->max_bin_size) {
		assert(h->bin_count >= 2);
		double smallest_diff = DBL_MAX;
		uint32_t merge_at = 0;
		for (uint32_t i = 0; i + 1 < h->bin_count; i++) {
			double diff = h->bins[i + 1].p - h->bins[i].p;
			if (diff < smallest_diff) {
				smallest_diff = diff;
				merge_at = i;
			}
		}
		vy_stream_histogram_merge_adjacent(h, merge_at);
	}
}

/** Insert new bin at sorted position; may temporarily use capacity. */
static void
vy_stream_histogram_insert_bin(struct vy_stream_histogram *h, double p,
			       uint64_t m)
{
	assert(h->bin_count < h->capacity);
	uint32_t i = 0;
	while (i < h->bin_count && h->bins[i].p < p)
		i++;
	memmove(&h->bins[i + 1], &h->bins[i],
		(h->bin_count - i) * sizeof(h->bins[0]));
	h->bins[i].p = p;
	h->bins[i].count = m;
	h->bin_count++;
}

void
vy_stream_histogram_update_weight(struct vy_stream_histogram *h, double p,
				  uint64_t m)
{
	if (m == 0)
		return;
	for (uint32_t i = 0; i < h->bin_count; i++) {
		if (h->bins[i].p == p) {
			h->bins[i].count += m;
			return;
		}
		if (h->bins[i].p > p)
			break;
	}
	vy_stream_histogram_insert_bin(h, p, m);
	vy_stream_histogram_trim(h);
}

void
vy_stream_histogram_update(struct vy_stream_histogram *h, double p)
{
	vy_stream_histogram_update_weight(h, p, 1);
}

void
vy_stream_histogram_merge(struct vy_stream_histogram *h,
			  const struct vy_stream_histogram *other)
{
	if (other->bin_count == 0)
		return;
	for (uint32_t i = 0; i < other->bin_count; i++) {
		vy_stream_histogram_update_weight(h, other->bins[i].p,
						  other->bins[i].count);
	}
}

double
vy_stream_histogram_sum(const struct vy_stream_histogram *h, double b)
{
	const struct vy_stream_histogram_bin *bins = h->bins;
	uint32_t n = h->bin_count;
	if (n == 0)
		return 0;

	/* upper_bound(b): first index with p > b */
	uint32_t pnext_i = n;
	for (uint32_t i = 0; i < n; i++) {
		if (bins[i].p > b) {
			pnext_i = i;
			break;
		}
	}

	if (pnext_i == n) {
		double sum = 0;
		for (uint32_t i = 0; i < n; i++)
			sum += (double)bins[i].count;
		return sum;
	}

	/* lower_bound(b): first index with p >= b */
	uint32_t pi_i = n;
	for (uint32_t i = 0; i < n; i++) {
		if (bins[i].p >= b) {
			pi_i = i;
			break;
		}
	}

	if (pi_i == n)
		return 0;
	if (pi_i == 0 && b < bins[0].p)
		return 0;
	if (bins[pi_i].p != b)
		pi_i--;

	double pi_first = bins[pi_i].p;
	uint64_t pi_second = bins[pi_i].count;
	double pnext_first = bins[pnext_i].p;
	uint64_t pnext_second = bins[pnext_i].count;

	double weight = (b - pi_first) / (pnext_first - pi_first);
	double mb = (double)pi_second +
		      ((int64_t)pnext_second - (int64_t)pi_second) * weight;
	double sum = (pi_second + mb) * weight / 2.;
	sum += (double)pi_second / 2.;

	for (uint32_t i = 0; i < pi_i; i++)
		sum += (double)bins[i].count;

	return sum;
}

size_t
vy_stream_histogram_msgpack_size(const struct vy_stream_histogram *h)
{
	if (h == NULL || h->bin_count == 0)
		return 0;
	uint32_t bc = h->bin_count;
	size_t s = mp_sizeof_array(2 + 2 * bc);
	s += mp_sizeof_uint(h->max_bin_size);
	s += mp_sizeof_uint(bc);
	for (uint32_t i = 0; i < bc; i++) {
		s += mp_sizeof_double(h->bins[i].p);
		s += mp_sizeof_uint(h->bins[i].count);
	}
	return s;
}

char *
vy_stream_histogram_msgpack_encode(const struct vy_stream_histogram *h, char *pos)
{
	assert(h != NULL && h->bin_count > 0);
	uint32_t bc = h->bin_count;
	pos = mp_encode_array(pos, 2 + 2 * bc);
	pos = mp_encode_uint(pos, h->max_bin_size);
	pos = mp_encode_uint(pos, bc);
	for (uint32_t i = 0; i < bc; i++) {
		pos = mp_encode_double(pos, h->bins[i].p);
		pos = mp_encode_uint(pos, h->bins[i].count);
	}
	return pos;
}

struct vy_stream_histogram *
vy_stream_histogram_msgpack_decode(const char **data)
{
	const char *p = *data;
	uint32_t n = mp_decode_array(&p);
	if (n < 2 || (n - 2) % 2 != 0)
		return NULL;
	uint32_t max_bin_size = mp_decode_uint(&p);
	uint32_t bin_count = mp_decode_uint(&p);
	if (max_bin_size < 1 || bin_count > max_bin_size ||
	    2 + 2 * bin_count != n)
		return NULL;
	struct vy_stream_histogram *h = vy_stream_histogram_new(max_bin_size);
	if (h == NULL)
		return NULL;
	for (uint32_t i = 0; i < bin_count; i++) {
		h->bins[i].p = mp_decode_double(&p);
		h->bins[i].count = mp_decode_uint(&p);
	}
	h->bin_count = bin_count;
	*data = p;
	return h;
}
