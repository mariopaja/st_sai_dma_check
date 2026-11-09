/*
 * Stress test for the STM32 SAI driver on the Zephyr DMA API.
 *
 * SAI1_A = TX controller, SAI1_B = RX target, looped back with jumpers:
 *   PE5 (SCK_A) - PF8 (SCK_B), PE4 (FS_A) - PF9 (FS_B), PE6 (SD_A) - PE3 (SD_B)
 *
 * Phase 1: soak, continuous full-duplex stream with interrupt and thread load
 * Phase 2: random configurations and trigger sequences
 * Phase 3: error recovery (RX overrun -> PREPARE, TX underrun -> restart)
 *
 * Build with -DEXTRA_CFLAGS=-DDIAG_FLUSH_TX_FIFO to flush the TX FIFO after each
 * configure: isolates the words a gated TX stop leaves in the FIFO.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/linker/devicetree_regions.h>
#include <string.h>

#define RATE        44100U
#define MAX_BLOCK   1024U
#define NUM_BLOCKS  16U
#define SOAK_S      30
#define CYCLES      200
#define RECOVERIES  20

/*
 * The driver does not invalidate RX blocks: on SoCs with a data cache the slabs
 * must be non-cacheable (CONFIG_NOCACHE_MEMORY=y). 32-byte aligned for cache lines.
 */
/* SAI sub-blocks: aliases i2s-tx / i2s-rx if set (e.g. SAI4 on BDMA), else SAI1 */
#if DT_NODE_EXISTS(DT_ALIAS(i2s_tx))
#define TX_NODE DT_ALIAS(i2s_tx)
#define RX_NODE DT_ALIAS(i2s_rx)
#else
#define TX_NODE DT_NODELABEL(sai1_a)
#define RX_NODE DT_NODELABEL(sai1_b)
#endif

#define USER DT_PATH(zephyr_user)

/* BDMA only reaches its own SRAM: zephyr,user slab-memory-region names it */
#if DT_NODE_HAS_PROP(USER, slab_memory_region)
#define SLAB_SECT                                                                                  \
	Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(DT_PHANDLE(USER, slab_memory_region)))
#elif defined(CONFIG_NOCACHE_MEMORY)
#define SLAB_SECT __nocache
#else
#define SLAB_SECT
#endif

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(tx_slab, SLAB_SECT, MAX_BLOCK, NUM_BLOCKS, 32);
K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_slab, SLAB_SECT, MAX_BLOCK, NUM_BLOCKS, 32);

static const struct device *const tx_dev = DEVICE_DT_GET(TX_NODE);
static const struct device *const rx_dev = DEVICE_DT_GET(RX_NODE);

#define TX_BLOCK ((SAI_Block_TypeDef *)DT_REG_ADDR(TX_NODE))
#define RX_BLOCK ((SAI_Block_TypeDef *)DT_REG_ADDR(RX_NODE))

#define TARGET (I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET)

#include <soc.h>
#define FLVL(b) ((unsigned int)(((b)->SR >> 16) & 7U))
#define EN(b)   ((unsigned int)(((b)->CR1 >> 16) & 1U))
static char fifo_log[160];
static void fifo_note(const char *tag)
{
	size_t l = strlen(fifo_log);

	snprintk(fifo_log + l, sizeof(fifo_log) - l, " %s A%u/%u B%u/%u", tag,
		 EN(TX_BLOCK), FLVL(TX_BLOCK), EN(RX_BLOCK), FLVL(RX_BLOCK));
}

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
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static uint32_t rnd_state = 0x5A1D3A11U;

static uint32_t rnd(void)
{
	rnd_state ^= rnd_state << 13;
	rnd_state ^= rnd_state >> 17;
	rnd_state ^= rnd_state << 5;
	return rnd_state;
}

static uint32_t rnd_range(uint32_t lo, uint32_t hi)
{
	return lo + (rnd() % (hi - lo + 1U));
}

