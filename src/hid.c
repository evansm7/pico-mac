/*
 * Derived from pico-examples/usb/host/host_cdc_msc_hid/hid_app.c, which is
 * Copyright (c) 2021, Ha Thach (tinyusb.org)
 * Further changes are Copyright 2024 Matt Evans
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "bsp/rp2040/board.h"
#include "tusb.h"

#include "kbd.h"

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

// If your host terminal support ansi escape code such as TeraTerm
// it can be use to simulate mouse cursor movement within terminal
#define USE_ANSI_ESCAPE   0

#define MAX_REPORT  4

static uint8_t const keycode2ascii[128][2] =  { HID_KEYCODE_TO_ASCII };

// Each HID instance can has multiple reports
static struct
{
        uint8_t report_count;
        tuh_hid_report_info_t report_info[MAX_REPORT];
} hid_info[CFG_TUH_HID];

static void process_kbd_report(hid_keyboard_report_t const *report);
static void process_mouse_report(hid_mouse_report_t const * report);
static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len);

void hid_app_task(void)
{
        // nothing to do
}

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use. tuh_hid_parse_report_descriptor()
// can be used to parse common/simple enough descriptor.
// Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE, it will be skipped
// therefore report_desc = NULL, desc_len = 0
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
        printf("HID device address = %d, instance = %d is mounted\r\n", dev_addr, instance);

        // Interface protocol (hid_interface_protocol_enum_t)
        const char* protocol_str[] = { "None", "Keyboard", "Mouse" };
        uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

        printf("HID Interface Protocol = %s\r\n", protocol_str[itf_protocol]);

        // By default host stack will use activate boot protocol on supported interface.
        // Therefore for this simple example, we only need to parse generic report descriptor (with built-in parser)
        if ( itf_protocol == HID_ITF_PROTOCOL_NONE )
        {
                hid_info[instance].report_count = tuh_hid_parse_report_descriptor(hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
                printf("HID has %u reports \r\n", hid_info[instance].report_count);
        }

        // request to receive report
        // tuh_hid_report_received_cb() will be invoked when report is available
        if ( !tuh_hid_receive_report(dev_addr, instance) )
        {
                printf("Error: cannot request to receive report\r\n");
        }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
        printf("HID device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
}

// Invoked when received report from device via interrupt endpoint
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
        uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

        switch (itf_protocol)
        {
        case HID_ITF_PROTOCOL_KEYBOARD:
                TU_LOG2("HID receive boot keyboard report\r\n");
                process_kbd_report( (hid_keyboard_report_t const*) report );
                break;

        case HID_ITF_PROTOCOL_MOUSE:
                TU_LOG2("HID receive boot mouse report\r\n");
                process_mouse_report( (hid_mouse_report_t const*) report );
                break;

        default:
                // Generic report requires matching ReportID and contents with previous parsed report info
                process_generic_report(dev_addr, instance, report, len);
                break;
        }

        // continue to request to receive report
        if ( !tuh_hid_receive_report(dev_addr, instance) )
        {
                printf("Error: cannot request to receive report\r\n");
        }
}

//--------------------------------------------------------------------+
// Keyboard
//--------------------------------------------------------------------+

static inline bool find_key_in_report(hid_keyboard_report_t const *report, uint8_t keycode)
{
        for(uint8_t i=0; i<6; i++)
        {
                if (report->keycode[i] == keycode)  return true;
        }

        return false;
}

static void process_kbd_report(hid_keyboard_report_t const *report)
{
        /* Previous report is stored to compare against for key release: */
        static hid_keyboard_report_t prev_report = { 0, 0, {0} };

        for(uint8_t i=0; i<6; i++) {
                if (report->keycode[i]) {
                        if (find_key_in_report(&prev_report, report->keycode[i])) {
                                /* Key held */
                        } else {
                                /* printf("Key pressed: %02x\n", report->keycode[i]); */
                                kbd_queue_push(report->keycode[i], true);
                        }
                }
                if (prev_report.keycode[i] && !find_key_in_report(report, prev_report.keycode[i])) {
                        /* printf("Key released: %02x\n", prev_report.keycode[i]); */
                        kbd_queue_push(prev_report.keycode[i], false);
                }
        }
        uint8_t mod_change = report->modifier ^ prev_report.modifier;
        if (mod_change) {
                uint8_t mp = mod_change & report->modifier;
                uint8_t mr = mod_change & prev_report.modifier;
                if (mp) {
                        /* printf("Modifiers pressed %02x\n", mp); */
                        mp = (mp | (mp >> 4)) & 0xf; /* Don't care if left or right :P */
                        if (mp & 1)
                                kbd_queue_push(HID_KEY_CONTROL_LEFT, true);
                        if (mp & 2)
                                kbd_queue_push(HID_KEY_SHIFT_LEFT, true);
                        if (mp & 4)
                                kbd_queue_push(HID_KEY_ALT_LEFT, true);
                        if (mp & 8)
                                kbd_queue_push(HID_KEY_GUI_LEFT, true);
                }
                if (mr) {
                        /* printf("Modifiers released %02x\n", mr); */
                        mr = (mr | (mr >> 4)) & 0xf;
                        if (mr & 1)
                                kbd_queue_push(HID_KEY_CONTROL_LEFT, false);
                        if (mr & 2)
                                kbd_queue_push(HID_KEY_SHIFT_LEFT, false);
                        if (mr & 4)
                                kbd_queue_push(HID_KEY_ALT_LEFT, false);
                        if (mr & 8)
                                kbd_queue_push(HID_KEY_GUI_LEFT, false);
                }
        }
        prev_report = *report;
}

