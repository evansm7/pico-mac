/* Video output:
 *
 * Using PIO[1], output the Mac 512x342 1BPP framebuffer to VGA/pins.  This is done
 * directly from the Mac framebuffer (without having to reformat in an intermediate
 * buffer).  The video output is 640x480, with the visible pixel data centred with
 * borders:  for analog VGA this is easy, as it just means increasing the horizontal
 * back porch/front porch (time between syncs and active video) and reducing the
 * display portion of a line.
 *
 * [1]: see pio_video.pio
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
#include <inttypes.h>
#include <string.h>
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/structs/padsbank0.h"
#include "pio_video.pio.h"

#include "hw.h"

////////////////////////////////////////////////////////////////////////////////
/* VESA VGA mode 640x480@60 */

/* The pixel clock _should_ be (125/2/25.175) (about 2.483) but that seems to
 * make my VGA-HDMI adapter sample weird, and pixels crawl.  Fudge a little,
 * looks better:
 */
#define VIDEO_PCLK_MULT         (2.5*2)
#define VIDEO_HSW               96
#define VIDEO_HBP               48
#define VIDEO_HRES              640
#define VIDEO_HFP               16
#define VIDEO_H_TOTAL_NOSYNC    (VIDEO_HBP + VIDEO_HRES + VIDEO_HFP)
#define VIDEO_VSW               2
#define VIDEO_VBP               33
#define VIDEO_VRES              480
#define VIDEO_VFP               10
#define VIDEO_V_TOTAL           (VIDEO_VSW + VIDEO_VBP + VIDEO_VRES + VIDEO_VFP)
/* The visible vertical span in the VGA output, [start, end) lines: */
#define VIDEO_V_VIS_START       (VIDEO_VSW + VIDEO_VBP)
#define VIDEO_V_VIS_END         (VIDEO_V_VIS_START + VIDEO_VRES)

#define VIDEO_FB_HRES           DISP_WIDTH
#define VIDEO_FB_VRES           DISP_HEIGHT

/* The lines at which the FB data is actively output: */
#define VIDEO_FB_V_VIS_START    (VIDEO_V_VIS_START + ((VIDEO_VRES - VIDEO_FB_VRES)/2))
#define VIDEO_FB_V_VIS_END      (VIDEO_FB_V_VIS_START + VIDEO_FB_VRES)

/* Words of 1BPP pixel data per line; this dictates the length of the
 * video data DMA transfer:
 */
#define VIDEO_VISIBLE_WPL       (VIDEO_FB_HRES / 32)

#if (VIDEO_HRES & 31)
#error "VIDEO_HRES: must be a multiple of 32b!"
#endif

////////////////////////////////////////////////////////////////////////////////
// Video DMA, framebuffer pointers

static uint32_t video_null[VIDEO_VISIBLE_WPL];
static uint32_t *video_framebuffer;

/* DMA buffer containing 2 pairs of per-line config words, for VS and not-VS: */
static uint32_t video_dma_cfg[4];

/* 3 DMA channels are used.  The first to transfer data to PIO, and
 * the other two to transfer descriptors to the first channel.
 */
static uint8_t video_dmach_tx;
static uint8_t video_dmach_descr_cfg;
static uint8_t video_dmach_descr_data;

typedef struct {
        const void *raddr;
        void *waddr;
        uint32_t count;
        uint32_t ctrl;
} dma_descr_t;

static dma_descr_t video_dmadescr_cfg;
static dma_descr_t video_dmadescr_data;

static volatile unsigned int video_current_y = 0;

static int      __not_in_flash_func(video_get_visible_y)(unsigned int y) {
        if ((y >= VIDEO_FB_V_VIS_START) && (y < VIDEO_FB_V_VIS_END)) {
                return y - VIDEO_FB_V_VIS_START;
        } else {
                return -1;
        }
}

static const uint32_t   *__not_in_flash_func(video_line_addr)(unsigned int y)
{
        int vy = video_get_visible_y(y);
        if (vy >= 0)
                return (const uint32_t *)&video_framebuffer[vy * VIDEO_VISIBLE_WPL];
        else
                return (const uint32_t *)video_null;
}

static const uint32_t   *__not_in_flash_func(video_cfg_addr)(unsigned int y)
{
        return &video_dma_cfg[(y < VIDEO_VSW) ? 0 : 2];
}

