#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "target.h"
#include "util.h"

struct targetstate {
	struct targetspec spec;
	uint8_t cur[16];
	uint64_t delayed_start;
	unsigned done : 1;
};

static void shuffle(void *buf, int stride, int n);
static int popcount(uint32_t x);
static void fill_cache(void);
static void next_addr(struct targetstate *t, uint8_t *dst);
static void count_total(const struct targetstate *t, uint64_t *total, bool *overflowed);
static void progress_single(const struct targetstate *t, uint64_t *total, uint64_t *done);


static int randomize = 1;
static int mode_streaming = 0;

static uint8_t *cache;
static int cache_i;
static int cache_size;

static FILE *targets_from;

static struct targetstate *targets;
static unsigned int targets_i, targets_size;
#define REALLOC_TARGETS() \
	realloc_if_needed((void**) &targets, sizeof(struct targetstate), targets_i, &targets_size)


int target_gen_init(void)
{
	cache = calloc(16, TARGET_RANDOMIZE_SIZE);
	if(!cache)
		abort();
	cache_i = 0;
	cache_size = 0;

	targets = NULL;
	targets_i = targets_size = 0;
	return REALLOC_TARGETS();
}

void target_gen_set_randomized(int v)
{
	randomize = !!v;
}

void target_gen_set_streaming(FILE *f)
{
	mode_streaming = f != NULL;
	targets_from = f;
}

float target_gen_progress(void)
{
	if(mode_streaming)
		return -1.0f;

	// Since we have no feedback from the scanner, we do something pretty bad:
	// We go through the bitmasks and assemble the number of hosts total and done
	// Those are added together and used to calculate the percentage (don't forget the cache though)
	// It only took two^Wthree tries to get this right!
	uint64_t total = 0, done = 0;
	for(int i = 0; i < targets_i; i++)
		progress_single(&targets[i], &total, &done);
	if (total == 0)
		return -1.0f;
	done -= cache_size - cache_i;

	return (done * 1000 / total) / 1000.0f;
}

void target_gen_fini(void)
{
	free(cache);
	free(targets);
	if(mode_streaming)
		fclose(targets_from);
}

int target_gen_add(const struct targetspec *s)
{
	if(mode_streaming)
		return -1;

	int i = targets_i++;
	if(REALLOC_TARGETS() < 0)
		return -1;

	targets[i].done = 0;
	targets[i].delayed_start = 0;
	memcpy(&targets[i].spec, s, sizeof(struct targetspec));
	memset(targets[i].cur, 0, 16);

	return 0;
}

int target_gen_finish_add(void)
{
	if(mode_streaming)
		return 0;
	if(targets_i == 0)
		return -1;

#if TARGET_EVEN_SPREAD
	// find "longest" target
	uint64_t max = 0;
	for(int i = 0; i < targets_i; i++) {
		uint64_t tmp = 0, junk = 0;
		progress_single(&targets[i], &tmp, &junk);
		if(tmp > max)
			max = tmp;
	}
	// adjust starting point of other targets
	for(int i = 0; i < targets_i; i++) {
		uint64_t tmp = 0, junk = 0;
		progress_single(&targets[i], &tmp, &junk);
		if(tmp == max)
			continue;
		assert(max > tmp);
		// set begin randomly between the first and last possible starting point
		targets[i].delayed_start = rand64() % (max - tmp + 1);
	}
#endif

	if(randomize)
		shuffle(targets, sizeof(struct targetstate), targets_i);

	log_debug("%u target(s) loaded", targets_i);
	return 0;
}

int target_gen_next(uint8_t *dst)
{
	if(cache_i == cache_size) {
		fill_cache();
		if(cache_size == 0)
			return -1;
		if(randomize)
			shuffle(cache, 16, cache_size);
	}
	memcpy(dst, &cache[cache_i*16], 16);
	cache_i++;
	return 0;
}

void target_gen_print_summary(int max_rate, int nports)
{
	if(mode_streaming) {
		printf("???\n");
		return;
	}

	uint64_t total = 0;
	bool total_overflowed = false;
	int largest = 128, smallest = 0;
	for(int i = 0; i < targets_i; i++) {
		const struct targetstate *t = &targets[i];

		count_total(t, &total, &total_overflowed);

		int maskbits = 0;
		for(int j = 0; j < 4; j++) {
			uint32_t v;
			memcpy(&v, &t->spec.mask[4*j], 4);
			maskbits += popcount(v);
		}
		if(maskbits < largest)
			largest = maskbits;
		if(maskbits > smallest)
			smallest = maskbits;
	}

	printf("%d target(s) loaded, covering ", targets_i);
	if (total_overflowed)
		printf("more than 2^64 addresses.\n");
	else
		printf("%" PRIu64 " addresses.\n", total);
	if (targets_i == 1)
		printf("Target is equivalent to a /%d subnet.\n", largest);
	else
		printf("Largest target is equivalent to /%d subnet, smallest /%d.\n", largest, smallest);

	if(max_rate != -1) {
		if (total_overflowed)
			goto over;
		assert(nports >= 1);
		uint64_t dur64 = total * (uint64_t)nports;
		if (dur64 < total)
			goto over;
		assert(max_rate >= 1);
		dur64 /= (uint64_t)max_rate;
		if (dur64 > UINT32_MAX)
			goto over;
		const uint32_t dur = dur64;

		int n1, n2;
		const char *f1, *f2;
		if(dur > 7*24*60*60) {
			n1 = dur / (7*24*60*60), n2 = dur % (7*24*60*60) / (24*60*60);
			f1 = "weeks", f2 = "days";
		} else if(dur > 24*60*60) {
			n1 = dur / (24*60*60), n2 = dur % (24*60*60) / (60*60);
			f1 = "days", f2 = "hours";
		} else if(dur > 60*60) {
			n1 = dur / (60*60), n2 = dur % (60*60) / (60);
			f1 = "hours", f2 = "minutes";
		} else {
			n1 = dur / (60), n2 = dur % (60);
			f1 = "minutes", f2 = "seconds";
		}

		if (0) {
over:
			printf("At %d PPS and %d port(s) the estimated scan duration is ", max_rate, nports);
			// might be a lie if total_overflowed (max_rate can be very large), but this is insane anyway.
			printf("more than 100 years.\n");
		} else {
			printf("At %d PPS and %d port(s) the estimated scan duration is ", max_rate, nports);
			if(n1 == 0)
				printf("%d %s.\n", n2, f2);
			else if(n2 == 0)
				printf("%d %s.\n", n1, f1);
			else
				printf("%d %s %d %s.\n", n1, f1, n2, f2);
		}
	}
}

