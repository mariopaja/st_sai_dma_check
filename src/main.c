/*
 * Copyright (c) 2026 Mario Paja
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * STM32 SAI DMA hardware check.
 *
 * Default image: SAI1_A TX master, SAI1_B RX master (unwired). Checks pacing, trigger
 * paths, slab accounting, concurrent TX/RX and the FS/MCLK pins, for 16/24/32-bit words
 * with the bit clock gated and free-running.
 *
 * Sync image (sync overlay): the RX sub-block is a synchronous slave of the TX sub-block and
 * SD_A is wired to SD_B (pins per board, see README.md). Checks that every TX sample is
 * received unchanged, in order and on the right slot.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/sys/printk.h>
#include <soc.h>
#include <zephyr/linker/devicetree_regions.h>

#define FRAMES     64
#define NUM_BLOCKS 8
#define MAX_BLOCK  (FRAMES * 2 * 4)
#define RATE       44100
#define N_XFER     300

#define RX_NODE DT_ALIAS(i2s_rx)
#define USER    DT_PATH(zephyr_user)
#define SYNC_RX DT_PROP(RX_NODE, synchronous)

/*
 * Cache-line aligned slab buffers: in the memory region named by zephyr,user
 * slab-memory-region if set, else non-cacheable with CONFIG_NOCACHE_MEMORY.
 */
#if DT_NODE_HAS_PROP(USER, slab_memory_region)
#define SLAB_ATTR Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(DT_PHANDLE(USER, slab_memory_region)))
#elif defined(CONFIG_NOCACHE_MEMORY)
#define SLAB_ATTR __nocache
#else
#define SLAB_ATTR
#endif
static char SLAB_ATTR __aligned(32) tx_slab_buf[NUM_BLOCKS * MAX_BLOCK];
static char SLAB_ATTR __aligned(32) rx_slab_buf[NUM_BLOCKS * MAX_BLOCK];
static struct k_mem_slab tx_slab;
static struct k_mem_slab rx_slab;

static const struct device *tx_dev = DEVICE_DT_GET(DT_ALIAS(i2s_tx));
static const struct device *rx_dev = DEVICE_DT_GET(RX_NODE);

static int fails;
static int passes;

