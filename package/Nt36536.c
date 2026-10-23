// SPDX-License-Identifier: Apache-2.0
// See Nt36536.h and NOTICE.
#include "Nt36536.h"
#include <string.h>

#define NT36536_CHIP_VER_TRIM 0x1FB104
#define NT36536_SWRST_SIF 0x1FB43E
#define NT36536_ENG_RST 0x7FFF80
#define NT36536_FWINFO 0x78

// NT36536_single_memory_map; the cascade map of this chip is identical, so ENB_CASC does not change it.
const struct nvt_map nt36536_map = {
    .name="NT36536", .event_buf=0x130900, .boot_rdy=0x1FB50D, .tx_auto_copy_en=0x1FC825, .spi_dma_tx_info=0x1FC814,
    .ilm_des=0x1FCC30, .ilm_length=0x1FCC34, .dlm_des=0x1FCC40, .dlm_length=0x1FCC44,
    .g_ilm_checksum=0x1FCC38, .g_dlm_checksum=0x1FCC48, .r_ilm_checksum=0x1FCC7C, .r_dlm_checksum=0x1FCC80,
    .bld_ilm_dlm_crc=0x1FB532, .enb_casc={0x1FB12C,0x01}};

static void delay(struct nvt_chip *c, unsigned ms) {if (c->bus->delay_ms) c->bus->delay_ms(c->bus->ctx, ms);}

// Trim id patterns: {0x14, x, x, 0x23, 0x65, 0x03} and {0x15, 0x0E, x, 0x23, 0x65, 0x03}.
static int id_matches(const uint8_t *id) {
    if (id[3] != 0x23 || id[4] != 0x65 || id[5] != 0x03) return 0;
    return id[0] == 0x14 || (id[0] == 0x15 && id[1] == 0x0E);
}

int nt36536_identify(struct nvt_chip *c) {
    c->chip_ver_trim = NT36536_CHIP_VER_TRIM; c->swrst_sif = NT36536_SWRST_SIF; c->hw_crc_len = 3;
    // The vendor flow does not reset the engine first; the reset is harmless and gives the same start state as NvtProtocol.
    int r = nvt_write_byte(c, NT36536_ENG_RST, 0x5A); if (r) return r;
    delay(c, 11);
    static const uint8_t zero[6] = {0};
    uint8_t id[6] = {0};
    for (unsigned retry = 0; retry < 5; ++retry) {
        if ((r = nvt_write_byte(c, c->swrst_sif, 0x69))) return r;   // bootloader reset
        delay(c, 5);
        // A 7-byte write at the trim address (offset with the write bit and six zero bytes), then the 6-byte id read.
        if ((r = nvt_write(c, c->chip_ver_trim, zero, sizeof zero))) return r;
        if ((r = nvt_read(c, c->chip_ver_trim, id, sizeof id))) return r;
        memcpy(c->id, id, sizeof id);
        if (id_matches(id)) break;
        delay(c, 10);
    }
    if (!id_matches(id)) return NVT_ERR_CHIP;
    uint8_t casc = 0;
    if ((r = nvt_read(c, nt36536_map.enb_casc.addr, &casc, 1))) return r;
    c->cascade = (casc & nt36536_map.enb_casc.mask) == 0;
    c->map = &nt36536_map;
    return 0;
}

static int word_at(const uint8_t *d, size_t size, size_t pos, uint32_t *out) {
    if (pos > size || size - pos < 4) return NVT_ERR_FW;
    *out = (uint32_t)d[pos] | ((uint32_t)d[pos + 1] << 8) | ((uint32_t)d[pos + 2] << 16) | ((uint32_t)d[pos + 3] << 24);
    return 0;
}

