# Pico Micro Mac (pico-umac)

v0.2 27 August 2024


This project embeds the [umac Mac 128K
emulator](https://github.com/evansm7/umac) project into a Raspberry Pi
Pico microcontroller.  At long last, the worst Macintosh in a cheap,
portable form factor!

It has features, many features, the best features:

   * Outputs VGA 640x480@60Hz, monochrome, using three resistors
   * USB HID keyboard and mouse
   * Read-only disc image in flash (your creations are ephemeral, like life itself)
   * Or, if you have a hard time letting go, support for rewritable
     disc storage on an SPI-attached SD card
   * Mac 128K by default, or you can make use of more of the Pico's
     memory and run as a _Mac 208K_

Great features.  It even doesn't hang at random!  (Anymore.)

The _Mac 208K_ was, of course, never a real machine.  But, _umac_
supports odd-sized memories, and more memory runs more things.  A
surprising amount of software runs on the 128K config, but if you need
to run _MacPaint_ specifically then you'll need to build both SD
storage in addition to the _Mac 208K_ config.

So anyway, you can build this project yourself for less than the cost
of a beer!  You'll need at least a RPi Pico board, a VGA monitor (or
VGA-HDMI adapter), a USB mouse (and maybe a USB keyboard/hub), plus a
couple of cheap components.

# Build

## Prerequisites/essentials

   * git submodules
      - Clone the repo with `--recursive`, or `git submodule update --init --recursive`
   * Install/set up the [Pico/RP2040 SDK](https://github.com/raspberrypi/pico-sdk)

## Build umac

Install and build `umac` first.  It'll give you a preview of the fun
to come, plus is required to generate a patched ROM image.

If you want to use a non-default memory size (i.e. >128K) you will
need to build `umac` with a matching `MEMSIZE` build parameter, for
example:

```
cd external/umac
make MEMSIZE=208
```

This is because `umac` is used to patch the ROM, and when using
unsupported sizes between 128K and 512K the RAM size can't be probed
automatically, so the size needs to be embedded.

## Build pico-umac

Do the initial Pico SDK `cmake` setup into an out-of-tree build dir,
providing config options if required.

From the top-level `pico-umac` directory:

```
mkdir build
(cd build ; PICO_SDK_PATH=/path/to/sdk cmake .. <options>)
```

Options are required if you want SD support, or more than the default 128K of memory:

   * `-DUSE_SD=true`: Include SD card support.  The GPIOs default to
     `spi0` running at 5MHz, and GPIOs 2,3,4,5 for
     `SCK`/`TX`/`RX`/`CS` respectively.  These can be overridden for
     your board/setup:
      - `-DSD_TX=<gpio pin>`
      - `-DSD_RX=<gpio pin>`
      - `-DSD_SCK=<gpio pin>`
      - `-DSD_CS=<gpio pin>`
      - `-DSD_MHZ=<integer speed in MHz>`
   * `-DMEMSIZE=<size in KB>`: The maximum practical size is about
     208KB, but values between 128 and 208 should work on a RP2040.
     Note that although apps and Mac OS seem to gracefully detect free
     memory, these products never existed and some apps might behave
     strangely.
      - With the `Mac Plus` ROM, a _Mac 128K_ doesn't quite have
        enough memory to run _MacPaint_.  So, 192 or 208 (and a
        writeable boot volume on SD) will allow _MacPaint_ to run.
      - **NOTE**: When this option is used, the ROM image must be
          built with an `umac` build with a corresponding `MEMSIZE`

Tip: `cmake` caches these variables, so if you see weird behaviour
having built previously and then changed an option, delete the `build`
directory and start again.

## ROM image

The flow is to use `umac` built on your workstation (e.g. Linux,
but WSL may work too) to prepare a patched ROM image.

`umac` is passed the 4D1F8172 MacPlusv3 ROM, and `-W` to write the
post-patching binary out:

```
./external/umac/main -r '4D1F8172 - MacPlus v3.ROM' -W rom.bin
```

Note: Again, remember that if you are using the `-DMEMSIZE` option to
increase the `pico-umac` memory, you will need to create this ROM
image with a `umac` built with the corresponding `MEMSIZE` option, as
above.

## Disc image

If you don't build SD support, an internal read-only disc image is
stored in flash.  If you do build SD support, you have the option to
still include an image in flash, and this is used as a fallback if
SD boot fails.

Grab a Macintosh system disc from somewhere.  A 400K or 800K floppy
image works just fine, up to System 3.2 (the last version to support
Mac128Ks).  I've used images from
<https://winworldpc.com/product/mac-os-0-6/system-3x> but also check
the various forums and MacintoshRepository.  See the `umac` README for
info on formats (it needs to be raw data without header).

The image size can be whatever you have space for in flash (typically
about 1.3MB is free there), or on the SD card.  (I don't know what the
HFS limits are.  But if you make a 50MB disc you're unlikely to fill
it with software that actually works on the _Mac 128K_ :) )