static uint32_t mask_of(uint8_t word)
{
	return (word == 16U) ? 0xFFFFU : ((word == 24U) ? 0xFFFFFFU : 0xFFFFFFFFU);
}

/* Pattern: 1, 2, ..., mask, 1, ... (0 is reserved for silence) */
static uint32_t next_val(uint32_t v, uint8_t word)
{
	v = (v + 1U) & mask_of(word);
	return (v == 0U) ? 1U : v;
}

static int configure(const struct device *dev, enum i2s_dir dir, uint8_t word, size_t block,
		     struct k_mem_slab *slab, uint32_t opts, int32_t timeout)
{
	struct i2s_config cfg = {
		.word_size = word,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = opts,
		.frame_clk_freq = RATE,
		.mem_slab = slab,
		.block_size = block,
		.timeout = timeout,
	};

	return i2s_configure(dev, dir, &cfg);
}

struct tx_gen {
	uint8_t word;
	uint32_t val;
};

static int tx_write_new(struct tx_gen *g, size_t size, k_timeout_t alloc_to)
{
	void *blk;
	int ret;

	ret = k_mem_slab_alloc(&tx_slab, &blk, alloc_to);
	if (ret < 0) {
		return ret;
	}

	if (g->word == 16U) {
		uint16_t *p = blk;

		for (size_t i = 0; i < size / 2U; i++) {
			g->val = next_val(g->val, g->word);
			p[i] = (uint16_t)g->val;
		}
	} else {
		uint32_t *p = blk;

		for (size_t i = 0; i < size / 4U; i++) {
			g->val = next_val(g->val, g->word);
			p[i] = g->val;
		}
	}

	ret = i2s_write(tx_dev, blk, size);
	if (ret < 0) {
		k_mem_slab_free(&tx_slab, blk);
	}

	return ret;
}

struct rx_check {
	uint8_t word;
	bool locked;
	bool left_ok;
	uint32_t expect;
	uint32_t idx;
	uint32_t good;
	uint32_t bad;
	uint32_t silence;
	uint32_t first_bad_got, first_bad_exp;
	uint32_t lock_val, lock_idx, bad_idx;
};

