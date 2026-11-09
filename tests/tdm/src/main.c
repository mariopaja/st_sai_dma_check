/*
 * SAI TDM loopback test: SAI1_A TX controller, SAI1_B RX target.
 * Loopback wiring: PE5-PF8 (SCK), PE4-PF9 (FS), PE6-PE3 (SD).
 *
 * Every slot carries its own sequence: a lost, duplicated or shifted slot
 * shows up as a mismatch.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>

#define RATE       44100U
#define FRAMES     64U  /* frames per block */
#define NBLOCKS    16
#define MAX_BLOCK  2048U
#define NUM_BLOCKS 8U

#ifdef CONFIG_NOCACHE_MEMORY
#define SLAB_SECT __nocache
#else
#define SLAB_SECT
#endif

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(tx_slab, SLAB_SECT, MAX_BLOCK, NUM_BLOCKS, 32);
K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_slab, SLAB_SECT, MAX_BLOCK, NUM_BLOCKS, 32);

static const struct device *const tx_dev = DEVICE_DT_GET(DT_NODELABEL(sai1_a));
static const struct device *const rx_dev = DEVICE_DT_GET(DT_NODELABEL(sai1_b));

/* mclk-divider "no-div" is the first value of the binding enum */
#define NO_DIV (DT_ENUM_IDX(DT_NODELABEL(sai1_a), mclk_divider) == 0)

static int n_pass, n_fail;
static uint32_t rate = RATE;

