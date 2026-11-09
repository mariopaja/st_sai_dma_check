/*
 * S/PDIF loopback: SAI1_A S/PDIF output (PE6) into SPDIFRX (PD7).
 *
 * A thread streams frames with left = frame counter and right = left ^ 0xA5A5A5.
 * The receiver checks that, once locked, every frame has right == left ^ 0xA5A5A5
 * and continues the sequence. The run is done with 24-bit words (sample right
 * aligned) and with 32-bit words (sample left aligned).
 *
 * Wiring: PE6-PD7.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>

#define RATE        44100U
#define FRAMES      192U
#define BLOCK       (FRAMES * 2U * sizeof(uint32_t))
#define TX_NUM      6U
#define RX_NUM      8U
#define RX_BLOCKS   40U
#define SKIP_BLOCKS 2U
#define SAMPLE_MASK 0xFFFFFFU
#define RIGHT_XOR   0xA5A5A5U

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(tx_slab, __nocache, BLOCK, TX_NUM, 32);
K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_slab, __nocache, BLOCK, RX_NUM, 32);

static const struct device *const tx_dev = DEVICE_DT_GET(DT_NODELABEL(sai1_a));
static const struct device *const rx_dev = DEVICE_DT_GET(DT_NODELABEL(spdifrx));

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

static atomic_t tx_run;
static K_SEM_DEFINE(tx_done, 0, 1);

static int configure(const struct device *dev, enum i2s_dir dir, uint8_t word,
		     struct k_mem_slab *slab, uint32_t opts)
{
	struct i2s_config cfg = {
		.word_size = word,
		.channels = 2U,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = opts,
		.frame_clk_freq = RATE,
		.mem_slab = slab,
		.block_size = BLOCK,
		.timeout = 1000,
	};

	return i2s_configure(dev, dir, &cfg);
}

static int tx_write(uint32_t *frame)
{
	uint32_t *buf;
	int ret = k_mem_slab_alloc(&tx_slab, (void **)&buf, K_MSEC(1000));

	if (ret < 0) {
		return ret;
	}

	for (uint32_t f = 0U; f < FRAMES; f++, (*frame)++) {
		buf[2U * f] = *frame & SAMPLE_MASK;
		buf[2U * f + 1U] = (*frame ^ RIGHT_XOR) & SAMPLE_MASK;
	}

	ret = i2s_write(tx_dev, buf, BLOCK);
	if (ret < 0) {
		k_mem_slab_free(&tx_slab, buf);
	}

	return ret;
}

/* Stream S/PDIF frames until tx_run is cleared */
static void tx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		uint32_t frame = 0U;
		int ret = 0;

		k_sem_take(&tx_done, K_FOREVER);

		for (uint32_t i = 0U; i < 4U && ret == 0; i++) {
			ret = tx_write(&frame);
		}

		if (ret == 0) {
			ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		}

		while ((ret == 0) && (atomic_get(&tx_run) != 0)) {
			ret = tx_write(&frame);
		}

		if (ret < 0) {
			printk("TX stopped: %d\n", ret);
		}

		(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		atomic_set(&tx_run, 0);
		k_sem_give(&tx_done);
	}
}

K_THREAD_DEFINE(tx_tid, 2048, tx_thread, NULL, NULL, NULL, 5, 0, 0);