int target_gen_sanity_check(void)
{
	uint64_t total = 0;
	bool overflowed = false;
	for(int i = 0; i < targets_i; i++) {
		const struct targetstate *t = &targets[i];
		count_total(t, &total, &overflowed);
	}

	const uint64_t limit = UINT64_C(1) << TARGET_SANITY_MAX_BITS;
	if (overflowed || total >= limit) {
		fprintf(stderr, "Error: You are trying to scan ");
		if (overflowed)
			fprintf(stderr, "more than 2^64");
		else
			fprintf(stderr, "%" PRIu64, total);
		fprintf(stderr, " addresses. Refusing.\n"
			"\n"
			"Even under ideal conditions this would take a tremendous amount of "
			"time (check with --print-summary).\nYou were probably expecting to "
			"scan an IPv6 subnet exhaustively just like you can with IPv4.\n"
			"In practice common sizes like /64 would take more than tens of "
			"thousands YEARS to enumerate.\nYou will need to rethink your approach. "
			"Good advice on IPv6 scanning can be found on the internet.\n"
			"\n"
			"In case you were hoping to scan stochastically, note that fi6s "
			"IP randomization is not suited for this.\nAs an alternative you can "
			"let an external program generate IPs and use --stream-targets.\n"
		);
		return -1;
	}
	return 0;
}

static void shuffle(void *_buf, int stride, int n)
{
	char tmp[stride], *buf = (char*) _buf;
	for(int i = n-1; i > 0; i--) {
		int j = rand() % (i+1);
		memcpy(tmp, &buf[stride * j], stride);
		memcpy(&buf[stride * j], &buf[stride * i], stride);
		memcpy(&buf[stride * i], tmp, stride);
	}
}

static int popcount(uint32_t x)
{
    int c = 0;
    for (; x; x >>= 1)
        c += x & 1;
    return c;
}

static void fill_cache(void)
{
	cache_i = 0;
	cache_size = 0;

	if(mode_streaming) {
		char buf[128];
		while(cache_size < TARGET_RANDOMIZE_SIZE) {
			if(fgets(buf, sizeof(buf), targets_from) == NULL)
				break;

			trim_string(buf, " \t\r\n");
			if(buf[0] == '#' || buf[0] == '\0')
				continue; // skip comments and empty lines

			if(parse_ipv6(buf, &cache[cache_size*16]) < 0) {
				log_error("Failed to parse target IP \"%s\".", buf);
				break;
			}
			cache_size++;
		}
		return;
	}

	while(1) {
		int any = 0;
		for(int i = 0; i < targets_i; i++) {
			if(targets[i].done)
				continue;
			if(targets[i].delayed_start > 0) {
				targets[i].delayed_start--;
				continue;
			}

			any = 1;
			next_addr(&targets[i], &cache[cache_size*16]);
			cache_size++;
			if(cache_size == TARGET_RANDOMIZE_SIZE)
				goto out;
		}
		if(!any)
			goto out;
	}
	out:
	return;
}

static void next_addr(struct targetstate *t, uint8_t *dst)
{
	int carry = 0;
	// copy what we currently have into dst
	for(int i = 0; i < 16; i++)
		dst[i] = t->spec.addr[i] | t->cur[i];
	// do manual addition on t->cur while ignoring positions set in t->spec.mask
	int any = 0;
	for(int i = 15; i >= 0; i--) {
		for(unsigned int j = 1; j != (1 << 8); j <<= 1) {
			if(t->spec.mask[i] & j)
				continue;
			any = 1;
			if(t->cur[i] & j) {
				t->cur[i] &= ~j; // unset & carry
				carry = 1;
			} else {
				t->cur[i] |= j; // set & exit
				carry = 0;
				goto out;
			}
		}
	}
	out:
	// mark target as done if there's carry left over or the mask has all bits set
	if(!any || carry == 1)
		t->done = 1;
}

static void count_total(const struct targetstate *t, uint64_t *total, bool *overflowed)
{
	uint64_t one = 0, tmp = 0;
	progress_single(t, &one, &tmp);
	// if this target is larger than 2^64 all bits will be shifted off the end
	if (one == 0) {
		*overflowed = true;
	} else {
		// add with overflow check
		tmp = *total;
		*total += one;
		if (*total < tmp)
			*overflowed = true;
	}
}

static void progress_single(const struct targetstate *t, uint64_t *total, uint64_t *done)
{
	uint64_t _total = 0, _done = 0;
	for(int i = 0; i < 16; i++) {
		for(unsigned int j = (1 << 7); j != 0; j >>= 1) {
			if(t->spec.mask[i] & j)
				continue;
			_total <<= 1;
			_total |= 1;
			_done <<= 1;
			_done |= !!(t->cur[i] & j);
		}
	}
	*total += _total + 1;
	if(t->done) // cur wraps around to zero when the target is complete
		*done += _total + 1;
	else
		*done += _done;
}
