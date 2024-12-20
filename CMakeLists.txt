# CMakeLists
#
# MIT License
#
# Copyright (c) 2021, 2024 Matt Evans
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
#
cmake_minimum_required(VERSION 3.13)

# Options that should be defined when initialising the build
# directory with cmake, e.g. "cmake .. -DOPTION=true":
#

option(USE_SD "Build in SD support" OFF)
set(SD_TX 3 CACHE STRING "SD SPI TX pin")
set(SD_RX 4 CACHE STRING "SD SPI RX pin")
set(SD_SCK 2 CACHE STRING "SD SPI SCK pin")
set(SD_CS 5 CACHE STRING "SD SPI CS pin")
set(SD_MHZ 5 CACHE STRING "SD SPI speed in MHz")
option(USE_VGA_RES "Video uses VGA (640x480) resolution" OFF)
set(VIDEO_PIN 18 CACHE STRING "Video GPIO base pin (followed by VS, CLK, HS)")

# See below, -DMEMSIZE=<size in KB> will configure umac's memory size,
# overriding defaults.

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(firmware)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# For TUSB host stuff:
set(FAMILY rp2040)
set(BOARD raspberry_pi_pico)

set(TINYUSB_PATH ${PICO_SDK_PATH}/lib/tinyusb)

# umac subproject (and Musashi sub-subproject)
set(UMAC_PATH ${CMAKE_CURRENT_SOURCE_DIR}/external/umac)
set(UMAC_MUSASHI_PATH ${UMAC_PATH}/external/Musashi)
set(UMAC_INCLUDE_PATHS ${UMAC_PATH}/include ${UMAC_MUSASHI_PATH})

# This isn't very nice, but hey it's Sunday :p
set(UMAC_SOURCES
  ${UMAC_PATH}/src/disc.c
  ${UMAC_PATH}/src/main.c
  ${UMAC_PATH}/src/rom.c
  ${UMAC_PATH}/src/scc.c
  ${UMAC_PATH}/src/via.c
  ${UMAC_MUSASHI_PATH}/m68kcpu.c
  ${UMAC_MUSASHI_PATH}/m68kdasm.c
  ${UMAC_MUSASHI_PATH}/m68kops.c
  ${UMAC_MUSASHI_PATH}/softfloat/softfloat.c
  )

set(MEMSIZE 128 CACHE STRING "Memory size, in KB")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -DPICO -DMUSASHI_CNF=\\\"../include/m68kconf.h\\\" -DUMAC_MEMSIZE=${MEMSIZE}")

if (USE_SD)
   add_compile_definitions(USE_SD=1)
   set(FF_DISABLE_RTC ${PICO_RP2350})  # RP2350 doesn't have RTC, so disable it
   add_subdirectory(external/no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI build)
   set(EXTRA_SD_SRC src/sd_hw_config.c)
   set(EXTRA_SD_LIB FatFs_SPI)
   add_compile_definitions(SD_TX=${SD_TX} SD_RX=${SD_RX} SD_SCK=${SD_SCK} SD_CS=${SD_CS} SD_MHZ=${SD_MHZ})
endif()

if (USE_VGA_RES)
   add_compile_definitions(USE_VGA_RES=1)
   add_compile_definitions(DISP_WIDTH=640)
   add_compile_definitions(DISP_HEIGHT=480)
else()
   add_compile_definitions(DISP_WIDTH=512)
   add_compile_definitions(DISP_HEIGHT=342)
endif()
add_compile_definitions(GPIO_VID_BASE=${VIDEO_PIN})

if (TARGET tinyusb_device)
  add_executable(firmware
    src/main.c
    src/video.c
    src/kbd.c
    src/hid.c
    ${EXTRA_SD_SRC}

    ${UMAC_SOURCES}
    )

  # The umac sources need to prepare Musashi (some sources are generated):
  add_custom_command(OUTPUT ${UMAC_MUSASHI_PATH}/m68kops.c
    COMMAND echo "*** Preparing umac source ***"
    COMMAND make -C ${UMAC_PATH} prepare
    )
  add_custom_target(prepare_umac
    DEPENDS ${UMAC_MUSASHI_PATH}/m68kops.c
    )
  add_dependencies(firmware prepare_umac)

  target_link_libraries(firmware
    pico_stdlib
    pico_multicore
    tinyusb_host
    tinyusb_board
    hardware_dma
    hardware_pio
    hardware_sync
    ${EXTRA_SD_LIB}
    )

  target_include_directories(firmware PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${TINYUSB_PATH}/hw
    ${TINYUSB_PATH}/src
    ${UMAC_INCLUDE_PATHS}
    incbin
    )

  pico_generate_pio_header(firmware ${CMAKE_CURRENT_LIST_DIR}/src/pio_video.pio)

  pico_enable_stdio_uart(firmware 1)

  # Needed for UF2:
  pico_add_extra_outputs(firmware)

elseif(PICO_ON_DEVICE)
   message(WARNING "not building firmware because TinyUSB submodule is not initialized in the SDK")
endif()

