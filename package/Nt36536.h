// SPDX-License-Identifier: Apache-2.0
// NT36536 (in-cell, host download, firmware-reported coordinates) on top of the NvtProtocol transport. Register and format
// facts follow the study of the OnePlus nt36536_noflash driver (no code copied); see docs/erhai-touch-facts.md. The bus,
// the download sequence and the ILM/DLM CRC are NvtProtocol's; only identification, the memory map, the firmware's
// section walk and the coordinate report differ from the NT36532E. No board addresses or clocks here.
#ifndef NT36536_H
#define NT36536_H
#include "NvtProtocol.h"
#include "NvtTouchCore.h"

#define NT36536_POINTS 10
#define NT36536_REPORT_LEN 121           // [0] = address phase slot (zero), [1..120] = event buffer bytes 0..119
#define NT36536_HIRES 0xF1               // event-buffer protocol byte selecting 16-bit coordinates

extern const struct nvt_map nt36536_map;

// Bootloader reset, read the trim id and accept the two NT36536 id patterns. Selects nt36536_map.
int nt36536_identify(struct nvt_chip *c);
// Partition walk with the NT36536 header end: the first info entry that has a size and no file offset ends the header
// (doubled for an image with a cascade second header). Fills fw for nvt_download_firmware.
int nt36536_fw_parse(struct nvt_fw *fw, const uint8_t *data, size_t size);

struct nt36536_info {
    uint8_t fw_ver, fw_ver_bar, x_num, y_num, protocol, sub_ver;
    uint16_t abs_x_max, abs_y_max, pid;
};
// Firmware info block at the event buffer + 0x78 (read once after the download).
int nt36536_get_info(struct nvt_chip *c, struct nt36536_info *info);
// One report: NT36536_REPORT_LEN bytes into buf (buf[0] = 0, then the event buffer).
int nt36536_read_report(struct nvt_chip *c, uint8_t *buf);

// Result of decoding a report.
#define NT36536_REPORT_OK 0
#define NT36536_REPORT_CHECKSUM (-1)     // finger checksum (buf[65]) does not match
#define NT36536_REPORT_ESD (-2)          // bytes 1..6 all 0x77: the firmware asks for recovery
#define NT36536_REPORT_WDT (-3)          // bytes 1..6 all 0xFD or 0xFE: watchdog reset, firmware must be downloaded again
// Contacts down (status 1 enter, 2 move) in firmware units: x along the firmware's x axis, y along its y axis.
// *count gets the number written to out (at most max).
int nt36536_decode(const uint8_t *buf, int hires, struct nvt_contact *out, unsigned max, unsigned *count);
#endif