#define RESULT(ok, fmt, ...)                                                                       \
	do {                                                                                       \
		if (ok) {                                                                          \
			n_pass++;                                                                  \
		} else {                                                                           \
			n_fail++;                                                                  \
		}                                                                                  \
		printk("[%s] " fmt "\n", (ok) ? "PASS" : "FAIL", ##__VA_ARGS__);                   \
	} while (0)

static int configure(const struct device *dev, enum i2s_dir dir, i2s_fmt_t fmt, uint8_t word,
		     uint8_t ch, size_t block, struct k_mem_slab *slab, uint32_t opts)
{
	struct i2s_config cfg = {
		.word_size = word,
		.channels = ch,
		.format = fmt,
		.options = opts,
		.frame_clk_freq = rate,
		.mem_slab = slab,
		.block_size = block,
		.timeout = 200,
	};

	return i2s_configure(dev, dir, &cfg);
}

static uint32_t mask_of(uint8_t word)
{
	return (word == 16U) ? 0xFFFFU : ((word == 24U) ? 0xFFFFFFU : 0xFFFFFFFFU);
}

/* Marker bit, slot number and frame number: never zero, unique per slot */
static uint32_t enc(uint8_t word, uint32_t slot, uint32_t frame)
{
	switch (word) {
	case 16U:
		return 0x8000U | (slot << 11) | (frame & 0x7FFU);
	case 24U:
		return 0x800000U | (slot << 16) | (frame & 0xFFFFU);
	default:
		return 0x5A000000U | (slot << 16) | (frame & 0xFFFFU);
	}
}

static void fill(void *buf, uint8_t word, uint8_t ch, uint32_t *frame)
{
	for (uint32_t f = 0; f < FRAMES; f++, (*frame)++) {
		for (uint32_t s = 0; s < ch; s++) {
			uint32_t v = enc(word, s, *frame);

			if (word == 16U) {
				((uint16_t *)buf)[f * ch + s] = (uint16_t)v;
			} else {
				((uint32_t *)buf)[f * ch + s] = v;
			}
		}
	}
}

static int tx_write_new(uint8_t word, uint8_t ch, size_t block, uint32_t *frame,
			k_timeout_t to)
{
	void *blk;
	int ret = k_mem_slab_alloc(&tx_slab, &blk, to);

	if (ret < 0) {
		return ret;
	}

	fill(blk, word, ch, frame);

	ret = i2s_write(tx_dev, blk, block);
	if (ret < 0) {
		k_mem_slab_free(&tx_slab, blk);
	}

	return ret;
}

static bool wait_slab_full(struct k_mem_slab *slab, int ms)
{
	while (ms-- > 0 && k_mem_slab_num_free_get(slab) != NUM_BLOCKS) {
		k_msleep(1);
	}

	return k_mem_slab_num_free_get(slab) == NUM_BLOCKS;
}

static void drain_rx_queue(void)
{
	void *blk;
	size_t sz;

	while (i2s_read(rx_dev, &blk, &sz) == 0) {
		k_mem_slab_free(&rx_slab, blk);
	}
}

struct checker {
	uint8_t word, ch;
	bool locked;
	size_t idx, first_idx;
	uint32_t good, bad, total;
	uint32_t bad_got, bad_exp, bad_k;
};

static void check(struct checker *c, uint32_t v)
{
	uint32_t m = mask_of(c->word);

	v &= m;

	if (!c->locked) {
		if (v == enc(c->word, 0, 0)) {
			c->locked = true;
			c->first_idx = c->idx;
			c->good = 1;
		}
	} else if (c->good + c->bad < c->total) {
		uint32_t k = c->idx - c->first_idx;
		uint32_t exp = enc(c->word, k % c->ch, k / c->ch) & m;

		if (v == exp) {
			c->good++;
		} else {
			if (c->bad == 0U) {
				c->bad_got = v;
				c->bad_exp = exp;
				c->bad_k = k;
			}
			c->bad++;
		}
	}

	c->idx++;
}

static void test_tdm(i2s_fmt_t fmt, const char *fmt_name, uint8_t word, uint8_t ch)
{
	size_t ss = (word == 16U) ? 2U : 4U;
	size_t block = FRAMES * ch * ss;
	struct checker c = {.word = word, .ch = ch, .total = NBLOCKS * FRAMES * ch};
	uint32_t frame = 0;
	int ret, written = 0, rx_blocks = 0;

	ret = configure(tx_dev, I2S_DIR_TX, fmt, word, ch, block, &tx_slab, 0);
	if (ret == 0) {
		ret = configure(rx_dev, I2S_DIR_RX, fmt, word, ch, block, &rx_slab,
				I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET);
	}
	if (ret < 0) {
		RESULT(false, "%s %2u-bit x%2u: configure %d", fmt_name, word, ch, ret);
		return;
	}

	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		RESULT(false, "%s %2u-bit x%2u: RX START %d", fmt_name, word, ch, ret);
		return;
	}

	for (int i = 0; i < 2; i++) {
		if (tx_write_new(word, ch, block, &frame, K_MSEC(200)) == 0) {
			written++;
		}
	}

	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		printk("  TX START %d\n", ret);
	}

	for (int iter = 0; iter < NBLOCKS + 10; iter++) {
		void *blk;
		size_t sz;

		if (written < NBLOCKS &&
		    tx_write_new(word, ch, block, &frame, K_NO_WAIT) == 0) {
			written++;
		}

		if (i2s_read(rx_dev, &blk, &sz) < 0) {
			break;
		}
		rx_blocks++;

		for (size_t i = 0; i < sz / ss; i++) {
			check(&c, (ss == 2U) ? ((uint16_t *)blk)[i] : ((uint32_t *)blk)[i]);
		}
		k_mem_slab_free(&rx_slab, blk);

		if (c.locked && c.good + c.bad >= c.total) {
			break;
		}
	}

	(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	drain_rx_queue();

	bool slabs = wait_slab_full(&tx_slab, 500) && wait_slab_full(&rx_slab, 500);
	bool ok = c.locked && c.bad == 0U && c.good == c.total && (c.first_idx % ch) == 0U &&
		  slabs;

	RESULT(ok, "%s %2u-bit x%2u @%5u Hz: rx_blocks %d, slot 0 at idx %u (slot %u), good %u/%u bad %u%s",
	       fmt_name, word, ch, rate, rx_blocks, c.first_idx, c.first_idx % ch, c.good, c.total,
	       c.bad, slabs ? "" : " SLAB LEAK");
	if (c.bad != 0U) {
		printk("       first mismatch at slot %u frame %u: got 0x%08x expected 0x%08x\n",
		       c.bad_k % ch, c.bad_k / ch, c.bad_got, c.bad_exp);
	}
	if (!c.locked) {
		printk("       pattern never seen\n");
	}
}