int nt36536_fw_parse(struct nvt_fw *fw, const uint8_t *d, size_t size) {
    memset(fw, 0, sizeof *fw); fw->data = d; fw->size = size;
    uint32_t first;
    if (size < 0x40 || word_at(d, size, 0, &first) || first < 0x30 || first > size) return NVT_ERR_FW;
    // Header end: the info entry (sram, size, bin, crc) that has a size and starts at file offset 0 is the bin header,
    // and the info table ends after it.
    uint32_t end = first;
    for (uint32_t pos = 0x30; pos + 0x10 <= first; pos += 0x10) {
        uint32_t sz, bin;
        if (word_at(d, size, pos + 4, &sz) || word_at(d, size, pos + 8, &bin)) return NVT_ERR_FW;
        if (sz && !bin) {end = pos + 0x10; break;}
    }
    fw->cascade_2nd_header = (d[0x20] & 0x02) >> 1;
    if (fw->cascade_2nd_header) end *= 2;
    if (end > size) return NVT_ERR_FW;
    unsigned info_sec_num = 0;
    for (uint32_t pos = 0x30; pos < (fw->cascade_2nd_header ? end / 2 : end); pos += 0x10) ++info_sec_num;
    if (fw->cascade_2nd_header) info_sec_num += 1;
    const unsigned ovly_sec_num = (d[0x28] & 0x10) ? (d[0x28] & 0x0f) : 0, ilm_dlm_num = 2;
    const unsigned partition = ilm_dlm_num + ovly_sec_num + info_sec_num;
    if (partition > NVT_MAX_PARTITIONS) return NVT_ERR_FW;
    fw->count = partition;
    int find_bin_header = 0;
    uint32_t pos = 0x30;
    for (unsigned list = 0; list < partition; ++list) {
        struct nvt_fw_partition *p = &fw->part[list];
        if (list < ilm_dlm_num) {
            if (word_at(d, size, list * 12, &p->bin_addr) || word_at(d, size, 4 + list * 12, &p->sram_addr) ||
                word_at(d, size, 8 + list * 12, &p->size)) return NVT_ERR_FW;
            if ((uint64_t)p->bin_addr + p->size > size) return NVT_ERR_FW;
            p->crc = nvt_crc32_words(d + p->bin_addr, p->size, size - p->bin_addr);
            memcpy(p->name, list == 0 ? "ILM" : "DLM", 4);
        } else if (list < ilm_dlm_num + info_sec_num) {
            if (!find_bin_header) pos = 0x30 + 0x10 * (list - ilm_dlm_num);
            else if (fw->cascade_2nd_header) pos = end - 0x10;
            if (word_at(d, size, pos, &p->sram_addr) || word_at(d, size, pos + 4, &p->size) || word_at(d, size, pos + 8, &p->bin_addr) ||
                word_at(d, size, pos + 12, &p->crc)) return NVT_ERR_FW;
            if (p->bin_addr < end && p->size) {memcpy(p->name, "Header", 7); find_bin_header = 1;}
            else memcpy(p->name, "Info", 5);
        } else {
            pos = fw->part[1].bin_addr + 0x10 * (list - ilm_dlm_num - info_sec_num);
            if (word_at(d, size, pos, &p->sram_addr) || word_at(d, size, pos + 4, &p->size) || word_at(d, size, pos + 8, &p->bin_addr) ||
                word_at(d, size, pos + 12, &p->crc)) return NVT_ERR_FW;
            memcpy(p->name, "Ovly", 5);
        }
        if ((uint64_t)p->bin_addr + p->size > size) return NVT_ERR_FW;
    }
    // The image's own ILM/DLM checksums (0x18, 0x1C) must equal the host CRC: a wrong walk or a damaged image stops here.
    uint32_t ilm_crc, dlm_crc;
    if (word_at(d, size, 0x18, &ilm_crc) || word_at(d, size, 0x1C, &dlm_crc) || ilm_crc != fw->part[0].crc || dlm_crc != fw->part[1].crc)
        return NVT_ERR_FW;
    return 0;
}

int nt36536_get_info(struct nvt_chip *c, struct nt36536_info *info) {
    uint8_t b[39];
    memset(info, 0, sizeof *info);
    for (int attempt = 0; attempt < 4; ++attempt) {
        int r = nvt_read(c, c->map->event_buf + NT36536_FWINFO, b, sizeof b); if (r) return r;
        info->fw_ver = b[0]; info->fw_ver_bar = b[1];
        if ((uint8_t)(b[0] + b[1]) != 0xff) {delay(c, 10); continue;}
        info->x_num = b[2]; info->y_num = b[3];
        info->abs_x_max = (uint16_t)((b[4] << 8) | b[5]); info->abs_y_max = (uint16_t)((b[6] << 8) | b[7]);
        info->protocol = b[12]; info->sub_ver = b[13];
        info->pid = (uint16_t)((b[35] << 8) | b[34]);
        return 0;
    }
    return NVT_ERR_FW;
}

int nt36536_read_report(struct nvt_chip *c, uint8_t *buf) {
    buf[0] = 0;
    return nvt_read(c, c->map->event_buf, buf + 1, NT36536_REPORT_LEN - 1);
}

static int all_bytes(const uint8_t *b, unsigned n, uint8_t v) {for (unsigned i = 0; i < n; ++i) if (b[i] != v) return 0; return 1;}

int nt36536_decode(const uint8_t *buf, int hires, struct nvt_contact *out, unsigned max, unsigned *count) {
    *count = 0;
    if (all_bytes(buf + 1, 6, 0x77)) return NT36536_REPORT_ESD;
    if (all_bytes(buf + 1, 6, 0xFD) || all_bytes(buf + 1, 6, 0xFE)) return NT36536_REPORT_WDT;
    uint8_t sum = 0;
    for (unsigned i = 1; i <= 64; ++i) sum = (uint8_t)(sum + buf[i]);
    if ((uint8_t)(0u - sum) != buf[65]) return NT36536_REPORT_CHECKSUM;
    for (unsigned i = 0; i < NT36536_POINTS && *count < max; ++i) {
        const uint8_t *p = buf + 1 + 6 * i;
        const int id = (p[0] >> 3) - 1, status = p[0] & 7;
        if (id < 0 || id >= NT36536_POINTS || (status != 1 && status != 2)) continue;
        struct nvt_contact *o = &out[(*count)++];
        memset(o, 0, sizeof *o);
        o->slot = id; o->tracking_id = id;
        if (hires) {
            o->x = (p[1] << 8) | p[2]; o->y = (p[3] << 8) | p[4]; o->area = p[5];
        } else {
            o->x = (p[1] << 4) | (p[3] >> 4); o->y = (p[2] << 4) | (p[3] & 0x0F); o->area = p[4];
        }
        o->peak = o->weight = 100;
    }
    return NT36536_REPORT_OK;
}
