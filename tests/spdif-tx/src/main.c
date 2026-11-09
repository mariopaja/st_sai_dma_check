/*
 * SAI S/PDIF TX self-check.
 *
 * SAI1_A sends an 8 kHz S/PDIF stream on PE6. SAI1_B, an RX controller at
 * 192 kHz with 32-bit stereo frames (15 MHz bit clock), samples the line on
 * PE3, about 15 samples per S/PDIF half-cell. The capture is decoded in
 * software (biphase-mark) and every frame is checked:
 * - preamble B/M/W and even parity
 * - left = frame counter, right = left ^ 0xA5A5A5
 * - V = U = 0, C = channel status bit of the frame
 * - B preamble exactly on frames that are a multiple of 192
 *
 * Wiring: PE6-PE3.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>

#define TX_RATE    8000U
#define RX_RATE    192000U
#define TX_FRAMES  384U
#define TX_BLOCK   (TX_FRAMES * 2U * sizeof(uint32_t))
#define TX_NUM     6U
#define TX_QUEUED  4U /* CONFIG_I2S_STM32_SAI_BLOCK_COUNT */
#define RX_BLOCK   4096U
#define RX_NUM     8U
#define CAP_BLOCKS 48U
#define CAP_WORDS  (CAP_BLOCKS * RX_BLOCK / sizeof(uint32_t))
#define CAP_BITS   (CAP_WORDS * 32U)

#define SPDIF_BLOCK_FRAMES 192U
#define SAMPLE_MASK        0xFFFFFFU
#define RIGHT_XOR          0xA5A5A5U
#define SPDIF_V            BIT(24)
#define SPDIF_U            BIT(25)
#define SPDIF_C            BIT(26)

#ifdef CONFIG_NOCACHE_MEMORY
#define SLAB_SECT __nocache
#else
#define SLAB_SECT
#endif

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(tx_slab, SLAB_SECT, TX_BLOCK, TX_NUM, 32);
K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_slab, SLAB_SECT, RX_BLOCK, RX_NUM, 32);

static const struct device *const tx_dev = DEVICE_DT_GET(DT_NODELABEL(sai1_a));
static const struct device *const rx_dev = DEVICE_DT_GET(DT_NODELABEL(sai1_b));

static uint32_t cap[CAP_WORDS];

/*
 * Consumer channel status, transmitted LSB first:
 * byte 0: PCM, copy permitted; byte 3: sampling frequency not indicated;
 * byte 4: 24-bit maximum word length, 24 bits used.
 */
static const uint8_t chan_status[SPDIF_BLOCK_FRAMES / 8U] = {
	[0] = 0x04U,
	[3] = 0x01U,
	[4] = 0x0BU,
};

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

static uint32_t cbit(uint32_t frame)
{
	uint32_t k = frame % SPDIF_BLOCK_FRAMES;

	return (chan_status[k / 8U] >> (k % 8U)) & 1U;
}

static int configure(const struct device *dev, enum i2s_dir dir, uint8_t word, uint8_t ch,
		     uint32_t rate, size_t block, struct k_mem_slab *slab, uint32_t opts)
{
	struct i2s_config cfg = {
		.word_size = word,
		.channels = ch,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = opts,
		.frame_clk_freq = rate,
		.mem_slab = slab,
		.block_size = block,
		.timeout = 1000,
	};

	return i2s_configure(dev, dir, &cfg);
}

static void tx_fill(uint32_t *buf, uint32_t *frame)
{
	for (uint32_t f = 0U; f < TX_FRAMES; f++, (*frame)++) {
		uint32_t c = (cbit(*frame) != 0U) ? SPDIF_C : 0U;

		buf[2U * f] = (*frame & SAMPLE_MASK) | c;
		buf[2U * f + 1U] = ((*frame ^ RIGHT_XOR) & SAMPLE_MASK) | c;
	}
}

