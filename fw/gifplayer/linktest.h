// Badge-to-badge link plumbing used by 2 PLAYER: restarting as the USB host (PLAYER 1) and the
// host-side serial port. Also a LINK TEST tool (dock-pin finder, TX blinker, UART and USB link
// tests) that is no longer in the menu but is kept for hardware bring-up.
#pragma once
#define LINKTEST_HOST_MAGIC 0x55534231u          // "USB1" in watchdog scratch 0: boot as PLAYER 1
void linktest_run(void (*svc)(void));      // returns when the user presses B or holds CENTER
void linktest_usb_host_run(void);          // PLAYER 1 link-test screen; returns when the user exits
void linktest_become_player1(void);        // save state and restart in USB host mode (no return)
int  linktest_host_cdc(void);              // host mode: TinyUSB CDC index of PLAYER 2, or -1