If using an SD card, use a FAT-formatted card and copy your disc image
into _one_ of the following files in the root of the card:

   * `umac0.img`:  A normal read/write disc image
   * `umac0ro.img`:  A read-only disc image

## Putting it together, and building

Given the `rom.bin` prepared above and a `disc.bin` destinated for
flash, you can now generate includes from them and perform the build:

```
mkdir incbin
xxd -i < rom.bin > incbin/umac-rom.h

# When using an internal disc image:
xxd -i < disc.bin > incbin/umac-disc.h
# OR, if using SD and if you do _not_ want an internal image:
echo > incbin/umac-disc.h

make -C build
```

You'll get a `build/firmware.uf2` out the other end.  Flash this to
your Pico: e.g. plug it in with button held/drag/drop.  (When
iterating/testing during development, unplugging the OTG cable each
time is a pain – I ended up moving to SWD probe programming.)

The LED should flash at about 2Hz once powered up.

# Hardware contruction

It's a simple circuit in terms of having few components: just the
Pico, with three series resistors and a VGA connection, and DC power.
However, if you're not comfortable soldering then don't choose this as
your first project: I don't want you to zap your mouse, keyboard,
monitor, SD cards...

Disclaimer: This is a hardware project with zero warranty.  All due
care has been taken in design/docs, but if you choose to build it then
I disclaim any responsibility for your hardware or personal safety.

With that out of the way...

## Theory of operation

Three 3.3V GPIO pins are driven by PIO to give VSYNC, HSYNC, and video
out signals.

The syncs are in many similar projects driven directly from GPIO, but
here I suggest a 66Ω series resistor on each in order to keep the
voltages at the VGA end (presumably into 75Ω termination?) in the
correct range.

For the video output, one GPIO drives R,G,B channels for mono/white
output.  A 100Ω resistor gives roughly 0.7V (max intensity) into 3*75Ω
signals.

That's it... power in, USB adapter.

## Pinout and circuit

Parts needed:

   * Pico/RP2040 board
   * USB OTG micro-B to A adapter
   * USB keyboard, mouse (and hub, if not integrated)
   * 5V DC supply (600mA+), and maybe a DC jack
   * 100Ω resistor
   * 2x 66Ω resistors
   * VGA DB15 connector, or janky chopped VGA cable
   * (optional) SD card breakout, SD card

If you want to get fancy with an SD card, you will need some kind of
SD card SPI breakout adapter.  (There are a lot of these around, but
many seem to have a buffer/level-converter for 5V operation.  Find one
without, or modify your adapter for a 3.3V supply.  Doing so, and
finding an SD card that works well with SPI is out of scope of this
doc.)

Pins are given for a RPi Pico board, but this will work on any RP2040
board with 2MB+ flash as long as all required GPIOs are pinned out:

| GPIO/pin     | Pico pin     | Usage          |
| ------------ | ------------ | -------------- |
|   GP0        | 1            | UART0 TX       |
|   GP1        | 2            | UART0 RX       |
|   GP18       | 24           | Video output   |
|   GP19       | 25           | VSYNC          |
|   GP21       | 27           | HSYNC          |
|   Gnd        | 23, 28       | Video ground   |
|   VBUS (5V)  | 40           | +5V supply     |
|   Gnd        | 38           | Supply ground  |