static int tx_write(uint32_t *frame)
{
	void *blk;
	int ret = k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(1000));

	if (ret < 0) {
		return ret;
	}

	tx_fill(blk, frame);

	ret = i2s_write(tx_dev, blk, TX_BLOCK);
	if (ret < 0) {
		k_mem_slab_free(&tx_slab, blk);
	}

	return ret;
}

static void test_reject(void)
{
	int ret;

	ret = configure(tx_dev, I2S_DIR_TX, 16U, 2U, TX_RATE, TX_BLOCK, &tx_slab,
			I2S_OPT_BIT_CLK_GATED);
	RESULT(ret == -EINVAL, "reject 16-bit words (%d)", ret);

	ret = configure(tx_dev, I2S_DIR_TX, 24U, 1U, TX_RATE, TX_BLOCK, &tx_slab,
			I2S_OPT_BIT_CLK_GATED);
	RESULT(ret == -EINVAL, "reject 1 channel (%d)", ret);

	ret = configure(tx_dev, I2S_DIR_TX, 24U, 2U, TX_RATE, TX_BLOCK, &tx_slab,
			I2S_OPT_BIT_CLK_GATED | I2S_OPT_BIT_CLK_TARGET |
				I2S_OPT_FRAME_CLK_TARGET);
	RESULT(ret == -EINVAL, "reject clock target (%d)", ret);
}

static int capture(void)
{
	uint32_t frame = 0U;
	int ret;

	ret = configure(tx_dev, I2S_DIR_TX, 24U, 2U, TX_RATE, TX_BLOCK, &tx_slab,
			I2S_OPT_BIT_CLK_GATED);
	RESULT(ret == 0, "configure S/PDIF TX (%d)", ret);
	if (ret < 0) {
		return ret;
	}

	ret = configure(rx_dev, I2S_DIR_RX, 32U, 2U, RX_RATE, RX_BLOCK, &rx_slab,
			I2S_OPT_BIT_CLK_GATED);
	RESULT(ret == 0, "configure sampler RX (%d)", ret);
	if (ret < 0) {
		return ret;
	}

	for (uint32_t i = 0U; i < TX_QUEUED; i++) {
		ret = tx_write(&frame);
		if (ret < 0) {
			printk("TX write %u: %d\n", i, ret);
			return ret;
		}
	}

	ret = i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		printk("TX START: %d\n", ret);
		return ret;
	}

	/* Refill the queue slot freed by START: 5 blocks last 240 ms */
	ret = tx_write(&frame);
	if (ret < 0) {
		printk("TX write: %d\n", ret);
	}

	ret = i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		printk("RX START: %d\n", ret);
		goto out;
	}

	for (uint32_t i = 0U; i < CAP_BLOCKS; i++) {
		void *blk;
		size_t size;

		ret = i2s_read(rx_dev, &blk, &size);
		if (ret < 0) {
			printk("RX read %u: %d\n", i, ret);
			break;
		}

		memcpy(&cap[i * (RX_BLOCK / sizeof(uint32_t))], blk, size);
		k_mem_slab_free(&rx_slab, blk);
	}

	(void)i2s_trigger(rx_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
out:
	(void)i2s_trigger(tx_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);

	RESULT(ret == 0, "capture %u bits", CAP_BITS);

	return ret;
}

static inline uint32_t cap_bit(uint32_t i)
{
	return (cap[i / 32U] >> (31U - (i % 32U))) & 1U;
}

/* Run lengths of the captured line, in RX bit clock periods */
struct runs {
	uint32_t pos;
	uint32_t level;
};

static uint32_t next_run(struct runs *r)
{
	uint32_t start = r->pos;

	while ((r->pos < CAP_BITS) && (cap_bit(r->pos) == r->level)) {
		r->pos++;
	}

	r->level ^= 1U;

	return r->pos - start;
}

