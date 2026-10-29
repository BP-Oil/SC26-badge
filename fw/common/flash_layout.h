// SC26 16 MB flash map (Phase 3). All offsets are from the start of flash (XIP_BASE = 0x10000000).
//
//   0x000000 +1.0 MB   SC26 OS (GIF player, launcher, NES emulator)       <- normal boot image
//   0x100000 +1.5 MB   ORIGINAL "Wayfarer" firmware, chain-booted from the menu
//   0x280000 +508 KB   NES ROM cache (the selected game is copied here; runs in place)
//   0x2FF000 +4 KB     settings (last GIF, STAY, brightness, zoom, LEDs)
//   0x300000 +512 KB   DOOM engine image, chain-booted from the menu
//   0x380000 +6.0 MB   DOOM game data (WHD converted from DOOM.WAD)
//   0x980000 +6.5 MB   "SC26 GIFS" USB drive (FAT): GIFs + GAMES folder
#pragma once

#define FLASH_TOTAL_SIZE   (16u << 20)

#define OS_OFFSET          0x000000u
#define OS_SIZE            0x100000u

#define ORIG_OFFSET        0x100000u
#define ORIG_SIZE          0x180000u

#define NESCACHE_OFFSET    0x280000u
#define NESCACHE_SIZE      0x07F000u

#define SETTINGS_OFFSET    0x2FF000u

#define DOOM_OFFSET        0x300000u          // chain-boot region starts here
#define DOOM_IMAGE_SIZE    0x080000u
#define DOOM_WHD_OFFSET    (DOOM_OFFSET + DOOM_IMAGE_SIZE)
#define DOOM_SIZE          0x680000u          // image + WHD, ends at 0x980000

#define FS_FLASH_OFFSET    0x980000u
#define FS_FLASH_SIZE      (FLASH_TOTAL_SIZE - FS_FLASH_OFFSET)   // 6.5 MB
