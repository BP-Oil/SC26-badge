cmake_minimum_required(VERSION 3.13)

# SC26 board: RP2350 (ARM), 16 MB QSPI flash.
set(PICO_PLATFORM rp2350-arm-s)
set(PICO_BOARD pico2)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(sc26_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# Shared board support: display, LEDs, buttons
add_library(sc26_board STATIC
    common/lcd.c
    common/leds.c
    common/buttons.c
)
pico_generate_pio_header(sc26_board ${CMAKE_CURRENT_LIST_DIR}/common/qspi_lcd.pio)
pico_generate_pio_header(sc26_board ${CMAKE_CURRENT_LIST_DIR}/common/ws2812.pio)
target_include_directories(sc26_board PUBLIC common)
target_compile_definitions(sc26_board PUBLIC PICO_FLASH_SIZE_BYTES=16777216)
target_link_libraries(sc26_board PUBLIC
    pico_stdlib hardware_pio hardware_dma hardware_pwm hardware_irq hardware_clocks)

# ---- Phase 1: hardware probe ------------------------------------------------
add_executable(sc26_probe probe/main.c)
target_link_libraries(sc26_probe sc26_board)
pico_enable_stdio_usb(sc26_probe 1)
pico_enable_stdio_uart(sc26_probe 0)
pico_set_program_name(sc26_probe "SC26 probe")
pico_set_program_version(sc26_probe "1.0")
pico_add_extra_outputs(sc26_probe)

# ---- Phase 2: GIF player ----------------------------------------------------
set(ANIMATEDGIF_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/AnimatedGIF/src)
add_executable(sc26_gif
    gifplayer/main.c
    gifplayer/gif_player.c
    gifplayer/ambient.c
    gifplayer/usb_glue.c
    gifplayer/usb_descriptors.c
    gifplayer/diskio.c
    gifplayer/settings.c
    gifplayer/nes_cache.c
    gifplayer/chain.c
    gifplayer/boot_splash.c
    gifplayer/help.c
    nes/nes_platform.cpp
    lib/infones/InfoNES.cpp
    lib/infones/InfoNES_Mapper.cpp
    lib/infones/InfoNES_pAPU.cpp
    lib/infones/K6502.cpp
    gifplayer/fatfs/ff.c
    gifplayer/fatfs/ffunicode.c
    common/flash_disk.c
)
target_include_directories(sc26_gif PRIVATE gifplayer nes lib/infones ${ANIMATEDGIF_DIR})
set_source_files_properties(nes/nes_platform.cpp lib/infones/InfoNES.cpp lib/infones/InfoNES_Mapper.cpp
    lib/infones/InfoNES_pAPU.cpp lib/infones/K6502.cpp PROPERTIES COMPILE_OPTIONS "-O2;-w")
if(SC26_PLAY_MIN_MS)   # test builds only: shorten the auto-advance time
    target_compile_definitions(sc26_gif PRIVATE PLAY_MIN_MS=${SC26_PLAY_MIN_MS})
endif()
target_compile_definitions(sc26_gif PRIVATE MAX_WIDTH=1024)   # widest GIF the decoder accepts
target_compile_options(sc26_gif PRIVATE -O2)
set_source_files_properties(gifplayer/gif_player.c PROPERTIES COMPILE_OPTIONS "-O3;-Wno-unused-function;-Wno-unused-variable")
target_link_libraries(sc26_gif sc26_board
    tinyusb_device tinyusb_board pico_unique_id pico_bootrom hardware_flash hardware_sync m)
pico_enable_stdio_usb(sc26_gif 0)
pico_enable_stdio_uart(sc26_gif 0)
pico_set_program_name(sc26_gif "SC26 OS")
pico_set_program_version(sc26_gif "2.0")
pico_add_extra_outputs(sc26_gif)