/* Half-cell length, in RX bit clock periods, times 16 */
static uint32_t half_cell_x16(void)
{
	struct runs r = {.pos = 0U, .level = cap_bit(0U)};
	uint32_t min = UINT32_MAX;
	uint32_t sum = 0U, cnt = 0U;

	(void)next_run(&r); /* the first run is truncated */
	for (uint32_t i = 0U; i < 4096U && r.pos < CAP_BITS; i++) {
		uint32_t len = next_run(&r);

		if ((len >= 3U) && (len < min)) {
			min = len;
		}
	}

	r.pos = 0U;
	r.level = cap_bit(0U);
	(void)next_run(&r);
	for (uint32_t i = 0U; i < 4096U && r.pos < CAP_BITS; i++) {
		uint32_t len = next_run(&r);

		if ((len >= min) && (2U * len <= 3U * min)) {
			sum += len;
			cnt++;
		}
	}

	return (cnt == 0U) ? 0U : (16U * sum) / cnt;
}

enum preamble {
	PRE_NONE,
	PRE_B,
	PRE_M,
	PRE_W,
};

struct decoder {
	struct runs r;
	uint32_t h16;
	uint32_t pending; /* units already read but not consumed */
};

static uint32_t next_units(struct decoder *d)
{
	uint32_t u;

	if (d->pending != 0U) {
		u = d->pending;
		d->pending = 0U;
		return u;
	}

	u = next_run(&d->r);

	/* Round len / h */
	return (32U * u + d->h16) / (2U * d->h16);
}

/* Find the next preamble; returns PRE_NONE at the end of the capture */
static enum preamble find_preamble(struct decoder *d)
{
	while (d->r.pos < CAP_BITS) {
		uint32_t u0 = next_units(d);
		uint32_t u1, u2, u3;

		if (u0 != 3U) {
			continue;
		}

		u1 = next_units(d);
		u2 = next_units(d);
		u3 = next_units(d);

		if ((u1 == 1U) && (u2 == 1U) && (u3 == 3U)) {
			return PRE_B;
		}
		if ((u1 == 3U) && (u2 == 1U) && (u3 == 1U)) {
			return PRE_M;
		}
		if ((u1 == 2U) && (u2 == 1U) && (u3 == 2U)) {
			return PRE_W;
		}

		/* u3 may be the start of the next preamble */
		d->pending = u3;
	}

	return PRE_NONE;
}

/* Decode the 28 time slots after a preamble; returns false on a coding error */
static bool read_slots(struct decoder *d, uint32_t *slots)
{
	*slots = 0U;

	for (uint32_t i = 0U; i < 28U; i++) {
		uint32_t u = next_units(d);

		if (u == 2U) {
			continue;
		}

		if ((u != 1U) || (next_units(d) != 1U)) {
			return false;
		}

		*slots |= BIT(i);
	}

	return d->r.pos < CAP_BITS;
}

struct subframe {
	enum preamble pre;
	uint32_t sample;
	uint32_t v, u, c;
	bool parity_ok;
};

static bool read_subframe(struct decoder *d, struct subframe *sf)
{
	uint32_t slots;

	sf->pre = find_preamble(d);
	if (sf->pre == PRE_NONE) {
		return false;
	}

	if (!read_slots(d, &slots)) {
		sf->pre = PRE_NONE;
		return true;
	}

	/* Slots 4-27: sample LSB first, 28: V, 29: U, 30: C, 31: P */
	sf->sample = slots & SAMPLE_MASK;
	sf->v = (slots >> 24) & 1U;
	sf->u = (slots >> 25) & 1U;
	sf->c = (slots >> 26) & 1U;
	sf->parity_ok = (__builtin_popcount(slots) % 2) == 0;

	return true;
}