static void rx_check_block(struct rx_check *c, const void *blk, size_t size)
{
	size_t ss = (c->word == 16U) ? 2U : 4U;
	uint32_t m = mask_of(c->word);

	for (size_t i = 0; i < size / ss; i++, c->idx++) {
		uint32_t v = (ss == 2U) ? ((const uint16_t *)blk)[i] : ((const uint32_t *)blk)[i];

		v &= m;

		if (v == 0U) {
			if (c->locked) {
				c->silence++;
			}
			continue;
		}

		if (!c->locked) {
			c->locked = true;
			c->lock_val = v;
			c->lock_idx = c->idx;
			/* Odd values go to the left slot: pattern starts at 1 */
			c->left_ok = ((c->idx & 1U) == ((v & 1U) ? 0U : 1U));
			c->expect = next_val(v, c->word);
			c->good++;
			continue;
		}

		if (v == c->expect) {
			c->good++;
		} else {
			if (c->bad == 0U) {
				c->first_bad_got = v;
				c->first_bad_exp = c->expect;
				c->bad_idx = c->idx;
			}
			c->bad++;
		}
		c->expect = next_val(v, c->word);
	}
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

/* Read and free whatever the RX queue still holds */
static int rx_flush(struct rx_check *c)
{
	void *blk;
	size_t sz;
	int n = 0;

	while (i2s_read(rx_dev, &blk, &sz) == 0) {
		if (c != NULL) {
			rx_check_block(c, blk, sz);
		}
		k_mem_slab_free(&rx_slab, blk);
		n++;
	}

	return n;
}

/* Bring both directions back to READY with empty queues */
static void reset_all(void)
{
	(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
	(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
	rx_flush(NULL);
}

/* ------------------------------------------------------------------------- */
/* Phase 1: soak                                                             */
/* ------------------------------------------------------------------------- */

#define SOAK_BLOCK 1024U

static volatile bool soak_run;
static volatile uint32_t soak_tx_blocks, soak_tx_fail, timer_ticks, spike_count;
static struct tx_gen soak_gen;

K_THREAD_STACK_DEFINE(feeder_stack, 2048);
K_THREAD_STACK_DEFINE(spike_stack, 1024);
K_THREAD_STACK_DEFINE(busy_stack, 1024);
static struct k_thread feeder_thread, spike_thread, busy_thread;

static void feeder_fn(void *a, void *b, void *c)
{
	while (soak_run) {
		if (tx_write_new(&soak_gen, SOAK_BLOCK, K_MSEC(100)) == 0) {
			soak_tx_blocks++;
		} else {
			soak_tx_fail++;
		}
	}
}

/* Highest priority: burns the CPU for 2 ms every ~25 ms */
static void spike_fn(void *a, void *b, void *c)
{
	while (soak_run) {
		k_msleep(rnd_range(10, 40));
		k_busy_wait(2000);
		spike_count++;
	}
}

/* Lowest priority: keeps the CPU busy in the background */
static void busy_fn(void *a, void *b, void *c)
{
	volatile uint32_t x = 0;

	while (soak_run) {
		x++;
	}
}

static void tick_fn(struct k_timer *t)
{
	timer_ticks++;
	k_busy_wait(5);
}

K_TIMER_DEFINE(load_timer, tick_fn, NULL);

static void phase_soak(void)
{
	struct rx_check c = {.word = 16};
	uint32_t rx_blocks = 0, rx_err = 0;
	int64_t end;
	int ret;

	printk("\n--- Phase 1: %d s soak, 16-bit stereo, %u-byte blocks, load on ---\n", SOAK_S,
	       SOAK_BLOCK);

	ret = configure(tx_dev, I2S_DIR_TX, 16, SOAK_BLOCK, &tx_slab, 0, 200);
	ret |= configure(rx_dev, I2S_DIR_RX, 16, SOAK_BLOCK, &rx_slab, TARGET, 200);
	if (ret < 0) {
		RESULT(false, "soak: configure");
		return;
	}

	soak_gen = (struct tx_gen){.word = 16};
	/* The driver queue holds CONFIG_I2S_STM32_SAI_BLOCK_COUNT (4) blocks */
	for (int i = 0; i < 2; i++) {
		(void)tx_write_new(&soak_gen, SOAK_BLOCK, K_MSEC(100));
	}

	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		RESULT(false, "soak: TX START %d", ret);
		reset_all();
		return;
	}

	/* Start the target only now: unread blocks of silence would overrun it */
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		RESULT(false, "soak: RX START %d", ret);
		reset_all();
		return;
	}

	soak_run = true;
	k_thread_create(&feeder_thread, feeder_stack, K_THREAD_STACK_SIZEOF(feeder_stack),
			feeder_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(3), 0, K_NO_WAIT);
	k_thread_create(&spike_thread, spike_stack, K_THREAD_STACK_SIZEOF(spike_stack), spike_fn,
			NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	k_thread_create(&busy_thread, busy_stack, K_THREAD_STACK_SIZEOF(busy_stack), busy_fn,
			NULL, NULL, NULL, K_PRIO_PREEMPT(14), 0, K_NO_WAIT);
	k_timer_start(&load_timer, K_USEC(50), K_USEC(50));

	end = k_uptime_get() + SOAK_S * 1000;
	int64_t next_report = k_uptime_get() + 5000;

	while (k_uptime_get() < end) {
		void *blk;
		size_t sz;

		ret = i2s_read(rx_dev, &blk, &sz);
		if (ret < 0) {
			rx_err++;
			printk("  soak: RX read %d after %u blocks\n", ret, rx_blocks);
			break;
		}
		rx_check_block(&c, blk, sz);
		k_mem_slab_free(&rx_slab, blk);
		rx_blocks++;

		if (k_uptime_get() >= next_report) {
			next_report += 5000;
			printk("  %3lld s: tx %u rx %u good %u bad %u silence %u spikes %u\n",
			       k_uptime_get() / 1000, soak_tx_blocks, rx_blocks, c.good, c.bad,
			       c.silence, spike_count);
		}
	}

	soak_run = false;
	k_timer_stop(&load_timer);
	k_thread_join(&feeder_thread, K_SECONDS(1));
	k_thread_join(&spike_thread, K_SECONDS(1));
	k_thread_join(&busy_thread, K_SECONDS(1));

	(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	k_msleep(150);
	(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
	k_msleep(50);
	rx_flush(NULL);
	reset_all();

	bool slabs = wait_slab_full(&tx_slab, 200) && wait_slab_full(&rx_slab, 200);
	uint32_t expect_samples = rx_blocks * (SOAK_BLOCK / 2U);

	RESULT(rx_err == 0U && c.bad == 0U && c.silence == 0U && c.left_ok && slabs &&
		       soak_tx_fail == 0U && c.good + 8U >= expect_samples,
	       "soak: rx %u blocks, %u samples good, %u bad, %u silence, left slot %s, "
	       "tx %u blocks (%u write failures), timer ticks %u, slabs %s",
	       rx_blocks, c.good, c.bad, c.silence, c.left_ok ? "yes" : "NO", soak_tx_blocks,
	       soak_tx_fail, timer_ticks, slabs ? "ok" : "LEAK");

	if (c.bad != 0U) {
		printk("       first mismatch: got 0x%x expected 0x%x\n", c.first_bad_got,
		       c.first_bad_exp);
	}
}

/* ------------------------------------------------------------------------- */
/* Phase 2: random configurations and trigger sequences                      */
/* ------------------------------------------------------------------------- */

static const char *const tx_end_name[] = {"DRAIN", "STOP", "DROP"};
static const char *const rx_end_name[] = {"STOP", "DROP"};

static uint32_t prev_last_tx;
static int short_block_underruns;

static bool one_cycle(int n)
{
	static const uint8_t words[] = {16, 24, 32};
	uint8_t word = words[rnd_range(0, 2)];
	size_t block = rnd_range(1, 64) * 16U;
	bool gated = (rnd() & 1U) != 0U;
	int run = (int)rnd_range(0, 12);
	int tx_end = (int)rnd_range(0, 2);
	int rx_end = (int)rnd_range(0, 1);
	struct tx_gen g = {.word = word};
	struct rx_check c = {.word = word};
	const char *why = NULL;
	int ret, rx_blocks = 0, wret = 0;
	/* Keep busy periods below half a block so TX never runs dry */
	uint32_t block_us = (uint32_t)((uint64_t)block * 1000000U / ((word == 16U ? 4U : 8U) * RATE));

	ret = configure(tx_dev, I2S_DIR_TX, word, block, &tx_slab,
			gated ? I2S_OPT_BIT_CLK_GATED : 0, 100);
	if (ret < 0) {
		why = "TX configure";
		goto out;
	}

	ret = configure(rx_dev, I2S_DIR_RX, word, block, &rx_slab, TARGET, 30);
	if (ret < 0) {
		why = "RX configure";
		goto out;
	}

#ifdef DIAG_FLUSH_TX_FIFO
	/* Diagnostic: flush what a previous stop left in the TX FIFO */
	TX_BLOCK->CR2 |= SAI_xCR2_FFLUSH;
#endif
	fifo_log[0] = '\0';
	fifo_note("cfg");

	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		why = "RX START";
		goto out;
	}
	fifo_note("rxstart");

	for (int i = 0; i < 2; i++) {
		(void)tx_write_new(&g, block, K_MSEC(100));
	}

	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		why = "TX START";
		goto out;
	}

	for (int i = 0; i < run; i++) {
		void *blk;
		size_t sz;

		wret = tx_write_new(&g, block, K_MSEC(100));
		if (wret == -EAGAIN && block_us < 500U) {
			/* A few queued blocks of < 0.5 ms each cannot absorb scheduling jitter:
			 * TX runs dry and returns to READY, as designed.
			 */
			short_block_underruns++;
			run = 0;
			break;
		}
		if (wret < 0) {
			why = "TX write";
			goto out;
		}

		if (i2s_read(rx_dev, &blk, &sz) == 0) {
			rx_check_block(&c, blk, sz);
			k_mem_slab_free(&rx_slab, blk);
			rx_blocks++;
		}

		if ((rnd() & 3U) == 0U) {
			k_busy_wait(rnd_range(0, block_us / 2U));
		}
	}

	/* TX end: STOP/DRAIN return -EIO when TX has already run out of data */
	ret = i2s_trigger(tx_dev, I2S_DIR_TX,
			  (tx_end == 0) ? I2S_TRIGGER_DRAIN
					: ((tx_end == 1) ? I2S_TRIGGER_STOP : I2S_TRIGGER_DROP));
	if (ret < 0 && ret != -EIO) {
		why = "TX end trigger";
		goto out;
	}

	k_msleep(rnd_range(0, 30));

	ret = i2s_trigger(rx_dev, I2S_DIR_RX,
			  (rx_end == 0) ? I2S_TRIGGER_STOP : I2S_TRIGGER_DROP);
	if (ret < 0 && ret != -EIO) {
		why = "RX end trigger";
		goto out;
	}

	k_msleep(40);
	rx_blocks += rx_flush(&c);

out:
	reset_all();
	fifo_note("end");

	bool slabs = wait_slab_full(&tx_slab, 200) && wait_slab_full(&rx_slab, 200);

	if (why == NULL && !slabs) {
		why = "slab leak";
	}
	if (why == NULL && c.bad != 0U) {
		why = "data mismatch";
	}
	if (why == NULL && c.locked && !c.left_ok) {
		why = "slot misalignment";
	}
	if (why == NULL && run >= 2 && !c.locked) {
		why = "no data received";
	}

	if (why != NULL) {
		printk("  cycle %3d FAIL (%s, ret %d): %u-bit block %u %s run %d TX %s RX %s, "
		       "rx %d good %u bad %u, slabs tx %u rx %u\n",
		       n, why, ret, word, block, gated ? "gated" : "free", run,
		       tx_end_name[tx_end], rx_end_name[rx_end], rx_blocks, c.good, c.bad,
		       k_mem_slab_num_free_get(&tx_slab), k_mem_slab_num_free_get(&rx_slab));
		printk("            write %d, lock 0x%x at idx %u, first bad got 0x%x exp 0x%x at "
		       "idx %u, prev cycle last tx 0x%x\n",
		       wret, c.lock_val, c.lock_idx, c.first_bad_got, c.first_bad_exp, c.bad_idx,
		       prev_last_tx);
	}

	if (why != NULL) {
		printk("  cycle %3d %s %s, en/flvl:%s\n", n, why ? "FAIL" : "ok  ",
		       gated ? "gated" : "free ", fifo_log);
	}

	prev_last_tx = g.val;

	return why == NULL;
}

static void phase_cycles(void)
{
	int fails = 0;
	int64_t t0 = k_uptime_get();

	printk("\n--- Phase 2: %d random config/trigger cycles (seed 0x%08x) ---\n", CYCLES,
	       rnd_state);

	for (int n = 0; n < CYCLES; n++) {
		if (!one_cycle(n)) {
			fails++;
		}
		if ((n + 1) % 50 == 0) {
			printk("  %d cycles, %d failures\n", n + 1, fails);
		}
	}

	RESULT(fails == 0,
	       "random cycles: %d/%d passed in %lld ms (%d expected underruns on short blocks)",
	       CYCLES - fails, CYCLES, k_uptime_get() - t0, short_block_underruns);
}

/* ------------------------------------------------------------------------- */
/* Phase 3: error recovery                                                   */
/* ------------------------------------------------------------------------- */

static void phase_recovery(void)
{
	int fails = 0;
	int ret;

	printk("\n--- Phase 3: error recovery, %d rounds each ---\n", RECOVERIES);

	/*
	 * RX overrun: nobody reads, the queue fills, the stream errors. SAI1_B stays a
	 * target clocked by a free-running SAI1_A: with the loopback jumpers, SAI1_B as a
	 * clock controller would drive the same wires as SAI1_A.
	 */
	ret = configure(tx_dev, I2S_DIR_TX, 16, 256, &tx_slab, 0, 100);
	ret |= configure(rx_dev, I2S_DIR_RX, 16, 256, &rx_slab, TARGET, 20);
	for (int i = 0; ret == 0 && i < RECOVERIES; i++) {
		void *blk;
		size_t sz;
		int r;

		r = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
		k_msleep(60);
		/* Queue overflow puts the stream in ERROR: read must fail */
		while ((r = i2s_read(rx_dev, &blk, &sz)) == 0) {
			k_mem_slab_free(&rx_slab, blk);
		}
		bool err_seen = (r == -EIO);

		r = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
		bool slabs = wait_slab_full(&rx_slab, 50);

		/* Normal operation must resume */
		int ok_reads = 0;

		if (i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START) == 0) {
			for (int k = 0; k < 3; k++) {
				if (i2s_read(rx_dev, &blk, &sz) == 0) {
					k_mem_slab_free(&rx_slab, blk);
					ok_reads++;
				}
			}
		}
		(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
		rx_flush(NULL);

		if (!err_seen || r != 0 || !slabs || ok_reads != 3 ||
		    !wait_slab_full(&rx_slab, 50)) {
			printk("  RX round %d: error seen %d, PREPARE %d, slabs %d, reads %d\n", i,
			       err_seen, r, slabs, ok_reads);
			fails++;
		}
	}
	RESULT(ret == 0 && fails == 0, "RX overrun -> ERROR -> PREPARE -> restart: %d failures",
	       fails);

	/* TX underrun: data runs out, the stream returns to READY on its own */
	fails = 0;
	ret = configure(tx_dev, I2S_DIR_TX, 16, 512, &tx_slab, 0, 100);
	for (int i = 0; ret == 0 && i < RECOVERIES; i++) {
		struct tx_gen g = {.word = 16};
		int r;

		for (int k = 0; k < 2; k++) {
			(void)tx_write_new(&g, 512, K_MSEC(100));
		}
		r = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		k_msleep(20);
		bool slabs = wait_slab_full(&tx_slab, 50);

		/* Stream is READY again: a new START must work */
		(void)tx_write_new(&g, 512, K_MSEC(100));
		int r2 = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);

		k_msleep(15);

		if (r != 0 || r2 != 0 || !slabs || !wait_slab_full(&tx_slab, 50)) {
			printk("  TX round %d: START %d, restart %d, slabs %d\n", i, r, r2, slabs);
			fails++;
		}
		(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	}
	RESULT(ret == 0 && fails == 0, "TX underrun -> READY -> restart: %d failures", fails);
}

int main(void)
{
	printk("\n=== SAI DMA stress test (%s) ===\n", CONFIG_BOARD_TARGET);

	if (!device_is_ready(tx_dev) || !device_is_ready(rx_dev)) {
		printk("SAI devices not ready\n");
		return 0;
	}

	phase_soak();
	phase_cycles();
	phase_recovery();

	printk("\n=== DONE: %d passed, %d failed ===\n", n_pass, n_fail);

	return 0;
}