/* The configuration must be refused */
static void test_reject(i2s_fmt_t fmt, const char *fmt_name, uint8_t word, uint8_t ch,
			const char *why)
{
	size_t ss = (word == 16U) ? 2U : 4U;
	int ret = configure(tx_dev, I2S_DIR_TX, fmt, word, ch, ss * MAX(ch, 1U), &tx_slab, 0);

	RESULT(ret == -EINVAL, "%s %2u-bit x%2u refused (%s): %d", fmt_name, word, ch, why, ret);
}

int main(void)
{
	const i2s_fmt_t pcm_s = I2S_FMT_DATA_FORMAT_PCM_SHORT;
	const i2s_fmt_t pcm_l = I2S_FMT_DATA_FORMAT_PCM_LONG;
	const i2s_fmt_t i2s = I2S_FMT_DATA_FORMAT_I2S;

	printk("\n=== SAI TDM test (%s, mclk-divider %s) ===\n", CONFIG_BOARD_TARGET,
	       NO_DIV ? "no-div" : "divided");

	if (!device_is_ready(tx_dev) || !device_is_ready(rx_dev)) {
		printk("SAI devices not ready\n");
		return 0;
	}

	printk("\n--- Two-slot formats still work ---\n");
	test_tdm(i2s, "I2S   ", 16, 2);
	test_tdm(pcm_s, "PCM-S ", 16, 2);
	test_tdm(pcm_s, "PCM-S ", 32, 1);

	printk("\n--- TDM ---\n");
	test_tdm(pcm_s, "PCM-S ", 16, 4);
	test_tdm(pcm_s, "PCM-S ", 16, 8);
	test_tdm(pcm_s, "PCM-S ", 16, 16);
	rate = 22050U;
	test_tdm(pcm_s, "PCM-S ", 16, 16);
	rate = RATE;
	test_tdm(pcm_s, "PCM-S ", 24, 4);
	test_tdm(pcm_s, "PCM-S ", 32, 4);
	test_tdm(pcm_s, "PCM-S ", 32, 8);
	test_tdm(pcm_l, "PCM-L ", 16, 8);
	test_tdm(pcm_l, "PCM-L ", 32, 8);

	if (NO_DIV) {
		printk("\n--- Frame lengths that are not a power of two (no-div) ---\n");
		test_tdm(pcm_s, "PCM-S ", 16, 6);
		test_tdm(pcm_s, "PCM-S ", 16, 12);
		test_tdm(pcm_s, "PCM-S ", 32, 3);
		test_tdm(pcm_l, "PCM-L ", 32, 6);
	}

	printk("\n--- Refused configurations ---\n");
	test_reject(i2s, "I2S   ", 16, 4, "I2S has two slots");
	test_reject(pcm_s, "PCM-S ", 16, 17, "more than 16 slots");
	test_reject(pcm_s, "PCM-S ", 32, 9, "frame over 256 bits");
	test_reject(pcm_s, "PCM-S ", 16, 0, "no channel");
	if (!NO_DIV) {
		test_reject(pcm_s, "PCM-S ", 16, 6, "96-bit frame, divided mclk");
		test_reject(pcm_s, "PCM-S ", 32, 3, "96-bit frame, divided mclk");
	}

	printk("\n=== DONE: %d passed, %d failed ===\n", n_pass, n_fail);

	return 0;
}
