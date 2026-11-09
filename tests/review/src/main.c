/*
 * Hardware checks for the STM32 SAI Zephyr DMA API migration.
 *
 * SAI1_A = TX, SAI1_B = RX. Optional loopback wiring:
 *   PE5 (SCK_A) - PF8 (SCK_B), PE4 (FS_A) - PF9 (FS_B), PE6 (SD_A) - PE3 (SD_B)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>

#define RATE       44100U
#define MAX_BLOCK  1024U
#define NUM_BLOCKS 16U

/*
 * The driver does not invalidate RX blocks: on SoCs with a data cache the slabs
 * must be non-cacheable (CONFIG_NOCACHE_MEMORY=y). 32-byte aligned for cache lines.
 */
#ifdef CONFIG_NOCACHE_MEMORY
#define SLAB_SECT __nocache
#else
#define SLAB_SECT
#endif

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(tx_slab, SLAB_SECT, MAX_BLOCK, NUM_BLOCKS, 32);
K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_slab, SLAB_SECT, MAX_BLOCK, NUM_BLOCKS, 32);

static const struct device *const tx_dev = DEVICE_DT_GET(DT_NODELABEL(sai1_a));
static const struct device *const rx_dev = DEVICE_DT_GET(DT_NODELABEL(sai1_b));

static int n_pass, n_fail;

