// SPDX-License-Identifier: Apache-2.0
// NT36536 section walk on the real firmware image (when present) and the report decoder (package/Nt36536.c).
//   cc -I.. -I../../drivers/NativeNovatekTouch -o /tmp/nt36536 nt36536.c ../Nt36536.c ../../drivers/NativeNovatekTouch/NvtProtocol.c && /tmp/nt36536
#include "../Nt36536.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t image[0x80000];

static void report_checksum(uint8_t *b) {
    uint8_t sum = 0;
    for (unsigned i = 1; i <= 64; ++i) sum = (uint8_t)(sum + b[i]);
    b[65] = (uint8_t)(0u - sum);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "../../.cache/firmware/touch/FW_NT36536_HUAXING_FAE.img";
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(image, 1, sizeof image, f);
        fclose(f);
        struct nvt_fw fw;
        assert(nt36536_fw_parse(&fw, image, n) == 0);
        assert(fw.cascade_2nd_header == 1 && fw.count == 16);
        // ILM, DLM and the two bin headers are the only sections with data.
        static const struct {uint32_t sram, size, bin;} want[4] = {
            {0x000000, 0x1FE43, 0x250}, {0x100000, 0x159F7, 0x22000}, {0x1159F8, 0xFF, 0x000}, {0x115AF8, 0xFF, 0x100}};
        unsigned k = 0;
        for (unsigned i = 0; i < fw.count; ++i) {
            if (!fw.part[i].size) continue;
            assert(k < 4 && fw.part[i].sram_addr == want[k].sram && fw.part[i].size == want[k].size && fw.part[i].bin_addr == want[k].bin);
            ++k;
        }
        assert(k == 4);
        // sheng's walk (header end = word 0) runs into the block at 0x200 and refuses the image.
        assert(nvt_fw_parse(&fw, image, n) == NVT_ERR_FW);
        image[0x300] ^= 1;   // inside ILM: the image's own CRC no longer matches
        assert(nt36536_fw_parse(&fw, image, n) == NVT_ERR_FW);
        printf("firmware walk: ok (%s)\n", path);
    } else {
        printf("firmware walk: skipped (no %s)\n", path);
    }

    uint8_t b[NT36536_REPORT_LEN] = {0};
    struct nvt_contact c[NT36536_POINTS];
    unsigned count = 0;
    b[1] = (3 << 3) | 1; b[2] = 0x12; b[3] = 0x34; b[4] = 0x56; b[5] = 0x78; b[6] = 9;   // id 2 enters at (0x1234, 0x5678)
    b[7] = (5 << 3) | 2; b[8] = 0x5D; b[9] = 0xBF; b[10] = 0x84; b[11] = 0x7F;            // id 4 moves at (23999, 33919)
    b[13] = (6 << 3) | 3;                                                                  // id 5: status 3 is not a finger down
    report_checksum(b);
    assert(nt36536_decode(b, 1, c, NT36536_POINTS, &count) == NT36536_REPORT_OK && count == 2);
    assert(c[0].tracking_id == 2 && c[0].x == 0x1234 && c[0].y == 0x5678 && c[0].area == 9);
    assert(c[1].tracking_id == 4 && c[1].x == 23999 && c[1].y == 33919);
    assert(nt36536_decode(b, 1, c, 1, &count) == NT36536_REPORT_OK && count == 1);
    // Legacy 12-bit layout of the same bytes: x = b1 << 4 | b3 >> 4, y = b2 << 4 | b3 & 15.
    assert(nt36536_decode(b, 0, c, NT36536_POINTS, &count) == NT36536_REPORT_OK && c[0].x == 0x125 && c[0].y == 0x346);
    b[65] ^= 1;
    assert(nt36536_decode(b, 1, c, NT36536_POINTS, &count) == NT36536_REPORT_CHECKSUM && count == 0);
    memset(b + 1, 0x77, 6);
    assert(nt36536_decode(b, 1, c, NT36536_POINTS, &count) == NT36536_REPORT_ESD);
    memset(b + 1, 0xFE, 6);
    assert(nt36536_decode(b, 1, c, NT36536_POINTS, &count) == NT36536_REPORT_WDT);
    printf("report decode: ok\n");
    return 0;
}
