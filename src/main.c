/* pico-umac
 *
 * Main loop to initialise umac, and run main event loop (piping
 * keyboard/mouse events in).
 *
 * Copyright 2024 Matt Evans
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hw.h"
#include "video.h"
#include "kbd.h"

#include "bsp/rp2040/board.h"
#include "tusb.h"

#include "umac.h"

////////////////////////////////////////////////////////////////////////////////
// Imports and data

extern void     hid_app_task(void);
extern int cursor_x;
extern int cursor_y;
extern int cursor_button;

// Mac binary data:  disc and ROM images
static const uint8_t umac_disc[] = {
#include "umac-disc.h"
};
static const uint8_t umac_rom[] = {
#include "umac-rom.h"
};

static uint8_t umac_ram[RAM_SIZE];

////////////////////////////////////////////////////////////////////////////////

static void     io_init()
{
        gpio_init(GPIO_LED_PIN);
        gpio_set_dir(GPIO_LED_PIN, GPIO_OUT);
}

static void     poll_led_etc()
{
        static int led_on = 0;
        static absolute_time_t last = 0;
        absolute_time_t now = get_absolute_time();

        if (absolute_time_diff_us(last, now) > 500*1000) {
                last = now;

                led_on ^= 1;
                gpio_put(GPIO_LED_PIN, led_on);
        }
}

static int umac_cursor_x = 0;
static int umac_cursor_y = 0;
static int umac_cursor_button = 0;

static void     poll_umac()
{
        static absolute_time_t last_1hz = 0;
        static absolute_time_t last_vsync = 0;
        absolute_time_t now = get_absolute_time();

        umac_loop();

        int64_t p_1hz = absolute_time_diff_us(last_1hz, now);
        int64_t p_vsync = absolute_time_diff_us(last_vsync, now);
        if (p_vsync >= 16667) {
                /* FIXME: Trigger this off actual vsync */
                umac_vsync_event();
                last_vsync = now;
        }
        if (p_1hz >= 1000000) {
                umac_1hz_event();
                last_1hz = now;
        }

        int update = 0;
        int dx = 0;
        int dy = 0;
        int b = umac_cursor_button;
        if (cursor_x != umac_cursor_x) {
                dx = cursor_x - umac_cursor_x;
                umac_cursor_x = cursor_x;
                update = 1;
        }
        if (cursor_y != umac_cursor_y) {
                dy = cursor_y - umac_cursor_y;
                umac_cursor_y = cursor_y;
                update = 1;
        }
        if (cursor_button != umac_cursor_button) {
                b = cursor_button;
                umac_cursor_button = cursor_button;
                update = 1;
        }
        if (update) {
                umac_mouse(dx, -dy, b);
        }

        if (!kbd_queue_empty()) {
                uint16_t k = kbd_queue_pop();
                umac_kbd_event(k & 0xff, !!(k & 0x8000));
        }
}

static void     core1_main()
{
        printf("Core 1 started\n");

        disc_descr_t discs[DISC_NUM_DRIVES] = {0};
        discs[0].base = (void *)umac_disc;
        discs[0].read_only = 1;
        discs[0].size = sizeof(umac_disc);

        umac_init(umac_ram, (void *)umac_rom, discs);
        /* Video runs on core 1, i.e. IRQs/DMA are unaffected by
         * core 0's USB activity.
         */
        video_init((uint32_t *)(umac_ram + umac_get_fb_offset()));

        while (true) {
                poll_umac();
        }
}

int     main()
{
        set_sys_clock_khz(250*1000, true);

	stdio_init_all();
        io_init();

        multicore_launch_core1(core1_main);

	printf("Starting, init usb\n");
        tusb_init();

        /* This happens on core 0: */
	while (true) {
                tuh_task();
                hid_app_task();
                poll_led_etc();
	}

	return 0;
}

