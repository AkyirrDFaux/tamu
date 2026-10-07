// TinyUSB configuration for the Valu v2.0 bootloader (CH32V203G8R6, USBFS device).
// Everything is kept to the single CDC-ACM function on the USBFS controller
// (roothub port 1; CH32V20x port 0 is the separate FSDEV controller).
//
// This file is found by TinyUSB because `include/` is on the build's include path:
// the `Valu_bootloader` env adds `-Iinclude` to build_flags, since PlatformIO scopes
// CPPPATH per library and the project's include/ dir is NOT on the path while the
// TinyUSB library itself is compiled. `platformio.ini` carries that comment too.
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// MCU / OS / roothub
//--------------------------------------------------------------------
#define CFG_TUSB_MCU            OPT_MCU_CH32V20X
#define CFG_TUSB_OS             OPT_OS_NONE

// The CH32V20x has two USB device controllers. TinyUSB's device default for this MCU is the
// FSDEV/USBD controller on **roothub port 0** (port 1 is the separate USBFS/OTG controller, which
// needs its own 48 MHz source via RCC_CFGR2.USBFSSRC). The project's previous working Valu v2
// release used port 0, and so does this bootloader: select it and let TinyUSB's defaults stand
// (CFG_TUD_WCH_USBIP_FSDEV defaults to 1 for CH32V20X when the USBFS override is absent).
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE

#define CFG_TUD_ENABLED         1
#define CFG_TUH_ENABLED         0

// NOTE: no CFG_TUD_WCH_USBIP_USBFS/FSDEV override here on purpose. Setting USBFS=1 (as the
// feasibility spike did) steers TinyUSB at the OTG controller instead, and that pairing is not
// what this board is wired/clocked for.

//--------------------------------------------------------------------
// Device: full speed, one CDC-ACM function
//--------------------------------------------------------------------
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             1
#define CFG_TUD_CDC_RX_BUFSIZE  64
#define CFG_TUD_CDC_TX_BUFSIZE  64
#define CFG_TUD_CDC_EP_BUFSIZE  64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