#define CHECK(cond, fmt, ...)                                                                      \
	do {                                                                                       \
		if (cond) {                                                                        \
			passes++;                                                                  \
		} else {                                                                           \
			fails++;                                                                   \
			printk("  FAIL line %d: " fmt "\n", __LINE__, ##__VA_ARGS__);              \
		}                                                                                  \
	} while (0)

static size_t block_size(uint8_t ws)
{
	return FRAMES * 2 * (ws == 16 ? 2 : 4);
}

static size_t samples_per_block(void)
{
	return FRAMES * 2;
}

static uint32_t ws_mask(uint8_t ws)
{
	return ws == 32 ? UINT32_MAX : BIT(ws) - 1U;
}

static int cfg(const struct device *dev, enum i2s_dir dir, struct k_mem_slab *slab, uint8_t ws,
	       i2s_opt_t options)
{
	struct i2s_config c = {
		.word_size = ws,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = options,
		.frame_clk_freq = RATE,
		.mem_slab = slab,
		.block_size = block_size(ws),
		.timeout = 1000,
	};

	return i2s_configure(dev, dir, &c);
}

static i2s_opt_t ctrl_opts(bool gated)
{
	return I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER |
	       (gated ? I2S_OPT_BIT_CLK_GATED : 0);
}

static bool wait_slab_full(struct k_mem_slab *slab, int ms)
{
	int64_t end = k_uptime_get() + ms;

	while (k_uptime_get() < end) {
		if (k_mem_slab_num_free_get(slab) == NUM_BLOCKS) {
			return true;
		}
		k_msleep(1);
	}
	return k_mem_slab_num_free_get(slab) == NUM_BLOCKS;
}

/* Sample n of the TX stream: a counter scrambled so that all bits toggle */
#define PATTERN_KEY 0xA5C3965AU

static uint32_t pattern(uint32_t n, uint8_t ws)
{
	return (n ^ PATTERN_KEY) & ws_mask(ws);
}

static void fill(void *b, uint8_t ws, uint32_t *n)
{
	for (size_t i = 0; i < samples_per_block(); i++) {
		if (ws == 16) {
			((uint16_t *)b)[i] = pattern((*n)++, ws);
		} else {
			((uint32_t *)b)[i] = pattern((*n)++, ws);
		}
	}
}

static __maybe_unused uint32_t sample_get(const void *b, uint8_t ws, size_t i)
{
	return ws == 16 ? ((const uint16_t *)b)[i] : ((const uint32_t *)b)[i] & ws_mask(ws);
}

static int tx_one(uint8_t ws, uint32_t *n)
{
	void *b;
	int ret = k_mem_slab_alloc(&tx_slab, &b, K_MSEC(500));

	if (ret < 0) {
		return ret;
	}
	fill(b, ws, n);
	ret = i2s_write(tx_dev, b, block_size(ws));
	if (ret < 0) {
		k_mem_slab_free(&tx_slab, b);
	}
	return ret;
}

/* TX writer thread: streams until told to stop (or a block count is reached), then drains */
static K_THREAD_STACK_DEFINE(writer_stack, 1024);
static struct k_thread writer_thread;
static volatile bool writer_stop;
static volatile int writer_err;
static volatile int writer_blocks;

static void writer(void *p1, void *p2, void *p3)
{
	uint8_t ws = (uint8_t)(uintptr_t)p1;
	int limit = (int)(uintptr_t)p2;
	uint32_t n = 0;
	int ret;

	ARG_UNUSED(p3);
	writer_err = 0;
	writer_blocks = 0;

	ret = tx_one(ws, &n);
	if (ret == 0) {
		ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	}
	while (ret == 0) {
		writer_blocks++;
		if (writer_stop || (limit > 0 && writer_blocks >= limit)) {
			break;
		}
		ret = tx_one(ws, &n);
	}
	if (ret != 0) {
		writer_err = ret;
		(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		return;
	}
	writer_err = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
}

static void writer_start(uint8_t ws, int limit)
{
	writer_stop = false;
	k_thread_create(&writer_thread, writer_stack, K_THREAD_STACK_SIZEOF(writer_stack), writer,
			(void *)(uintptr_t)ws, (void *)(uintptr_t)limit, NULL,
			K_PRIO_COOP(2), 0, K_NO_WAIT);
}

static __maybe_unused void writer_join(void)
{
	writer_stop = true;
	k_thread_join(&writer_thread, K_FOREVER);
}

static __maybe_unused bool timing_ok(int64_t dt, int blocks)
{
	int64_t expect = (int64_t)blocks * FRAMES * 1000 / RATE;

	return dt > expect * 9 / 10 && dt < expect * 11 / 10;
}

static __maybe_unused int rx_stream(uint8_t ws, int blocks, int64_t *dt)
{
	int64_t t0 = k_uptime_get();
	size_t sz;
	void *b;
	int ret;

	for (int i = 0; i < blocks; i++) {
		ret = i2s_read(rx_dev, &b, &sz);
		if (ret < 0) {
			return ret;
		}
		k_mem_slab_free(&rx_slab, b);
		if (sz != block_size(ws)) {
			return -EMSGSIZE;
		}
	}
	*dt = k_uptime_get() - t0;
	return 0;
}

static __maybe_unused void rx_drain(void)
{
	size_t sz;
	void *b;

	k_msleep(10);
	for (int i = 0; i < NUM_BLOCKS; i++) {
		if (k_mem_slab_num_free_get(&rx_slab) == NUM_BLOCKS) {
			break;
		}
		if (i2s_read(rx_dev, &b, &sz) < 0) {
			break;
		}
		k_mem_slab_free(&rx_slab, b);
	}
	CHECK(wait_slab_full(&rx_slab, 50), "slab leak after stop (%u free)",
	      k_mem_slab_num_free_get(&rx_slab));
}

static __maybe_unused void rx_stop_and_drain(void)
{
	size_t sz;
	void *b;
	int ret;

	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
	CHECK(ret == 0, "stop %d", ret);
	if (ret != 0) {
		/* Not running (e.g. error state): release everything */
		(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	}
	k_msleep(10);
	for (int i = 0; i < NUM_BLOCKS; i++) {
		if (k_mem_slab_num_free_get(&rx_slab) == NUM_BLOCKS) {
			break;
		}
		if (i2s_read(rx_dev, &b, &sz) < 0) {
			break;
		}
		k_mem_slab_free(&rx_slab, b);
	}
	CHECK(wait_slab_full(&rx_slab, 50), "slab leak after stop (%u free)",
	      k_mem_slab_num_free_get(&rx_slab));
}

#if !SYNC_RX

/* ---------- pin probes through GPIO IDR ---------- */

#define PIN_PORT(prop) ((GPIO_TypeDef *)DT_REG_ADDR(DT_GPIO_CTLR(USER, prop)))
#define PIN_MASK(prop) BIT(DT_GPIO_PIN(USER, prop))

/* Rising edges per second seen on a pin over a 5 ms window (IRQs locked) */
static uint32_t pin_freq(GPIO_TypeDef *port, uint32_t mask, uint32_t *highs, uint32_t *lows)
{
	uint32_t edges = 0, hi = 0, lo = 0;
	uint32_t window = sys_clock_hw_cycles_per_sec() / 200U;
	unsigned int key = irq_lock();
	uint32_t start = k_cycle_get_32();
	bool prev = (port->IDR & mask) != 0U;

	while ((k_cycle_get_32() - start) < window) {
		bool cur = (port->IDR & mask) != 0U;

		if (cur && !prev) {
			edges++;
		}
		if (cur) {
			hi++;
		} else {
			lo++;
		}
		prev = cur;
	}
	irq_unlock(key);

	if (highs != NULL) {
		*highs = hi;
	}
	if (lows != NULL) {
		*lows = lo;
	}
	return edges * 200U;
}

static bool freq_ok(uint32_t f)
{
	return f > RATE * 98U / 100U && f < RATE * 102U / 100U;
}

static void test_pins(uint8_t ws, bool gated)
{
	GPIO_TypeDef *fs_port = PIN_PORT(sai_fs_gpios);
	uint32_t fs = PIN_MASK(sai_fs_gpios);
	uint32_t n = 0, f, hi, lo;
	int ret;

	printk("PINS ws=%u gated=%d\n", ws, gated);
	ret = cfg(tx_dev, I2S_DIR_TX, &tx_slab, ws, ctrl_opts(gated));
	CHECK(ret == 0, "configure %d", ret);

	f = pin_freq(fs_port, fs, NULL, NULL);
	printk("  FS idle after configure: %u Hz\n", f);
	if (gated) {
		CHECK(f == 0U, "FS toggles while gated and idle (%u Hz)", f);
	} else {
		CHECK(freq_ok(f), "FS not running while free-running (%u Hz)", f);
	}

	for (int i = 0; i < 4; i++) {
		CHECK(tx_one(ws, &n) == 0, "prefill");
	}
	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	CHECK(ret == 0, "start %d", ret);
	f = pin_freq(fs_port, fs, NULL, NULL);
	printk("  FS streaming: %u Hz\n", f);
	CHECK(freq_ok(f), "FS frequency %u Hz", f);

#if DT_NODE_HAS_PROP(USER, sai_mclk_gpios)
	/* MCLK is far above the sampling rate: only count that it changes level */
	f = pin_freq(PIN_PORT(sai_mclk_gpios), PIN_MASK(sai_mclk_gpios), &hi, &lo) / 200U;
	printk("  MCLK streaming: %u edges, %u high / %u low samples\n", f, hi, lo);
	CHECK(f >= 20U, "MCLK not toggling while streaming (%u edges)", f);
#else
	ARG_UNUSED(hi);
	ARG_UNUSED(lo);
#endif

	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	CHECK(ret == 0 && wait_slab_full(&tx_slab, 100), "drop %d", ret);
	k_msleep(2);
	f = pin_freq(fs_port, fs, NULL, NULL);
	printk("  FS after drop: %u Hz\n", f);
	if (gated) {
		CHECK(f == 0U, "FS still toggling after gated stop (%u Hz)", f);
	} else {
		CHECK(freq_ok(f), "FS stopped although free-running (%u Hz)", f);
	}

#if DT_NODE_HAS_PROP(USER, sai_mclk_gpios)
	f = pin_freq(PIN_PORT(sai_mclk_gpios), PIN_MASK(sai_mclk_gpios), &hi, &lo) / 200U;
	printk("  MCLK after drop: %u edges\n", f);
#ifdef SAI_xCR1_MCKEN
	/* MCKEN: the master clock is generated independently of SAIEN */
	CHECK(f >= 20U, "MCLK stopped although MCKEN keeps it running (%u edges)", f);
#else
	if (gated) {
		CHECK(f == 0U, "MCLK still toggling after gated stop (%u edges)", f);
	} else {
		CHECK(f >= 20U, "MCLK stopped although free-running (%u edges)", f);
	}
#endif /* SAI_xCR1_MCKEN */
#endif
}

/* ---------- TX ---------- */

static void test_tx(uint8_t ws, bool gated)
{
	uint32_t n = 0;
	int64_t t0, dt;
	int ret;

	printk("TX ws=%u gated=%d\n", ws, gated);
	ret = cfg(tx_dev, I2S_DIR_TX, &tx_slab, ws, ctrl_opts(gated));
	CHECK(ret == 0, "configure %d", ret);

	/* Streaming + DRAIN, paced by the SAI frame clock */
	ret = tx_one(ws, &n);
	CHECK(ret == 0, "write %d", ret);
	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	CHECK(ret == 0, "start %d", ret);
	t0 = k_uptime_get();
	for (int i = 1; i < N_XFER; i++) {
		ret = tx_one(ws, &n);
		if (ret < 0) {
			CHECK(false, "stream write %d at block %d", ret, i);
			break;
		}
	}
	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	CHECK(ret == 0, "drain %d", ret);
	CHECK(wait_slab_full(&tx_slab, 500), "slab not returned after drain (%u free)",
	      k_mem_slab_num_free_get(&tx_slab));
	dt = k_uptime_get() - t0;
	printk("  stream %d blocks: %lld ms\n", N_XFER, dt);
	CHECK(timing_ok(dt, N_XFER), "timing off");

	/* Underrun: single block, driver goes back to READY on its own */
	ret = tx_one(ws, &n);
	ret |= i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	CHECK(ret == 0, "restart after drain %d", ret);
	CHECK(wait_slab_full(&tx_slab, 100), "slab not returned after underrun");

	/* STOP mid-stream, then DROP the leftovers */
	for (int i = 0; i < 4; i++) {
		ret = tx_one(ws, &n);
		CHECK(ret == 0, "prefill %d", ret);
	}
	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	CHECK(ret == 0, "start %d", ret);
	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);
	CHECK(ret == 0, "stop %d", ret);
	k_msleep(20);
	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	CHECK(ret == 0, "drop after stop %d", ret);
	CHECK(wait_slab_full(&tx_slab, 100), "slab leak after stop+drop (%u free)",
	      k_mem_slab_num_free_get(&tx_slab));

	/* DROP while running (aborts the in-flight DMA block) */
	for (int r = 0; r < 20; r++) {
		for (int i = 0; i < 4; i++) {
			(void)tx_one(ws, &n);
		}
		ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		CHECK(ret == 0, "start %d (round %d)", ret, r);
		k_busy_wait(300 + r * 97);
		ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		CHECK(ret == 0, "drop running %d", ret);
		CHECK(wait_slab_full(&tx_slab, 100), "slab leak after drop running r%d (%u free)",
		      r, k_mem_slab_num_free_get(&tx_slab));
	}

	/* TX must still stream after the DROP rounds */
	for (int i = 0; i < 4; i++) {
		ret = tx_one(ws, &n);
		CHECK(ret == 0, "write after drop rounds %d", ret);
	}
	t0 = k_uptime_get();
	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	ret |= i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	CHECK(ret == 0, "start/drain after drop rounds %d", ret);
	CHECK(wait_slab_full(&tx_slab, 100), "TX stalled after drop rounds (%u free)",
	      k_mem_slab_num_free_get(&tx_slab));
	dt = k_uptime_get() - t0;
	CHECK(dt >= 4 * FRAMES * 1000 / RATE - 1, "TX after drop rounds too fast: %lld ms", dt);
}

/* ---------- RX ---------- */

static void test_rx(uint8_t ws, bool gated)
{
	int64_t dt = 0;
	size_t sz;
	void *b;
	int ret;

	printk("RX ws=%u gated=%d\n", ws, gated);
	ret = cfg(rx_dev, I2S_DIR_RX, &rx_slab, ws, ctrl_opts(gated));
	CHECK(ret == 0, "configure %d", ret);

	/* Streaming + STOP */
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	CHECK(ret == 0, "start %d", ret);
	ret = rx_stream(ws, 1, &dt);
	CHECK(ret == 0, "first read %d", ret);
	ret = rx_stream(ws, N_XFER, &dt);
	CHECK(ret == 0, "stream read %d", ret);
	printk("  stream %d blocks: %lld ms\n", N_XFER, dt);
	CHECK(timing_ok(dt, N_XFER), "timing off");
	rx_stop_and_drain();

	/* DROP while running */
	for (int r = 0; r < 20; r++) {
		ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
		CHECK(ret == 0, "start %d (round %d)", ret, r);
		k_busy_wait(300 + r * 211);
		ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
		CHECK(ret == 0, "drop running %d", ret);
		CHECK(wait_slab_full(&rx_slab, 50), "slab leak after drop running r%d (%u free)",
		      r, k_mem_slab_num_free_get(&rx_slab));
	}

	/* RX must still deliver blocks after the DROP rounds */
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	CHECK(ret == 0, "start after drop rounds %d", ret);
	ret = rx_stream(ws, 4, &dt);
	CHECK(ret == 0, "read after drop rounds %d", ret);
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	CHECK(ret == 0 && wait_slab_full(&rx_slab, 50), "drop after reads %d", ret);

	/* Overrun: queue fills up -> ERROR, then PREPARE recovers */
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	CHECK(ret == 0, "start %d", ret);
	k_msleep(50);
	ret = i2s_read(rx_dev, &b, &sz);
	CHECK(ret == -EIO, "read in error state %d", ret);
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
	CHECK(ret == 0, "prepare %d", ret);
	CHECK(wait_slab_full(&rx_slab, 50), "slab leak after prepare (%u free)",
	      k_mem_slab_num_free_get(&rx_slab));
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	CHECK(ret == 0, "restart after prepare %d", ret);
	ret = rx_stream(ws, 1, &dt);
	CHECK(ret == 0, "read after prepare %d", ret);
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	CHECK(ret == 0 && wait_slab_full(&rx_slab, 50), "final drop");
}

/* ---------- TX and RX at the same time (independent masters) ---------- */

static void test_concurrent(uint8_t ws, bool gated)
{
	int64_t t0, dt_rx = 0, dt_tx;
	int ret;

	printk("TX+RX ws=%u gated=%d\n", ws, gated);
	ret = cfg(tx_dev, I2S_DIR_TX, &tx_slab, ws, ctrl_opts(gated));
	ret |= cfg(rx_dev, I2S_DIR_RX, &rx_slab, ws, ctrl_opts(gated));
	CHECK(ret == 0, "configure %d", ret);

	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	CHECK(ret == 0, "rx start %d", ret);
	t0 = k_uptime_get();
	writer_start(ws, N_XFER);
	ret = rx_stream(ws, N_XFER, &dt_rx);
	CHECK(ret == 0, "rx stream %d", ret);
	k_thread_join(&writer_thread, K_FOREVER);
	CHECK(writer_err == 0, "writer error %d after %d blocks", writer_err, writer_blocks);
	CHECK(wait_slab_full(&tx_slab, 500), "tx slab not returned (%u free)",
	      k_mem_slab_num_free_get(&tx_slab));
	dt_tx = k_uptime_get() - t0;
	rx_stop_and_drain();

	printk("  rx %d blocks: %lld ms, tx %d blocks: %lld ms\n", N_XFER, dt_rx, writer_blocks,
	       dt_tx);
	CHECK(timing_ok(dt_rx, N_XFER), "rx timing off");
	CHECK(timing_ok(dt_tx, N_XFER), "tx timing off");
}

static void run(void)
{
	static const uint8_t sizes[] = {16, 24, 32};

	for (int g = 1; g >= 0; g--) {
		ARRAY_FOR_EACH(sizes, i) {
			test_tx(sizes[i], g);
			test_rx(sizes[i], g);
		}
		test_pins(16, g);
		test_pins(32, g);
		test_concurrent(16, g);
		test_concurrent(24, g);
		test_concurrent(32, g);
	}
}

#else /* SYNC_RX */

/* ---------- loopback: SAI1_B synchronous slave of SAI1_A, SD_A wired to SD_B ---------- */

/*
 * Reads blocks and checks that the decoded counter increments by one on every sample.
 * The first samples after RX start may predate the TX data and are skipped.
 */
struct mismatch {
	int block;
	int sample;
	uint32_t got;
	uint32_t expect;
};

static void rx_verify(uint8_t ws, int blocks, const char *tag)
{
	uint32_t mask = ws_mask(ws);
	uint32_t expect = 0;
	bool locked = false;
	int skipped = 0, errors = 0, checked = 0, swapped = 0;
	struct mismatch mm[4];
	size_t sz;
	void *b;
	int ret;

	for (int i = 0; i < blocks; i++) {
		ret = i2s_read(rx_dev, &b, &sz);
		if (ret < 0) {
			CHECK(false, "%s: read %d at block %d (%d errors so far)", tag, ret, i, errors);
			(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
			return;
		}
		for (int s = 0; s < (int)samples_per_block(); s++) {
			uint32_t n = (sample_get(b, ws, s) ^ PATTERN_KEY) & mask;

			if (!locked) {
				/* Lock on two consecutive counter values */
				uint32_t next = s + 1 < (int)samples_per_block()
							? (sample_get(b, ws, s + 1) ^ PATTERN_KEY) & mask
							: n + 1U;

				if (((n + 1U) & mask) != next) {
					skipped++;
					continue;
				}
				locked = true;
				expect = n;
			}
			if (n != expect) {
				if (errors < (int)ARRAY_SIZE(mm)) {
					mm[errors] = (struct mismatch){i, s, n, expect};
				}
				errors++;
				expect = n;
			}
			/* TX counters start on the left slot: parity must match the slot */
			if (((n ^ (uint32_t)s) & 1U) != 0U) {
				swapped++;
			}
			expect = (expect + 1U) & mask;
			checked++;
		}
		k_mem_slab_free(&rx_slab, b);
	}

	/* Stop before printing so the RX queue does not overrun meanwhile */
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
	CHECK(ret == 0, "%s: stop %d", tag, ret);

	printk("  %s: %d samples checked, %d skipped before lock, %d errors, %d L/R swapped\n",
	       tag, checked, skipped, errors, swapped);
	for (int i = 0; i < MIN(errors, (int)ARRAY_SIZE(mm)); i++) {
		printk("    block %d sample %d: counter %u, expected %u (%+d)\n", mm[i].block,
		       mm[i].sample, mm[i].got, mm[i].expect, (int)(mm[i].got - mm[i].expect));
	}
	CHECK(locked && errors == 0 && swapped == 0 && skipped <= 2 * (int)samples_per_block(),
	      "%s: locked=%d errors=%d swapped=%d skipped=%d", tag, locked, errors, swapped,
	      skipped);
}

static void test_loopback(uint8_t ws)
{
	char tag[24];
	int ret;

	printk("LOOPBACK ws=%u\n", ws);
	/* The sync slave needs the master clocks running: no bit clock gating on TX */
	ret = cfg(tx_dev, I2S_DIR_TX, &tx_slab, ws, ctrl_opts(false));
	ret |= cfg(rx_dev, I2S_DIR_RX, &rx_slab, ws,
		   I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET);
	CHECK(ret == 0, "configure %d", ret);

	writer_start(ws, 0);
	k_msleep(20);

	/* RX start/stop cycles while TX keeps streaming */
	for (int r = 0; r < 3; r++) {
		snprintk(tag, sizeof(tag), "round %d", r);
		ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
		CHECK(ret == 0, "rx start %d", ret);
		rx_verify(ws, 100, tag);
		rx_drain();
	}

	/* DROP while running, then verify again */
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	CHECK(ret == 0, "rx start %d", ret);
	k_msleep(5);
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	CHECK(ret == 0 && wait_slab_full(&rx_slab, 50), "rx drop %d", ret);
	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	CHECK(ret == 0, "rx start after drop %d", ret);
	rx_verify(ws, 100, "after drop");
	rx_drain();

	writer_join();
	CHECK(writer_err == 0, "writer error %d after %d blocks", writer_err, writer_blocks);
	CHECK(wait_slab_full(&tx_slab, 500), "tx slab not returned (%u free)",
	      k_mem_slab_num_free_get(&tx_slab));
}

static void run(void)
{
	test_loopback(16);
	test_loopback(24);
	test_loopback(32);
}

#endif /* SYNC_RX */

int main(void)
{
	printk("SAI DMA hw test (%s), slabs %p/%p%s\n", SYNC_RX ? "sync loopback" : "async",
	       tx_slab_buf, rx_slab_buf, IS_ENABLED(CONFIG_NOCACHE_MEMORY) ? " (nocache)" : "");
	CHECK(k_mem_slab_init(&tx_slab, tx_slab_buf, MAX_BLOCK, NUM_BLOCKS) == 0 &&
	      k_mem_slab_init(&rx_slab, rx_slab_buf, MAX_BLOCK, NUM_BLOCKS) == 0, "slab init");
	CHECK(device_is_ready(tx_dev) && device_is_ready(rx_dev), "not ready");

	run();

	printk("RESULT: %s (%d passed, %d failed)\n", fails ? "FAIL" : "PASS", passes, fails);
	return 0;
}