static void test_reject(void)
{
	struct i2s_config cfg = {
		.word_size = 16U,
		.channels = 2U,
		.frame_clk_freq = RATE,
		.mem_slab = &rx_slab,
		.block_size = BLOCK,
	};
	int ret;

	ret = i2s_configure(rx_dev, I2S_DIR_RX, &cfg);
	RESULT(ret == -EINVAL, "reject 16-bit words (%d)", ret);

	cfg.word_size = 24U;
	cfg.channels = 1U;
	ret = i2s_configure(rx_dev, I2S_DIR_RX, &cfg);
	RESULT(ret == -EINVAL, "reject 1 channel (%d)", ret);

	cfg.channels = 2U;
	cfg.block_size = BLOCK + 4U;
	ret = i2s_configure(rx_dev, I2S_DIR_RX, &cfg);
	RESULT(ret == -EINVAL, "reject a block of half a frame (%d)", ret);

	cfg.block_size = BLOCK;
	ret = i2s_configure(rx_dev, I2S_DIR_TX, &cfg);
	RESULT(ret == -ENOSYS, "reject TX direction (%d)", ret);

	ret = i2s_write(rx_dev, NULL, 0U);
	RESULT(ret == -ENOSYS, "reject i2s_write (%d)", ret);

	ret = i2s_trigger(rx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	RESULT(ret == -ENOSYS, "reject TX trigger (%d)", ret);
}

static void run(uint8_t word)
{
	uint32_t shift = (word == 32U) ? 8U : 0U;
	uint32_t frames = 0U, errors = 0U, blocks = 0U;
	uint32_t first = 0U, prev = 0U;
	bool locked = false;
	int ret;

	printk("\n--- %u-bit words ---\n", word);

	ret = configure(tx_dev, I2S_DIR_TX, 24U, &tx_slab, I2S_OPT_BIT_CLK_GATED);
	RESULT(ret == 0, "configure S/PDIF TX (%d)", ret);
	ret = configure(rx_dev, I2S_DIR_RX, word, &rx_slab, 0U);
	RESULT(ret == 0, "configure SPDIFRX (%d)", ret);
	if (ret < 0) {
		return;
	}

	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	RESULT(ret == 0, "SPDIFRX START (%d)", ret);
	if (ret < 0) {
		return;
	}

	atomic_set(&tx_run, 1);
	k_sem_give(&tx_done);

	for (uint32_t b = 0U; b < RX_BLOCKS; b++) {
		uint32_t *buf;
		size_t size;

		ret = i2s_read(rx_dev, (void **)&buf, &size);
		if (ret < 0) {
			printk("RX read %u: %d\n", b, ret);
			break;
		}
		blocks++;

		/* The first blocks may hold the synchronization: skip them */
		for (uint32_t f = 0U; (b >= SKIP_BLOCKS) && (f < size / 8U); f++) {
			uint32_t l = buf[2U * f];
			uint32_t r = buf[2U * f + 1U];
			uint32_t n = l >> shift;
			bool ok = (l == (n << shift)) && (n <= SAMPLE_MASK) &&
				  (r == (((n ^ RIGHT_XOR) & SAMPLE_MASK) << shift));

			if (locked && ok && (n != ((prev + 1U) & SAMPLE_MASK))) {
				ok = false;
			}

			if (!ok && (errors++ < 8U)) {
				printk("  block %u frame %u: L %08x R %08x (prev %06x)\n", b, f, l,
				       r, prev);
			}

			if (!locked) {
				first = n;
			}
			locked = true;
			prev = n;
			frames++;
		}

		k_mem_slab_free(&rx_slab, buf);
	}

	(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	atomic_set(&tx_run, 0);
	k_sem_take(&tx_done, K_FOREVER);

	printk("received %u blocks, checked %u frames, counter %u to %u\n", blocks, frames,
	       first, prev);
	RESULT(blocks == RX_BLOCKS, "all blocks received (%u)", blocks);
	RESULT(frames == (RX_BLOCKS - SKIP_BLOCKS) * FRAMES, "frames checked (%u)", frames);
	RESULT(errors == 0U, "data and sequence errors (%u)", errors);
	RESULT(k_mem_slab_num_free_get(&rx_slab) == RX_NUM, "RX blocks released");
}

int main(void)
{
	printk("SPDIFRX loopback (wiring PE6-PD7)\n");

	if (!device_is_ready(tx_dev) || !device_is_ready(rx_dev)) {
		printk("devices not ready\n");
		return 0;
	}

	test_reject();
	run(24U);
	run(32U);

	printk("\n==== %d passed, %d failed ====\n=== DONE\n", n_pass, n_fail);

	return 0;
}