static void decode(void)
{
	struct decoder d = {.r = {.pos = 0U, .level = cap_bit(0U)}};
	struct subframe a, b;
	uint32_t frames = 0U, blocks = 0U, coding = 0U, parity = 0U, order = 0U;
	uint32_t data = 0U, seq = 0U, status = 0U, vu = 0U, align = 0U;
	uint32_t first = 0U, prev = 0U;
	uint32_t errors_shown = 0U;

	d.h16 = half_cell_x16();
	printk("half-cell: %u.%02u RX bit clocks\n", d.h16 / 16U, (d.h16 % 16U) * 100U / 16U);
	RESULT((d.h16 > 4U * 16U) && (d.h16 < 64U * 16U), "half-cell length plausible");
	if (d.h16 == 0U) {
		return;
	}

	(void)next_run(&d.r); /* the first run is truncated */

	/* Lock on the first left sub-frame */
	do {
		if (!read_subframe(&d, &a)) {
			RESULT(false, "no S/PDIF preamble found");
			return;
		}
	} while ((a.pre != PRE_B) && (a.pre != PRE_M));

	while (true) {
		bool ok = true;

		if (!read_subframe(&d, &b)) {
			break;
		}

		if ((a.pre == PRE_NONE) || (b.pre == PRE_NONE)) {
			coding++;
			ok = false;
		} else if (((a.pre != PRE_B) && (a.pre != PRE_M)) || (b.pre != PRE_W)) {
			order++;
			ok = false;
		} else {
			uint32_t n = a.sample;

			if (!a.parity_ok || !b.parity_ok) {
				parity++;
				ok = false;
			}
			if (b.sample != ((n ^ RIGHT_XOR) & SAMPLE_MASK)) {
				data++;
				ok = false;
			}
			if ((frames != 0U) && (n != ((prev + 1U) & SAMPLE_MASK))) {
				seq++;
				ok = false;
			}
			if ((a.v | a.u | b.v | b.u) != 0U) {
				vu++;
				ok = false;
			}
			if ((a.c != cbit(n)) || (b.c != cbit(n))) {
				status++;
				ok = false;
			}
			if ((a.pre == PRE_B) != ((n % SPDIF_BLOCK_FRAMES) == 0U)) {
				align++;
				ok = false;
			}
			if (a.pre == PRE_B) {
				blocks++;
			}
			if (frames == 0U) {
				first = n;
			}
			prev = n;
		}

		if (!ok && (errors_shown < 8U)) {
			errors_shown++;
			printk("  frame %u: pre %d/%d L %06x R %06x VUC %u%u%u/%u%u%u P %d/%d\n",
			       frames, a.pre, b.pre, a.sample, b.sample, a.v, a.u, a.c, b.v, b.u,
			       b.c, a.parity_ok, b.parity_ok);
		}

		frames++;

		/* Resync on the next left sub-frame after an error */
		if (!read_subframe(&d, &a)) {
			break;
		}
	}

	printk("decoded %u frames, first counter %u, last %u, %u B preambles\n", frames, first,
	       prev, blocks);
	RESULT(frames >= 600U, "frame count (%u)", frames);
	RESULT(blocks >= 3U, "block preambles (%u)", blocks);
	RESULT(coding == 0U, "biphase coding errors (%u)", coding);
	RESULT(order == 0U, "preamble order errors (%u)", order);
	RESULT(parity == 0U, "parity errors (%u)", parity);
	RESULT(data == 0U, "left/right data errors (%u)", data);
	RESULT(seq == 0U, "frame sequence errors (%u)", seq);
	RESULT(vu == 0U, "V/U bit errors (%u)", vu);
	RESULT(status == 0U, "channel status bit errors (%u)", status);
	RESULT(align == 0U, "block start alignment errors (%u)", align);
}

int main(void)
{
	printk("SAI S/PDIF TX self-check (wiring PE6-PE3)\n");

	if (!device_is_ready(tx_dev) || !device_is_ready(rx_dev)) {
		printk("SAI devices not ready\n");
		return 0;
	}

	test_reject();

	if (capture() == 0) {
		decode();
	}

	printk("\n==== %d passed, %d failed ====\n", n_pass, n_fail);

	return 0;
}
