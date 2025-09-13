#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "hardware/structs/bus_ctrl.h"
#include <hardware/structs/qmi.h>
#include "hardware/dma.h"
#include "pico/sem.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode_font_2bpp.h"

// TODO should put this in scratch_x, it out to fit...
#include "font_8x8.h"
#define FONT_CHAR_WIDTH 8
#define FONT_CHAR_HEIGHT 8
#define FONT_N_CHARS 95
#define FONT_FIRST_ASCII 32


// Pick one:
#define MODE_640x480_60Hz
// #define MODE_720x480_60Hz
// #define MODE_800x600_60Hz
// #define MODE_960x540p_60Hz
// #define MODE_1280x720_30Hz

#if defined(MODE_640x480_60Hz)
// DVDD 1.2V (1.1V seems ok too)
#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_640x480p_60hz

#elif defined(MODE_720x480_60Hz)
// DVDD 1.2V
#define FRAME_WIDTH 720
#define FRAME_HEIGHT 480
#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_720x480p_60hz

#elif defined(MODE_800x600_60Hz)
// DVDD 1.3V, going downhill with a tailwind
#define FRAME_WIDTH 800
#define FRAME_HEIGHT 600
#define VREG_VSEL VREG_VOLTAGE_1_30
#define DVI_TIMING dvi_timing_800x600p_60hz

#elif defined(MODE_960x540p_60Hz)
// DVDD 1.25V (slower silicon may need the full 1.3, or just not work)
#define FRAME_WIDTH 960
#define FRAME_HEIGHT 540
#define VREG_VSEL VREG_VOLTAGE_1_25
#define DVI_TIMING dvi_timing_960x540p_60hz

#elif defined(MODE_1280x720_30Hz)
// 1280x720p 30 Hz (nonstandard)
// DVDD 1.25V (slower silicon may need the full 1.3, or just not work)
#define FRAME_WIDTH 1280
#define FRAME_HEIGHT 720
#define VREG_VSEL VREG_VOLTAGE_1_25
#define DVI_TIMING dvi_timing_1280x720p_30hz

#else
#error "Select a video mode!"
#endif

struct dvi_inst dvi0;

#define CHAR_COLS (FRAME_WIDTH / FONT_CHAR_WIDTH)
#define CHAR_ROWS (FRAME_HEIGHT / FONT_CHAR_HEIGHT)

#define COLOUR_PLANE_SIZE_WORDS (CHAR_ROWS * CHAR_COLS * 4 / 32)
char charbuf[CHAR_ROWS * CHAR_COLS];
uint32_t colourbuf[3 * COLOUR_PLANE_SIZE_WORDS];

static inline void set_char(uint x, uint y, char c) {
	if (x >= CHAR_COLS || y >= CHAR_ROWS)
		return;
	charbuf[x + y * CHAR_COLS] = c;
}

// Pixel format RGB222
static inline void set_colour(uint x, uint y, uint8_t fg, uint8_t bg) {
	if (x >= CHAR_COLS || y >= CHAR_ROWS)
		return;
	uint char_index = x + y * CHAR_COLS;
	uint bit_index = char_index % 8 * 4;
	uint word_index = char_index / 8;
	for (int plane = 0; plane < 3; ++plane) {
		uint32_t fg_bg_combined = (fg & 0x3) | (bg << 2 & 0xc);
		colourbuf[word_index] = (colourbuf[word_index] & ~(0xfu << bit_index)) | (fg_bg_combined << bit_index);
		fg >>= 2;
		bg >>= 2;
		word_index += COLOUR_PLANE_SIZE_WORDS;
	}
}

#include "tmds_encode.h"
#include "moon_1bpp_640x480.h"
#define moon_img moon_1bpp_640x480
#include "tmds_encode_1bpp.pio.h"

void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);

    while (true) {
        for (uint y = 0; y < FRAME_HEIGHT; ++y) {
			const uint32_t *colourbuf2 = &((const uint32_t*)moon_img)[y * FRAME_WIDTH / 32];
            uint32_t *tmdsbuf = 0;
            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
			int plane = 1;
			tmds_encode_font_2bpp(
				(const uint8_t*)&charbuf[y / FONT_CHAR_HEIGHT * CHAR_COLS],
				(const uint32_t*)(&colourbuf[y / FONT_CHAR_HEIGHT * (COLOUR_PLANE_SIZE_WORDS / CHAR_ROWS) + plane * COLOUR_PLANE_SIZE_WORDS]),
				(tmdsbuf + plane * (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD)),
				FRAME_WIDTH,
				(const uint8_t*)&font_8x8[y % FONT_CHAR_HEIGHT * FONT_N_CHARS] - FONT_FIRST_ASCII
			);
			tmds_encode_1bpp(colourbuf2, tmdsbuf, FRAME_WIDTH);
			tmds_encode_1bpp(colourbuf2, (tmdsbuf + 2 * (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD)), FRAME_WIDTH);
            queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
		}
    }
}

void __not_in_flash() flash_timings() {
	const int max_flash_freq = 88 * MHZ;
	const int clock_hz = DVI_TIMING.bit_clk_khz * 1000;
	int divisor = (clock_hz + max_flash_freq - 1) / max_flash_freq;
	if (divisor == 1 && clock_hz > 100000000) {
		divisor = 2;
	}
	int rxdelay = divisor;
	if (clock_hz / divisor > 100000000) {
		rxdelay += 1;
	}
	qmi_hw->m[0].timing = 0x60007000 |
						rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
						divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

int __not_in_flash("main") main() {
	vreg_disable_voltage_limit();
	vreg_set_voltage(VREG_VSEL);
    flash_timings();
    sleep_ms(100);
	// Run system at TMDS bit clock
	set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

	dvi0.timing = &DVI_TIMING;
	dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
	dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

	for (uint y = 0; y < CHAR_ROWS; ++y) {
		for (uint x = 0; x < CHAR_COLS; ++x) {
			set_char(x, y, (x + y * CHAR_COLS) % FONT_N_CHARS + FONT_FIRST_ASCII);
			set_colour(x, y, x, y);
		}
	}

	hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
	multicore_launch_core1(core1_main);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    /// main test DONE signal
    for (int i = 0; i < 6; i++) {
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

	while (1)
		__wfi();
}
	
