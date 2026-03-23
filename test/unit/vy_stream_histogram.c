#include "vy_stream_histogram.h"

#include "unit.h"

#include <math.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

enum {
	N_RECORDS = 1000000,
	WEIGHT_PER_RECORD = 1000,
	/** Parametrized via test helpers; default stress-test value. */
	MAX_BINS = 100,
};

/** 10^9 — без выражений с * и / в коде теста. */
static const uint64_t TOTAL_WEIGHT = UINT64_C(1000000000);

/** 100 сегментов по 10000 ключей подряд: 0..9999, 10000..19999, … */
enum { SEGMENT_KEY_WIDTH = 10000 };

/** Ожидаемая масса на сегмент ~10^7; допуск без деления в рантайме. */
static const double SEGMENT_MASS_LO = 5000000.;
static const double SEGMENT_MASS_HI = 15000000.;

static void
test_new_invalid(void)
{
	header();
	plan(1);
	fail_if(vy_stream_histogram_new(0) != NULL);
	footer();
}

static void
test_single_bin_sum(void)
{
	header();
	plan(4);
	struct vy_stream_histogram *h = vy_stream_histogram_new(10);
	fail_if(h == NULL);

	vy_stream_histogram_update(h, 5.0);
	ok(fabs(vy_stream_histogram_sum(h, 4.0)) < 1e-9,
	   "sum below only bin -> 0");
	ok(fabs(vy_stream_histogram_sum(h, 5.0) - 1.0) < 1e-9,
	   "sum at bin edge");
	ok(fabs(vy_stream_histogram_sum(h, 100.0) - 1.0) < 1e-9,
	   "sum above bin -> total weight");

	vy_stream_histogram_delete(h);
	footer();
}

static void
test_accumulate_same_key(void)
{
	header();
	plan(2);
	struct vy_stream_histogram *h = vy_stream_histogram_new(5);
	vy_stream_histogram_update_weight(h, 2.0, 3);
	vy_stream_histogram_update_weight(h, 2.0, 7);
	is(h->bin_count, 1, "single bin");
	ok(fabs(vy_stream_histogram_sum(h, 2.0) - 10.0) < 1e-9, "total 10");

	vy_stream_histogram_delete(h);
	footer();
}

static void
test_merge_bins_when_full(void)
{
	header();
	plan(2);
	struct vy_stream_histogram *h = vy_stream_histogram_new(3);
	vy_stream_histogram_update(h, 1.0);
	vy_stream_histogram_update(h, 2.0);
	vy_stream_histogram_update(h, 4.0);
	vy_stream_histogram_update(h, 8.0);
	ok(h->bin_count <= 3, "at most max_bin_size bins");
	uint64_t total = 0;
	for (uint32_t i = 0; i < h->bin_count; i++)
		total += h->bins[i].count;
	is(total, 4ULL, "weight preserved after merges");

	vy_stream_histogram_delete(h);
	footer();
}

static void
test_merge_histograms(void)
{
	header();
	plan(2);
	struct vy_stream_histogram *a = vy_stream_histogram_new(20);
	struct vy_stream_histogram *b = vy_stream_histogram_new(20);
	vy_stream_histogram_update(a, 1.0);
	vy_stream_histogram_update(a, 2.0);
	vy_stream_histogram_update_weight(b, 2.0, 5);
	vy_stream_histogram_update_weight(b, 3.0, 1);
	vy_stream_histogram_merge(a, b);
	ok(fabs(vy_stream_histogram_sum(a, 100.0) - 8.0) < 1e-6,
	   "total weight after merge = 8");
	ok(a->bin_count >= 1, "non-empty");

	vy_stream_histogram_delete(a);
	vy_stream_histogram_delete(b);
	footer();
}

/**
 * 1M точек, вес 1000, max 100 бинов в гистограмме, ~10^7 на сегмент.
 * Ключ — просто номер записи (double)i; сегменты по 10000 ключей, границы
 * наращиваются через += (без * и / в этом тесте).
 */
static void
test_million_records_hundred_bins(void)
{
	header();
	plan(2 + MAX_BINS);

	struct vy_stream_histogram *h = vy_stream_histogram_new(MAX_BINS);
	fail_if(h == NULL);

	for (uint32_t i = 0; i < N_RECORDS; i++)
		vy_stream_histogram_update_weight(h, (double)i,
						    WEIGHT_PER_RECORD);

	double total_est = vy_stream_histogram_sum(h, (double)N_RECORDS);
	ok(fabs(total_est - (double)TOTAL_WEIGHT) < 1e6,
	   "sum(N_RECORDS) ~ total weight 10^9");

	is(h->bin_count, (unsigned)MAX_BINS, "merged to 100 bins");

	uint64_t lo64 = 0;
	for (unsigned k = 0; k < MAX_BINS; k++) {
		uint64_t hi64 = lo64 + SEGMENT_KEY_WIDTH;
		double c = vy_stream_histogram_sum(h, (double)hi64) -
			   vy_stream_histogram_sum(h, (double)lo64);
		ok(c >= SEGMENT_MASS_LO && c <= SEGMENT_MASS_HI,
		   "segment %u sum() mass ~10M (got %.0f)", k + 1, c);
		lo64 = hi64;
	}

	vy_stream_histogram_delete(h);
	footer();
}

static void
test_msgpack_roundtrip(void)
{
	header();
	plan(5);
	struct vy_stream_histogram *h = vy_stream_histogram_new(40);
	fail_if(h == NULL);
	vy_stream_histogram_update_weight(h, 0.0, 10);
	vy_stream_histogram_update_weight(h, 500.0, 3);
	size_t sz = vy_stream_histogram_msgpack_size(h);
	char buf[1024];
	fail_if(sz > sizeof(buf));
	char *end = vy_stream_histogram_msgpack_encode(h, buf);
	ok((size_t)(end - buf) == sz, "encode length matches sizeof");
	const char *rp = buf;
	struct vy_stream_histogram *h2 = vy_stream_histogram_msgpack_decode(&rp);
	fail_if(h2 == NULL);
	ok((size_t)(rp - buf) == sz, "decode consumes full blob");
	ok(fabs(vy_stream_histogram_sum(h2, 250.0) -
	       vy_stream_histogram_sum(h, 250.0)) < 1e-6,
	   "sum matches after round-trip");
	ok(h2->max_bin_size == h->max_bin_size && h2->bin_count == h->bin_count,
	   "bin meta preserved");
	vy_stream_histogram_delete(h);
	vy_stream_histogram_delete(h2);
	footer();
}

static void
test_max_bins_parameter_small(void)
{
	header();
	plan(2);
	const uint32_t max_bins = 32;
	struct vy_stream_histogram *h = vy_stream_histogram_new(max_bins);
	fail_if(h == NULL);
	for (uint32_t i = 0; i < 1000; i++)
		vy_stream_histogram_update_weight(h, (double)i, 1);
	ok(h->bin_count <= max_bins, "respects max_bins cap");
	double s = vy_stream_histogram_sum(h, 999.0);
	ok(fabs(s - 1000.0) < 50.0, "total mass ~1000");
	vy_stream_histogram_delete(h);
	footer();
}

int
main(void)
{
	test_new_invalid();
	test_single_bin_sum();
	test_accumulate_same_key();
	test_merge_bins_when_full();
	test_merge_histograms();
	test_million_records_hundred_bins();
	test_msgpack_roundtrip();
	test_max_bins_parameter_small();
	return 0;
}