static void    __not_in_flash_func(video_dma_prep_new)()
{
        /* The descriptor DMA read pointers have moved on; reset them.
         * The write pointers wrap so should be pointing to the
         * correct DMA regs.
         */
        dma_hw->ch[video_dmach_descr_cfg].read_addr = (uintptr_t)&video_dmadescr_cfg;
        dma_hw->ch[video_dmach_descr_cfg].transfer_count = 4;
        dma_hw->ch[video_dmach_descr_data].read_addr = (uintptr_t)&video_dmadescr_data;
        dma_hw->ch[video_dmach_descr_data].transfer_count = 4;

        /* Configure the two DMA descriptors, video_dmadescr_cfg and
         * video_dmadescr_data, to transfer from video config/data corresponding
         * to the current line.
         *
         * These descriptors will be used to program the video_dmach_tx channel,
         * pushing the buffer to PIO.
         *
         * This can be relatively relaxed, as it's triggered as line data
         * starts; we have until the end of the video line (when the descriptors
         * are retriggered) to program them.
         *
         * FIXME: this time could be used for something clever like split-screen
         * (e.g. info/text lines) constructed on-the-fly.
         */
        video_dmadescr_cfg.raddr = video_cfg_addr(video_current_y);
        video_dmadescr_data.raddr = video_line_addr(video_current_y);

        /* Frame done */
        if (++video_current_y >= VIDEO_V_TOTAL)
                video_current_y = 0;
}

static void     __not_in_flash_func(video_dma_irq)()
{
        /* The DMA IRQ occurs once the video portion of the line has been
         * triggered (not when the video transfer completes, but when the
         * descriptor transfer (that leads to the video transfer!) completes.
         * All we need to do is reconfigure the descriptors; the video DMA will
         * re-trigger the descriptors later.
         */
        if (dma_channel_get_irq0_status(video_dmach_descr_data)) {
                dma_channel_acknowledge_irq0(video_dmach_descr_data);
                video_dma_prep_new();
        }
}

static void     video_prep_buffer()
{
        memset(video_null, 0xff, VIDEO_VISIBLE_WPL * 4);

        unsigned int porch_padding = (VIDEO_HRES - VIDEO_FB_HRES)/2;
        // FIXME: HBP/HFP are prob off by one or so, check
        uint32_t timing = ((VIDEO_HSW - 1) << 23) |
                ((VIDEO_HBP + porch_padding - 3) << 15) |
                ((VIDEO_HFP + porch_padding - 4) << 7);
        video_dma_cfg[0] = timing | 0x80000000;
        video_dma_cfg[1] = VIDEO_FB_HRES - 1;
        video_dma_cfg[2] = timing;
        video_dma_cfg[3] = VIDEO_FB_HRES - 1;
}