//--------------------------------------------------------------------+
// Mouse
//--------------------------------------------------------------------+

/* Exported for use by other thread! */
int cursor_x = 0;
int cursor_y = 0;
int cursor_button = 0;

#define MAX_DELTA       8

static int clamp(int i)
{
        return (i >= 0) ? (i > MAX_DELTA ? MAX_DELTA : i) :
                (i < -MAX_DELTA ? -MAX_DELTA : i);
}

static void process_mouse_report(hid_mouse_report_t const * report)
{
        static hid_mouse_report_t prev_report = { 0 };
        uint8_t button_changed_mask = report->buttons ^ prev_report.buttons;
        /* report->wheel can be used too... */

        cursor_button = !!(report->buttons & MOUSE_BUTTON_LEFT);
        cursor_x += clamp(report->x);
        cursor_y += clamp(report->y);
}

//--------------------------------------------------------------------+
// Generic Report
//--------------------------------------------------------------------+
static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
        (void) dev_addr;

        uint8_t const rpt_count = hid_info[instance].report_count;
        tuh_hid_report_info_t* rpt_info_arr = hid_info[instance].report_info;
        tuh_hid_report_info_t* rpt_info = NULL;

        if ( rpt_count == 1 && rpt_info_arr[0].report_id == 0)
        {
                // Simple report without report ID as 1st byte
                rpt_info = &rpt_info_arr[0];
        }else
        {
                // Composite report, 1st byte is report ID, data starts from 2nd byte
                uint8_t const rpt_id = report[0];

                // Find report id in the arrray
                for(uint8_t i=0; i<rpt_count; i++)
                {
                        if (rpt_id == rpt_info_arr[i].report_id )
                        {
                                rpt_info = &rpt_info_arr[i];
                                break;
                        }
                }

                report++;
                len--;
        }

        if (!rpt_info)
        {
                printf("Couldn't find the report info for this report !\r\n");
                return;
        }

        // For complete list of Usage Page & Usage checkout src/class/hid/hid.h. For examples:
        // - Keyboard                     : Desktop, Keyboard
        // - Mouse                        : Desktop, Mouse
        // - Gamepad                      : Desktop, Gamepad
        // - Consumer Control (Media Key) : Consumer, Consumer Control
        // - System Control (Power key)   : Desktop, System Control
        // - Generic (vendor)             : 0xFFxx, xx
        if ( rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP )
        {
                switch (rpt_info->usage)
                {
                case HID_USAGE_DESKTOP_KEYBOARD:
                        TU_LOG1("HID receive keyboard report\r\n");
                        // Assume keyboard follow boot report layout
                        process_kbd_report( (hid_keyboard_report_t const*) report );
                        break;

                case HID_USAGE_DESKTOP_MOUSE:
                        TU_LOG1("HID receive mouse report\r\n");
                        // Assume mouse follow boot report layout
                        process_mouse_report( (hid_mouse_report_t const*) report );
                        break;

                default: break;
                }
        }
}