#define RESULT(ok, fmt, ...)                                                                       \
	do {                                                                                       \
		if (ok) {                                                                          \
			n_pass++;                                                                  \
		} else {                                                                           \
			n_fail++;                                                                  \
		}                                                                                  \
		printk("[%s] " fmt "\n", (ok) ? "PASS" : "FAIL", ##__VA_ARGS__);                   \
	} while (0)

/* ------------------------------------------------------------------------- */
/* Wiring probe                                                              */
/* ------------------------------------------------------------------------- */

struct port_save {
	uint32_t moder, otyper, ospeedr, pupdr, afr0, afr1;
};

static void port_save(GPIO_TypeDef *p, struct port_save *s)
{
	s->moder = p->MODER;
	s->otyper = p->OTYPER;
	s->ospeedr = p->OSPEEDR;
	s->pupdr = p->PUPDR;
	s->afr0 = p->AFR[0];
	s->afr1 = p->AFR[1];
}

static void port_restore(GPIO_TypeDef *p, const struct port_save *s)
{
	p->AFR[0] = s->afr0;
	p->AFR[1] = s->afr1;
	p->OTYPER = s->otyper;
	p->OSPEEDR = s->ospeedr;
	p->PUPDR = s->pupdr;
	p->MODER = s->moder;
}

static bool probe_pair(const struct device *po, gpio_pin_t out, const struct device *pi,
		       gpio_pin_t in)
{
	bool ok = true;

	gpio_pin_configure(po, out, GPIO_OUTPUT_LOW);
	gpio_pin_configure(pi, in, GPIO_INPUT | GPIO_PULL_DOWN);

	for (int i = 0; i < 4; i++) {
		int v = i & 1;

		gpio_pin_set_raw(po, out, v);
		k_busy_wait(20);
		if (gpio_pin_get_raw(pi, in) != v) {
			ok = false;
		}
	}

	return ok;
}

static bool wiring_probe(void)
{
	const struct device *ge = DEVICE_DT_GET(DT_NODELABEL(gpioe));
	const struct device *gf = DEVICE_DT_GET(DT_NODELABEL(gpiof));
	struct port_save se, sf;
	bool sck, fs, sd;

	port_save(GPIOE, &se);
	port_save(GPIOF, &sf);

	sck = probe_pair(ge, 5, gf, 8);
	fs = probe_pair(ge, 4, gf, 9);
	sd = probe_pair(ge, 6, ge, 3);

	port_restore(GPIOE, &se);
	port_restore(GPIOF, &sf);

	printk("Wiring: SCK PE5-PF8 %s, FS PE4-PF9 %s, SD PE6-PE3 %s\n", sck ? "yes" : "no",
	       fs ? "yes" : "no", sd ? "yes" : "no");

	return sck && fs && sd;
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static int configure(const struct device *dev, enum i2s_dir dir, uint8_t word, uint8_t ch,
		     size_t block, struct k_mem_slab *slab, uint32_t opts, int32_t timeout)
{
	struct i2s_config cfg = {
		.word_size = word,
		.channels = ch,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = opts,
		.frame_clk_freq = RATE,
		.mem_slab = slab,
		.block_size = block,
		.timeout = timeout,
	};

	return i2s_configure(dev, dir, &cfg);
}

static uint32_t sig_of(uint8_t word)
{
	return (word == 16U) ? 0x1000U : ((word == 24U) ? 0xA50000U : 0x5AA50000U);
}

static uint32_t mask_of(uint8_t word)
{
	return (word == 16U) ? 0xFFFFU : ((word == 24U) ? 0xFFFFFFU : 0xFFFFFFFFU);
}

/* Fill a TX block with an incrementing pattern, seq continues across blocks */
static void fill(void *buf, size_t size, uint8_t word, uint32_t *seq)
{
	if (word == 16U) {
		uint16_t *p = buf;

		for (size_t i = 0; i < size / 2U; i++) {
			p[i] = (uint16_t)(sig_of(word) + (*seq)++);
		}
	} else {
		uint32_t *p = buf;

		for (size_t i = 0; i < size / 4U; i++) {
			p[i] = sig_of(word) | ((*seq)++ & 0xFFFFU);
		}
	}
}

static int tx_write_new(uint8_t word, size_t size, uint32_t *seq, k_timeout_t alloc_to)
{
	void *blk;
	int ret;

	ret = k_mem_slab_alloc(&tx_slab, &blk, alloc_to);
	if (ret < 0) {
		return ret;
	}

	fill(blk, size, word, seq);

	ret = i2s_write(tx_dev, blk, size);
	if (ret < 0) {
		k_mem_slab_free(&tx_slab, blk);
	}

	return ret;
}

static bool wait_slab_full(struct k_mem_slab *slab, int ms)
{
	while (ms-- > 0) {
		if (k_mem_slab_num_free_get(slab) == NUM_BLOCKS) {
			return true;
		}
		k_msleep(1);
	}

	return k_mem_slab_num_free_get(slab) == NUM_BLOCKS;
}

static void drain_rx_queue(void)
{
	void *blk;
	size_t sz;

	/* i2s_read works in READY state until the queue is empty */
	while (i2s_read(rx_dev, &blk, &sz) == 0) {
		k_mem_slab_free(&rx_slab, blk);
	}
}

/* ------------------------------------------------------------------------- */
/* Issue 3: RX target START without external clocks                          */
/* ------------------------------------------------------------------------- */

static void test_rx_target_no_clock(void)
{
	int ret;

	ret = configure(rx_dev, I2S_DIR_RX, 16, 2, 256, &rx_slab,
			I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET, 100);
	if (ret < 0) {
		RESULT(false, "rx target configure: %d", ret);
		return;
	}

	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	RESULT(ret == 0, "RX target START with no clock present returns %d", ret);

	if (ret == 0) {
		(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	}

	RESULT(wait_slab_full(&rx_slab, 10), "RX slab fully returned after failed/dropped start");
}

/* ------------------------------------------------------------------------- */
/* Issue 1/2: TX START race with small blocks                                */
/* ------------------------------------------------------------------------- */

static void test_tx_stream(size_t block, int total, uint32_t opts)
{
	uint32_t seq = 0;
	int written = 0;
	int ret;

	ret = configure(tx_dev, I2S_DIR_TX, 16, 2, block, &tx_slab, opts, 200);
	if (ret < 0) {
		RESULT(false, "tx configure: %d", ret);
		return;
	}

	for (int i = 0; i < 2; i++) {
		ret = tx_write_new(16, block, &seq, K_MSEC(200));
		if (ret == 0) {
			written++;
		}
	}

	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		RESULT(false, "TX block %u: START %d", block, ret);
		(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		return;
	}

	while (written < total) {
		ret = tx_write_new(16, block, &seq, K_MSEC(200));
		if (ret < 0) {
			break;
		}
		written++;
	}

	if (written == total) {
		ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	} else {
		ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	}

	bool slab_ok = wait_slab_full(&tx_slab, 2000);

	RESULT(written == total && slab_ok,
	       "TX block %3u bytes %s: %d/%d blocks written, end trigger %d, slab free %u/%u",
	       block, (opts & I2S_OPT_BIT_CLK_GATED) ? "gated" : "free-running", written, total,
	       ret, k_mem_slab_num_free_get(&tx_slab), NUM_BLOCKS);

	if (!slab_ok || written != total) {
		(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
	}
}

/* ------------------------------------------------------------------------- */
/* Loopback data integrity                                                   */
/* ------------------------------------------------------------------------- */

struct checker {
	uint8_t word;
	/* Only lock on sequence 0: a stray word on an idle line cannot be taken as data */
	bool from_zero;
	bool locked;
	uint32_t expect;
	uint32_t first;
	size_t first_idx;
	size_t idx;
	uint32_t good;
	uint32_t bad;
	uint32_t first_bad_got, first_bad_exp;
};

static void check_sample(struct checker *c, uint32_t v, uint32_t total)
{
	uint32_t m = mask_of(c->word);
	uint32_t sig = sig_of(c->word);

	v &= m;

	if (!c->locked) {
		bool match = (c->word == 16U) ? (v >= sig && v < sig + total)
					      : ((v & ~0xFFFFU & m) == sig);

		if (c->from_zero) {
			match = (v == (sig & m));
		}

		if (match) {
			c->locked = true;
			c->first = (c->word == 16U) ? v - sig : (v & 0xFFFFU);
			c->first_idx = c->idx;
			c->expect = v + 1U;
			c->good = 1;
		}
	} else if (c->good + c->first < total) {
		uint32_t exp = c->expect & m;

		if (v == exp) {
			c->good++;
		} else {
			if (c->bad == 0U) {
				c->first_bad_got = v;
				c->first_bad_exp = exp;
			}
			c->bad++;
		}
		c->expect = exp + 1U;
	}

	c->idx++;
}

static void test_loopback(uint8_t word, size_t block, int nblocks)
{
	size_t ss = (word == 16U) ? 2U : 4U;
	uint32_t total = nblocks * (block / ss);
	struct checker c = {.word = word};
	uint32_t seq = 0;
	int ret, written = 0, rx_blocks = 0;

	ret = configure(tx_dev, I2S_DIR_TX, word, 2, block, &tx_slab, 0, 200);
	ret |= configure(rx_dev, I2S_DIR_RX, word, 2, block, &rx_slab,
			 I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET, 100);
	if (ret < 0) {
		RESULT(false, "loopback %u: configure", word);
		return;
	}

	/* TX controller is free-running after configure: the target sees clocks */
	k_msleep(2);

	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		RESULT(false, "loopback %u: RX target START %d", word, ret);
		return;
	}

	for (int i = 0; i < 2; i++) {
		if (tx_write_new(word, block, &seq, K_MSEC(200)) == 0) {
			written++;
		}
	}

	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		RESULT(false, "loopback %u: TX START %d", word, ret);
	}

	/* Interleave: keep TX fed, consume RX */
	for (int iter = 0; iter < nblocks + 10; iter++) {
		void *blk;
		size_t sz;

		if (written < nblocks && tx_write_new(word, block, &seq, K_NO_WAIT) == 0) {
			written++;
		}

		ret = i2s_read(rx_dev, &blk, &sz);
		if (ret < 0) {
			break;
		}
		rx_blocks++;

		for (size_t i = 0; i < sz / ss; i++) {
			uint32_t v = (ss == 2U) ? ((uint16_t *)blk)[i] : ((uint32_t *)blk)[i];

			check_sample(&c, v, total);
		}
		k_mem_slab_free(&rx_slab, blk);

		if (written >= nblocks && c.locked && c.good + c.bad + c.first >= total) {
			break;
		}
	}

	(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
	k_msleep(20);
	drain_rx_queue();
	(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);

	bool tx_ok = wait_slab_full(&tx_slab, 500);
	bool rx_ok = wait_slab_full(&rx_slab, 500);
	bool data_ok = c.locked && c.bad == 0U && c.first == 0U &&
		       (c.good == total) && ((c.first_idx % 2U) == 0U);

	RESULT(data_ok && tx_ok && rx_ok,
	       "loopback %2u-bit: written %d rx_blocks %d, first seq %u at idx %u (%s slot), "
	       "good %u/%u bad %u%s, slabs tx %u rx %u",
	       word, written, rx_blocks, c.first, c.first_idx,
	       (c.first_idx % 2U) ? "right" : "left", c.good, total, c.bad,
	       c.locked ? "" : " (pattern never seen)", k_mem_slab_num_free_get(&tx_slab),
	       k_mem_slab_num_free_get(&rx_slab));

	if (c.bad != 0U) {
		printk("       first mismatch: got 0x%08x expected 0x%08x\n", c.first_bad_got,
		       c.first_bad_exp);
	}

	/* Leave TX in READY so it can be reconfigured */
	(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
}

/*
 * RX target started before its controller provides clocks (codec started last),
 * then the gated TX controller runs: the target must receive the pattern.
 */
static void test_rx_target_before_clock(void)
{
	const size_t block = 512;
	const int nblocks = 8;
	uint32_t total = nblocks * (block / 2U);
	struct checker c = {.word = 16};
	uint32_t seq = 0;
	int ret, rx_blocks = 0;

	/* Gated controller: configure leaves the sub-block disabled, no clocks */
	ret = configure(tx_dev, I2S_DIR_TX, 16, 2, block, &tx_slab, I2S_OPT_BIT_CLK_GATED, 200);
	ret |= configure(rx_dev, I2S_DIR_RX, 16, 2, block, &rx_slab,
			 I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET, 100);
	if (ret < 0) {
		RESULT(false, "target-first: configure");
		return;
	}

	printk("  B after configure: CR1 %08x SR %08x\n", SAI1_Block_B->CR1, SAI1_Block_B->SR);

	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		RESULT(false, "target-first: RX target START before clocks returns %d", ret);
		return;
	}

	printk("  B after START:     CR1 %08x SR %08x\n", SAI1_Block_B->CR1, SAI1_Block_B->SR);

	for (int i = 0; i < nblocks; i++) {
		(void)tx_write_new(16, block, &seq, K_MSEC(200));
		if (i == 1) {
			(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
			k_busy_wait(200);
			printk("  B after TX START:  CR1 %08x SR %08x\n", SAI1_Block_B->CR1,
			       SAI1_Block_B->SR);
		}
	}
	(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);

	for (int iter = 0; iter < nblocks + 4; iter++) {
		void *blk;
		size_t sz;

		if (i2s_read(rx_dev, &blk, &sz) < 0) {
			break;
		}
		rx_blocks++;
		for (size_t i = 0; i < sz / 2U; i++) {
			check_sample(&c, ((uint16_t *)blk)[i], total);
		}
		k_mem_slab_free(&rx_slab, blk);
	}

	(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	drain_rx_queue();

	bool ok = c.locked && c.first == 0U && c.bad == 0U && c.good == total &&
		  (c.first_idx % 2U) == 0U && wait_slab_full(&rx_slab, 200) &&
		  wait_slab_full(&tx_slab, 200);

	RESULT(ok,
	       "target-first: RX target started before clocks, rx_blocks %d, first seq %u at "
	       "idx %u, good %u/%u bad %u",
	       rx_blocks, c.first, c.first_idx, c.good, total, c.bad);
}

/* ------------------------------------------------------------------------- */
/* TX target: SAI1_B is the RX clock controller, SAI1_A a TX target.          */
/* SAI1_A is always reconfigured as a target before SAI1_B becomes a         */
/* controller, so the SCK/FS wires are never driven from both ends.          */
/* ------------------------------------------------------------------------- */

#define TGT_OPTS (I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET)

static int pin_edges(uint32_t pin)
{
	uint32_t last = GPIOE->IDR & BIT(pin);
	int edges = 0;

	for (int i = 0; i < 2000; i++) {
		uint32_t v = GPIOE->IDR & BIT(pin);

		edges += (v != last);
		last = v;
	}

	return edges;
}

/* First received words of a stream, to see how the data is shifted */
static uint16_t rx_first[512];

static int sck_edges(void)
{
	uint32_t last = GPIOE->IDR & BIT(5);
	int edges = 0;

	for (int i = 0; i < 2000; i++) {
		uint32_t v = GPIOE->IDR & BIT(5);

		edges += (v != last);
		last = v;
	}

	return edges;
}

static void dump_a(const char *what)
{
	printk("    SCK edges in 2000 samples: %d\n", sck_edges());
	printk("    A %-22s CR1 %08x SR %08x (SAIEN %u FLVL %u OVRUDR %u)\n", what,
	       SAI1_Block_A->CR1, SAI1_Block_A->SR, (SAI1_Block_A->CR1 >> 16) & 1U,
	       (SAI1_Block_A->SR >> 16) & 7U, SAI1_Block_A->SR & 1U);
}

/*
 * One stream: A (TX target) sends nblocks of the pattern, B (RX controller) checks it.
 * ctrl_first: stop the RX controller (and its clocks, when gated) before the TX target.
 */
struct tt_trace {
	uint32_t us;
	int wr;
	uint32_t wr_us;
	uint32_t sr;
	uint32_t cr1;
	uint32_t ndtr;
	uint32_t dcr;
	uint32_t lisr;
};

#define TT_N 24
static struct tt_trace tt[TT_N];
static int tt_n;

static void tt_rec(int wr, uint32_t wr_us)
{
	if (tt_n < TT_N) {
		tt[tt_n++] = (struct tt_trace){
			.us = k_cyc_to_us_floor32(k_cycle_get_32()),
			.wr = wr,
			.wr_us = wr_us,
			.sr = SAI1_Block_A->SR,
			.cr1 = SAI1_Block_A->CR1,
#if defined(DMA1_Stream0)
			.ndtr = DMA1_Stream0->NDTR,
			.dcr = DMA1_Stream0->CR,
			.lisr = DMA1->LISR & 0x3DU,
#endif
		};
	}
}

static void tt_print(void)
{
	uint32_t t0 = tt[0].us;

	printk("    trace: t(us) write(ret,us) A.CR1 SAIEN DMAEN A.SR FLVL OVR | DMA1_S0 NDTR EN "
	       "LISR\n");
	for (int i = 0; i < tt_n; i++) {
		printk("    %6u %4d %6u  %08x %u %u %08x %u %u | %4u %u %02x\n", tt[i].us - t0,
		       tt[i].wr, tt[i].wr_us, tt[i].cr1, (tt[i].cr1 >> 16) & 1U,
		       (tt[i].cr1 >> 17) & 1U, tt[i].sr, (tt[i].sr >> 16) & 7U, tt[i].sr & 1U,
		       tt[i].ndtr, tt[i].dcr & 1U, tt[i].lisr);
	}
}

static bool tx_target_stream(int n, uint32_t rx_opts, bool ctrl_first)
{
	const uint8_t word = 16;
	const size_t block = 512;
	const int nblocks = 20;
	uint32_t total = nblocks * (block / 2U);
	struct checker c = {.word = word, .from_zero = true};
	uint32_t seq = 0;
	int ret, written = 0, rx_blocks = 0;

	dump_a("before START");

	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		printk("    stream %d: RX controller START %d\n", n, ret);
		return false;
	}
	k_busy_wait(500);
	/* Printing here would delay the reads: the RX queue fills in a few blocks */
	int idle_edges = sck_edges();
	uint32_t idle_cr1 = SAI1_Block_A->CR1, idle_sr = SAI1_Block_A->SR;

	for (int i = 0; i < 2; i++) {
		if (tx_write_new(word, block, &seq, K_MSEC(200)) == 0) {
			written++;
		}
	}

	tt_n = 0;
	tt_rec(99, 0);
	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		printk("    stream %d: TX target START %d\n", n, ret);
	}
	tt_rec(ret, 0);
	k_busy_wait(100);
	tt_rec(98, 0);

	for (int iter = 0; iter < 3 * nblocks; iter++) {
		void *blk;
		size_t sz;
		int wr = 97;
		uint32_t t_wr = k_cyc_to_us_floor32(k_cycle_get_32());

		/* ctrl_first keeps TX fed past the pattern, so it is still running at the stop */
		if (written < nblocks || ctrl_first) {
			wr = tx_write_new(word, block, &seq, K_NO_WAIT);
			if (wr == 0) {
				written++;
			}
		}
		tt_rec(wr, k_cyc_to_us_floor32(k_cycle_get_32()) - t_wr);

		ret = i2s_read(rx_dev, &blk, &sz);
		if (ret < 0) {
			printk("    stream %d: i2s_read %d\n", n, ret);
			break;
		}
		if (rx_blocks < 2) {
			memcpy(&rx_first[rx_blocks * 256], blk, 512);
		}
		rx_blocks++;
		for (size_t i = 0; i < sz / 2U; i++) {
			check_sample(&c, ((uint16_t *)blk)[i], total);
		}
		k_mem_slab_free(&rx_slab, blk);

		if (c.locked && c.good + c.bad + c.first >= total) {
			break;
		}
	}

	/* State at the end of the transfer, before anything is stopped */
	int end_sck = pin_edges(5), end_fs = pin_edges(4);
	uint32_t end_a_cr1 = SAI1_Block_A->CR1, end_a_sr = SAI1_Block_A->SR;
	uint32_t end_a_cr2 = SAI1_Block_A->CR2;
	uint32_t end_b_cr1 = SAI1_Block_B->CR1, end_b_sr = SAI1_Block_B->SR;

	if (ctrl_first) {
		(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
		k_msleep(2);
		dump_a("ctrl stopped");
		(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	} else {
		(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
		(void)wait_slab_full(&tx_slab, 500);
		(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	}
	drain_rx_queue();
	k_msleep(2);
	dump_a("after stop");

	bool slabs = wait_slab_full(&tx_slab, 500) && wait_slab_full(&rx_slab, 500);
	bool ok = c.locked && c.bad == 0U && c.first == 0U && c.good == total &&
		  (c.first_idx % 2U) == 0U && slabs;

	printk("    SCK edges in 2000 samples: %d\n", idle_edges);
	printk("    A %-22s CR1 %08x SR %08x (SAIEN %u FLVL %u OVRUDR %u)\n", "with clocks, idle",
	       idle_cr1, idle_sr, (idle_cr1 >> 16) & 1U, (idle_sr >> 16) & 7U, idle_sr & 1U);
	if (!ok) {
		tt_print();
		printk("    at end: A pins SCK %d FS %d edges/2000 | A CR1 %08x CR2 %08x SR %08x | "
		       "B CR1 %08x SR %08x\n",
		       end_sck, end_fs, end_a_cr1, end_a_cr2, end_a_sr, end_b_cr1, end_b_sr);
		printk("    rx words 32..127:");
		for (int i = 32; i < 128; i++) {
			printk("%s%04x", (i % 16) ? " " : "\n      ", rx_first[i]);
		}
		printk("\n");
	}

	printk("    stream %d: rx_blocks %d, first seq %u at idx %u (%s slot), good %u/%u bad %u"
	       "%s%s\n",
	       n, rx_blocks, c.first, c.first_idx, (c.first_idx % 2U) ? "right" : "left", c.good,
	       total, c.bad, c.locked ? "" : " (pattern never seen)", slabs ? "" : " SLAB LEAK");
	if (c.bad != 0U) {
		printk("    first mismatch: got 0x%04x expected 0x%04x\n", c.first_bad_got,
		       c.first_bad_exp);
	}

	/* Leave TX in READY */
	(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);

	return ok;
}

static void test_tx_target(const char *name, uint32_t rx_opts, bool ctrl_first, int nstreams)
{
	int ok = 0;
	int ret;

	printk("\n  %s\n", name);

	/* Target first: B must not drive SCK/FS while A is still a controller */
	ret = configure(tx_dev, I2S_DIR_TX, 16, 2, 512, &tx_slab, TGT_OPTS, 200);
	dump_a("after configure");
	ret |= configure(rx_dev, I2S_DIR_RX, 16, 2, 512, &rx_slab, rx_opts, 200);
	if (ret < 0) {
		RESULT(false, "%s: configure", name);
		return;
	}

	for (int n = 1; n <= nstreams; n++) {
		if (tx_target_stream(n, rx_opts, ctrl_first)) {
			ok++;
		}
	}

	RESULT(ok == nstreams, "%s: %d/%d streams intact", name, ok, nstreams);
}

static void restore_b_target(void)
{
	/* Leave B a target again, so its pins stop driving the shared clock wires */
	(void)configure(rx_dev, I2S_DIR_RX, 16, 2, 512, &rx_slab, TGT_OPTS, 100);
}

/* ------------------------------------------------------------------------- */
/* RX target STOP/DROP cycles, slab leak check                               */
/* SAI1_B is never a clock controller: with the loopback jumpers its clock   */
/* pins are wired to SAI1_A's outputs.                                       */
/* ------------------------------------------------------------------------- */

static void test_rx_cycles(uint32_t opts)
{
	int fails = 0;
	int ret;

	/* Free-running TX controller provides the clocks to the RX target */
	ret = configure(tx_dev, I2S_DIR_TX, 16, 2, 512, &tx_slab, 0, 100);
	ret |= configure(rx_dev, I2S_DIR_RX, 16, 2, 512, &rx_slab,
			 opts | I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET, 100);
	if (ret < 0) {
		RESULT(false, "rx controller configure %d", ret);
		return;
	}

	for (int cycle = 0; cycle < 10; cycle++) {
		void *blk;
		size_t sz;
		bool use_drop = (cycle & 1) != 0;

		ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
		if (ret < 0) {
			printk("  cycle %d: START %d\n", cycle, ret);
			fails++;
			(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
			continue;
		}

		for (int i = 0; i < 3; i++) {
			ret = i2s_read(rx_dev, &blk, &sz);
			if (ret < 0) {
				printk("  cycle %d: read %d\n", cycle, ret);
				fails++;
				break;
			}
			k_mem_slab_free(&rx_slab, blk);
		}

		ret = i2s_trigger(rx_dev, I2S_DIR_RX,
				  use_drop ? I2S_TRIGGER_DROP : I2S_TRIGGER_STOP);
		if (ret < 0) {
			printk("  cycle %d: %s %d\n", cycle, use_drop ? "DROP" : "STOP", ret);
			fails++;
		}

		k_msleep(15);
		drain_rx_queue();

		if (!wait_slab_full(&rx_slab, 50)) {
			printk("  cycle %d (%s): slab free %u/%u\n", cycle,
			       use_drop ? "DROP" : "STOP", k_mem_slab_num_free_get(&rx_slab),
			       NUM_BLOCKS);
			fails++;
		}
	}

	RESULT(fails == 0, "RX target: 10 START/read/STOP|DROP cycles, %d failures", fails);
}

/* TX DROP mid-stream then restart */
static void test_tx_drop_restart(void)
{
	uint32_t seq = 0;
	int fails = 0;
	int ret;

	ret = configure(tx_dev, I2S_DIR_TX, 16, 2, 1024, &tx_slab, I2S_OPT_BIT_CLK_GATED, 200);
	if (ret < 0) {
		RESULT(false, "tx configure %d", ret);
		return;
	}

	for (int cycle = 0; cycle < 10; cycle++) {
		for (int i = 0; i < 4; i++) {
			(void)tx_write_new(16, 1024, &seq, K_MSEC(200));
		}

		ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		if (ret < 0) {
			printk("  cycle %d: START %d\n", cycle, ret);
			fails++;
		}

		k_msleep(3);

		ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		if (ret < 0) {
			printk("  cycle %d: DROP %d\n", cycle, ret);
			fails++;
		}

		if (!wait_slab_full(&tx_slab, 50)) {
			printk("  cycle %d: slab free %u/%u\n", cycle,
			       k_mem_slab_num_free_get(&tx_slab), NUM_BLOCKS);
			fails++;
		}
	}

	RESULT(fails == 0, "TX gated: 10 START/DROP-mid-block cycles, %d failures", fails);
}

int main(void)
{
	bool wired;

	printk("\n=== SAI Zephyr DMA review test (%s) ===\n", CONFIG_BOARD_TARGET);

	if (!device_is_ready(tx_dev) || !device_is_ready(rx_dev)) {
		printk("SAI devices not ready\n");
		return 0;
	}

	wired = wiring_probe();

#ifdef PROBE_ONLY
	printk("\n=== DONE: probe only ===\n");
	return 0;
#endif

#ifdef TX_TARGET_ONLY
	if (!wired) {
		printk("\n=== DONE: not wired, TX target tests skipped ===\n");
		return 0;
	}

	printk("\n--- TX target, SAI1_B RX controller ---\n");
	test_tx_target("free-running controller", 0, false, 15);
	test_tx_target("gated controller", I2S_OPT_BIT_CLK_GATED, false, 15);
	test_tx_target("gated controller, stopped before the target", I2S_OPT_BIT_CLK_GATED,
		       true, 15);
	restore_b_target();

	printk("\n=== DONE: %d passed, %d failed ===\n", n_pass, n_fail);
	return 0;
#endif

	printk("\n--- Issue 3: RX target restart without clocks ---\n");
	test_rx_target_no_clock();

	printk("\n--- Issue 1/2: TX START race / reload with small blocks ---\n");
	test_tx_stream(16, 300, 0);
	test_tx_stream(32, 300, 0);
	test_tx_stream(64, 300, 0);
	test_tx_stream(16, 300, I2S_OPT_BIT_CLK_GATED);
	test_tx_stream(1024, 40, 0);
	test_tx_stream(1024, 40, I2S_OPT_BIT_CLK_GATED);

	printk("\n--- Leak / restart cycles ---\n");
	test_rx_cycles(0);
	test_tx_drop_restart();

	if (wired) {
		printk("\n--- Loopback data integrity ---\n");
		test_loopback(16, 512, 20);
		test_loopback(24, 1024, 20);
		test_loopback(32, 1024, 20);
		test_rx_target_before_clock();
	} else {
		printk("\nLoopback tests skipped: SAI1_A and SAI1_B not wired\n");
	}

	printk("\n=== DONE: %d passed, %d failed ===\n", n_pass, n_fail);

	return 0;
}