static void     video_init_dma()
{
        /* pio_video expects each display line to be composed of two words of config
         * describing the line geometry and whether VS is asserted, followed by
         * visible data.
         *
         * To avoid having to embed config metadata in the display framebuffer,
         * we use two DMA transfers to PIO for each line.  The first transfers
         * the config from a config buffer, and then triggers the second to
         * transfer the video data from the framebuffer.  (This lets us use a
         * flat, regular FB.)
         *
         * The PIO side emits 1BPP MSB-first.  The other advantage of
         * using a second DMA transfer is then we can also can
         * byteswap the DMA of the video portion to match the Mac
         * framebuffer layout.
         *
         *  "Another caveat is that multiple channels should not be connected
         *   to the same DREQ.":
         * The final complexity is that only one DMA channel can do the
         * transfers to PIO, because of how the credit-based flow control works.
         * So, _only_ channel 0 transfers from $SOME_BUFFER into the PIO FIFO,
         * and channel 1+2 are used to reprogram/trigger channel 0 from a DMA
         * descriptor list.
         *
         * Two extra channels are used to manage interrupts; ch1 programs ch0,
         * completes, and does nothing.  (It programs a descriptor that causes
         * ch0 to transfer config, then trigger ch2 when complete.)  ch2 then
         * programs ch0 with a descriptor to transfer data, then trigger ch1
         * when ch0 completes; when ch2 finishes doing that, it produces an IRQ.
         * Got that?
         *
         * The IRQ handler sets up ch1 and ch2 to point to 2 fresh cfg+data
         * descriptors; the deadline is by the end of ch0's data transfer
         * (i.e. a whole line).  When ch0 finishes the data transfer it again
         * triggers ch1, and the new config entry is programmed.
         */
        video_dmach_tx = dma_claim_unused_channel(true);
        video_dmach_descr_cfg = dma_claim_unused_channel(true);
        video_dmach_descr_data = dma_claim_unused_channel(true);

        /* Transmit DMA: config+video data */
        /* First, make dmacfg for data to transfer from config buffers + data buffers: */
        dma_channel_config dc_tx_c = dma_channel_get_default_config(video_dmach_tx);
        channel_config_set_dreq(&dc_tx_c, DREQ_PIO0_TX0);
        channel_config_set_transfer_data_size(&dc_tx_c, DMA_SIZE_32);
        channel_config_set_read_increment(&dc_tx_c, true);
        channel_config_set_write_increment(&dc_tx_c, false);
        channel_config_set_bswap(&dc_tx_c, false);
        /* Completion of the config TX triggers the video_dmach_descr_data channel */
        channel_config_set_chain_to(&dc_tx_c, video_dmach_descr_data);
        video_dmadescr_cfg.raddr = NULL;                /* Reprogrammed each line */
        video_dmadescr_cfg.waddr = (void *)&pio0_hw->txf[0];
        video_dmadescr_cfg.count = 2;                   /* 2 words of video config */
        video_dmadescr_cfg.ctrl = dc_tx_c.ctrl;

        dma_channel_config dc_tx_d = dma_channel_get_default_config(video_dmach_tx);
        channel_config_set_dreq(&dc_tx_d, DREQ_PIO0_TX0);
        channel_config_set_transfer_data_size(&dc_tx_d, DMA_SIZE_32);
        channel_config_set_read_increment(&dc_tx_d, true);
        channel_config_set_write_increment(&dc_tx_d, false);
        channel_config_set_bswap(&dc_tx_d, true);      /* This channel bswaps */
        /* Completion of the data TX triggers the video_dmach_descr_cfg channel */
        channel_config_set_chain_to(&dc_tx_d, video_dmach_descr_cfg);
        video_dmadescr_data.raddr = NULL;               /* Reprogrammed each line */
        video_dmadescr_data.waddr = (void *)&pio0_hw->txf[0];
        video_dmadescr_data.count = VIDEO_VISIBLE_WPL;
        video_dmadescr_data.ctrl = dc_tx_d.ctrl;

        /* Now, the descr_cfg and descr_data channels transfer _those_
         * descriptors to program the video_dmach_tx channel:
         */
        dma_channel_config dcfg = dma_channel_get_default_config(video_dmach_descr_cfg);
        channel_config_set_transfer_data_size(&dcfg, DMA_SIZE_32);
        channel_config_set_read_increment(&dcfg, true);
        channel_config_set_write_increment(&dcfg, true);
        /* This channel loops on 16-byte/4-wprd boundary (i.e. writes all config): */
        channel_config_set_ring(&dcfg, true, 4);
        /* No completion IRQ or chain: the video_dmach_tx DMA completes and triggers
         * the next 'data' descriptor transfer.
         */
        dma_channel_configure(video_dmach_descr_cfg, &dcfg,
                              &dma_hw->ch[video_dmach_tx].read_addr,
                              &video_dmadescr_cfg,
                              4 /* 4 words of config */,
                              false /* Not yet */);

        dma_channel_config ddata = dma_channel_get_default_config(video_dmach_descr_data);
        channel_config_set_transfer_data_size(&ddata, DMA_SIZE_32);
        channel_config_set_read_increment(&ddata, true);
        channel_config_set_write_increment(&ddata, true);
        channel_config_set_ring(&ddata, true, 4);
        /* This transfer has a completion IRQ.  Receipt of that means that both
         * config and data descriptors have been transferred, and should be
         * reprogrammed for the next line.
         */
        dma_channel_set_irq0_enabled(video_dmach_descr_data, true);
        dma_channel_configure(video_dmach_descr_data, &ddata,
                              &dma_hw->ch[video_dmach_tx].read_addr,
                              &video_dmadescr_data,
                              4 /* 4 words of config */,
                              false /* Not yet */);

        /* Finally, set up video_dmadescr_cfg.raddr and video_dmadescr_data.raddr to point
         * to next line's video cfg/data buffers.  Then, video_dmach_descr_cfg can be triggered
         * to start video.
         */
}

////////////////////////////////////////////////////////////////////////////////

/* Initialise PIO, DMA, start sending pixels.  Passed a pointer to a 512x342x1
 * Mac-order framebuffer.
 *
 * FIXME: Add an API to change the FB base after init live, e.g. for bank
 * switching.
 */
void    video_init(uint32_t *framebuffer)
{
        printf("Video init\n");

        pio_video_program_init(pio0, 0,
                               pio_add_program(pio0, &pio_video_program),
                               GPIO_VID_DATA, /* Followed by HS, VS, CLK */
                               VIDEO_PCLK_MULT);

        /* Invert output pins:  HS/VS are active-low, also invert video! */
        gpio_set_outover(GPIO_VID_HS, GPIO_OVERRIDE_INVERT);
        gpio_set_outover(GPIO_VID_VS, GPIO_OVERRIDE_INVERT);
        gpio_set_outover(GPIO_VID_DATA, GPIO_OVERRIDE_INVERT);
        /* Highest drive strength (VGA is current-based, innit) */
        hw_write_masked(&padsbank0_hw->io[GPIO_VID_DATA],
                        PADS_BANK0_GPIO0_DRIVE_VALUE_12MA << PADS_BANK0_GPIO0_DRIVE_LSB,
                        PADS_BANK0_GPIO0_DRIVE_BITS);

        /* IRQ handlers for DMA_IRQ_0: */
        irq_set_exclusive_handler(DMA_IRQ_0, video_dma_irq);
        irq_set_enabled(DMA_IRQ_0, true);

        video_init_dma();

        /* Init config word buffers */
        video_current_y = 0;
        video_framebuffer = framebuffer;
        video_prep_buffer();

        /* Set up pointers to first line, and start DMA */
        video_dma_prep_new();
        dma_channel_start(video_dmach_descr_cfg);
}