Method:

   * Wire 5V supply to VBUS/Gnd
   * Video output --> 100Ω --> VGA RGB (pins 1,2,3) all connected together
   * HSYNC --> 66Ω --> VGA pin 13
   * VSYNC --> 66Ω --> VGA pin 14
   * Video ground --> VGA grounds (pins 5-8, 10)

If you don't have exactly 100Ω, using slightly more is OK but display
will be dimmer.  If you don't have 66Ω for the syncs, connecting them
directly is "probably OK", but YMMV.

If you are including an SD card, the default pinout is as follows
(this can be changed at build time, above):

| GPIO/pin     | Pico pin     | Usage          |
| ------------ | ------------ | -------------- |
|   GP2        | 4            | SPI0 SCK       |
|   GP3        | 5            | SPI0 TX (MOSI) |
|   GP4        | 6            | SPI0 RX (MISO) |
|   GP5        | 7            | SPI0 /CS       |

(The SD card needs a good ground, e.g. Pico pin 8 nearby, and 3.3V
supply from Pico pin 36.)

If your SD breakout board is "raw", i.e. has no buffer or series
resistors on-board, you may find adding a 66Ω resistor in series on
all of the four signal lines will help.  Supply decoupling caps will
also be important (e.g. 1uF+0.1uF) to keep the SD card happy.  _Keep
SD card wiring short._ The default SPI clock (5MHz) is
conservative/slow, but I suggest verifying the circuit/SD card works
before increasing it.

Test your connections: the key part is not getting over 0.7V into your
VGA connector's signals, or shorting SD card pins.

Connect USB mouse, and keyboard if you like, and power up.

# Software

Both CPU cores are used, and are overclocked (blush) to 250MHz so that
Missile Command is enjoyable to play.

The `umac` emulator and video output runs on core 1, and core 0 deals
with USB HID input.  Video DMA is initialised pointing to the
framebuffer in the Mac's RAM.

Other than that, it's just a main loop in `main.c` shuffling things
into `umac`.

Quite a lot of optimisation has been done in `umac` and `Musashi` to
get performance up on Cortex-M0+ and the RP2040, like careful location
of certain routines in RAM, ensuring inlining/constants can be
foldeed, etc.  It's 5x faster than it was at the beginning.

The top-level project might be a useful framework for other emulators,
or other projects that need USB HID input and a framebuffer (e.g. a
VT220 emulator!).

The USB HID code is largely stolen from the TinyUSB example, but shows
how in practice you might capture keypresses/deal with mouse events.

## Video

The video system is pretty good and IMHO worth stealing for other
projects: It uses one PIO state machine and 3 DMA channels to provide
a rock-solid bitmapped 1BPP 640x480 video output.  The Mac 512x342
framebuffer is centred inside this by using horizontal blanking
regions (programmed into the line scan-out) and vertical blanking
areas from a dummy "always black" mini-framebuffer.

It supports (at build time) flexible resolutions/timings.  The one
caveat (or advantage?) is that it uses an HSYNC IRQ routine to
recalculate the next DMA buffer pointer; doing this at scan-time costs
about 1% of the CPU time (on core 1).  However, it could be used to
generate video on-the-fly from characters/tiles without a true
framebuffer.

I'm considering improvements to the video system:

   * Supporting multiple BPP/colour output
   * Implement the rest of `DE`/display valid strobe support, making
     driving LCDs possible.
   * Using a video DMA address list and another DMA channel to reduce
     the IRQ frequency (CPU overhead) to per-frame, at the cost of a
     couple of KB of RAM.


# Licence

`hid.c` and `tusb_config.h` are based on code from the TinyUSB
project, which is Copyright (c) 2019, 2021 Ha Thach (tinyusb.org) and
released under the MIT licence.  `sd_hw_config.c` is based on code
from the no-OS-FatFS-SD-SPI-RPi-Pico project, which is Copyright (c)
2021 Carl John Kugler III.

The remainder of the code is released under the MIT licence:

 Copyright (c) 2024 Matt Evans:

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.

