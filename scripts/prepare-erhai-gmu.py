#!/usr/bin/env python3
"""Generate the gen8 (Adreno 830, SM8750) profile of the sheng GMU board code from pinned inputs; no device I/O.

prepare-sheng-gmu.py and prepare-sheng-gmu-votes.py generate ShengGmuSource.h, ShengGmuVotesSource.h and
ShengGmuCommandDbSource.h for the A740. This writes the same three headers into build/erhai-gmu for erhai, with
SHENG_GMU_GEN8 set, from: the pinned Linux a6xx XML / a6xx_gmu.c (scripts/data/adreno-inputs.json, the A8XX register
variants and the a8xx branches of a6xx_gmu_fw_start), the msm-next A830 catalog entry (chip id, gmu_chipid, gbif_cx, BCMs,
speed bins), the upstream sm8750.dtsi (GPU/GMU register ranges, OPP tables with opp-supported-hw, cmd-db reservation),
the RPMh level names and the local OEM gen80000_gmu.bin (scripts/data/erhai-gpu-inputs.json). Nothing here is board
evidence; the tablet run is."""
import hashlib, json, re, struct, sys, xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHIP_ID = 0x44050001


def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()


def prepare():
    pins = json.loads((ROOT / 'scripts/data/adreno-inputs.json').read_text())
    erhai = json.loads((ROOT / 'scripts/data/erhai-gpu-inputs.json').read_text())
    cache, extra = ROOT / '.cache/adreno-source', ROOT / '.cache/erhai-gpu-source'
    linux = ['drivers/gpu/drm/msm/adreno/a6xx_gmu.c'] + ['drivers/gpu/drm/msm/registers/adreno/' + s for s in ['a6xx.xml', 'a6xx_gmu.xml']]
    for name in linux:
        if sha(cache / name) != pins['linux']['files'][name]['sha256']: raise SystemExit('Pinned source differs: ' + name)
    for name, expected in erhai['files'].items():
        if sha(extra / name) != expected['sha256']: raise SystemExit('Pinned erhai input differs: ' + name)
    firmware = ROOT / '.cache/firmware/erhai-gpu/qcom/gen80000_gmu.bin'
    policy = erhai['firmware']['qcom/gen80000_gmu.bin']
    if sha(firmware) != policy['sha256'] or firmware.stat().st_size != policy['bytes']: raise SystemExit('GMU firmware differs')

    # Registers: the one definition valid on A8XX (no variant, or a variant range that includes A8XX).
    ns = {'r': 'http://nouveau.freedesktop.org/'}
    trees = [ET.parse(cache / name).getroot() for name in linux[1:]]
    def a8xx(variants):
        if variants is None: return True
        return any(v == 'A8XX' or (v.endswith('-') and v[:-1] in ('A6XX', 'A7XX', 'A8XX')) for v in variants.split(','))
    regs = {}
    def register(domain, name, symbol=None):
        found = [n for t in trees for d in t.findall('r:domain', ns) if d.attrib['name'] == domain
                 for n in d.findall('r:reg32', ns) if n.attrib.get('name') == name and a8xx(n.attrib.get('variants'))]
        if len(found) != 1: raise SystemExit('Ambiguous/missing A8XX register ' + name)
        regs['REG_' + (symbol or name)] = int(found[0].attrib['offset'], 0)
        return found[0]
    core = ['GMU_CM3_ITCM_START', 'GMU_CM3_DTCM_START', 'GMU_CM3_SYSRESET', 'GMU_CM3_BOOT_CONFIG', 'GMU_CM3_FW_INIT_RESULT',
            'GMU_CM3_CFG', 'GMU_HFI_QTBL_ADDR', 'GMU_HFI_QTBL_INFO', 'GMU_HFI_CTRL_INIT', 'GMU_HFI_CTRL_STATUS',
            'GMU_HOST2GMU_INTR_SET', 'GMU_SYS_BUS_CONFIG', 'GMU_ICACHE_CONFIG', 'GMU_DCACHE_CONFIG', 'GMU_AHB_FENCE_RANGE_0',
            'GMU_GMU2HOST_INTR_INFO', 'GMU_AO_HOST_INTERRUPT_STATUS', 'GMU_MRC_GBIF_QOS_CTRL', 'GPU_GMU_CX_GMU_CX_FALNEXT_INTF',
            'GPU_GMU_CX_GMU_CX_FAL_INTF', 'GMU_GENERAL_8', 'GMU_GENERAL_9', 'GMU_GENERAL_10', 'GBIF_QSB_SIDE0', 'GBIF_CX_CONFIG']
    for name in core: register('A6XX', name)
    register('A6XX', 'RSCC_SEQ_BUSY_DRV0')
    register('A6XX_CX_MISC', 'CX_MISC_TCM_RET_CNTL')
    def field(name, bit):
        for t in trees:
            for d in t.findall('r:domain', ns):
                for reg in d.findall('r:reg32', ns):
                    if d.attrib['name'] == 'A6XX' and reg.attrib.get('name') == name:
                        for f in reg.findall('r:bitfield', ns):
                            if f.attrib.get('name') == bit: return 1 << int(f.attrib['pos'])
        raise SystemExit('Missing fatal bit ' + bit)
    fatal = sum(field('GMU_AO_HOST_INTERRUPT_STATUS', b) for b in ['WDOG_BITE', 'FENCE_ERR', 'HOST_AHB_BUS_ERROR'])
    cm3 = field('GMU_GMU2HOST_INTR_INFO', 'CM3_FAULT')

    # The a8xx branches of a6xx_gmu_fw_start that this profile reproduces.
    gmu = (cache / linux[0]).read_text()
    start = gmu[gmu.index('static int a6xx_gmu_fw_start('):gmu.index('static int a6xx_gmu_fw_start(') + 6000]
    for needle in ['if (adreno_is_a8xx(adreno_gpu)) {\n\t\tfence_range_upper = 0x32;\n\t\tfence_range_lower = 0x8c0;',
                   'gpu_write(gpu, REG_A8XX_GBIF_CX_CONFIG, 0x20023000);', 'gmu_write(gmu, REG_A6XX_GMU_MRC_GBIF_QOS_CTRL, 0x33);',
                   'else if (!adreno_is_a8xx(adreno_gpu))\n\t\tgmu_write(gmu, REG_A6XX_GMU_GENERAL_7, 1);',
                   'gmu_write(gmu, REG_A8XX_GPU_GMU_CX_GMU_CX_FALNEXT_INTF, 1);']:
        if needle not in start: raise SystemExit('Pinned a8xx GMU start sequence changed: ' + needle.split('\n')[0])
    if 'if (adreno_is_a8xx(adreno_gpu))\n\t\tgmu_write(gmu, REG_A8XX_GMU_GENERAL_9, 0);' not in gmu: raise SystemExit('GENERAL_9 changed')

    # msm-next A830 catalog entry.
    catalog = (extra / 'msm-next/drivers/gpu/drm/msm/adreno/a6xx_catalog.c').read_text()
    record = re.search(r'\.chip_ids = ADRENO_CHIP_IDS\(0x44050001\)([\s\S]*?)\}, \{', catalog)[1]
    fw_chip = int(re.search(r'\.gmu_chipid = (0x[0-9a-f]+)', record)[1], 0)
    if '[ADRENO_FW_GMU] = "gen80000_gmu.bin"' not in record or '.gbif_cx = a840_gbif' not in record: raise SystemExit('A830 entry differs')
    gbif_body = catalog[catalog.index('static const struct adreno_reglist a840_gbif[] = {'):]
    gbif_body = gbif_body[:gbif_body.index('};')]
    gbif = []
    for reg, value in re.findall(r'\{ REG_A6XX_(GBIF_\w+), (0x[0-9a-f]+) \}', gbif_body):
        register('A6XX', reg)
        gbif.append((regs['REG_' + reg], int(value, 0)))
    if len(gbif) != 5: raise SystemExit('a840_gbif length differs')
    bcms = re.findall(r'\{\s*\.name = "([A-Z0-9]+)",([^{}]+)\}', record[record.index('.bcms'):])
    if [b[0] for b in bcms] != ['SH0', 'MC0', 'ACV']: raise SystemExit('BCM order differs')
    policies = []
    for name, body in bcms:
        if name != 'ACV':
            m = re.fullmatch(r'\s*\.buswidth = (\d+)\s*', body)
            if not m: raise SystemExit('Unrecognized BCM policy')
            policies.append(dict(buswidth=int(m[1]), fixed=0, perfmode=0, perfmode_kbps=0))
        else:
            m = re.fullmatch(r'\s*\.fixed = true,\s*\.perfmode = BIT\((\d+)\),\s*\.perfmode_bw = (\d+),\s*', body)
            if not m: raise SystemExit('Unrecognized fixed BCM policy')
            policies.append(dict(buswidth=0, fixed=1, perfmode=1 << int(m[1]), perfmode_kbps=int(m[2])))
    speedbins = [(int(a), int(b)) for a, b in re.findall(r'\{\s*(\d+),\s*(\d+)\s*\}', record[record.index('.speedbins'):])]

    # Upstream DT: register ranges, OPP tables, cmd-db reservation.
    dts = (extra / 'arch/arm64/boot/dts/qcom/sm8750.dtsi').read_text()
    levels = {k: int(v) for k, v in re.findall(r'#define (RPMH_REGULATOR_LEVEL_\w+)\s+(\d+)', (extra / 'include/dt-bindings/power/qcom,rpmhpd.h').read_text())}
    def node(label):
        at = dts.index('\t\t' + label + ' {'); depth, i = 0, dts.index('{', at)
        while True:
            depth += {'{': 1, '}': -1}.get(dts[i], 0); i += 1
            if not depth: return dts[at:i]
    def reg(text, index=0):
        cells = [int(v, 16) for v in re.findall(r'0x([0-9a-f]+)', re.search(r'\breg = ([^;]+);', text)[1])]
        return cells[4 * index + 1], cells[4 * index + 3]
    gpu_node, gmu_node = node('gpu: gpu@3d00000'), node('gmu: gmu@3d6c000')
    if '"kgsl_3d0_reg_memory",\n\t\t\t\t    "cx_mem"' not in gpu_node or 'reg-names = "gmu";' not in gmu_node: raise SystemExit('DT reg-names differ')
    gpu_win, cx_win, gmu_win = reg(gpu_node, 0), reg(gpu_node, 1), reg(gmu_node, 0)
    if 'RSCC lives at GPU base + 0x50000' not in gmu: raise SystemExit('A8xx RSCC placement changed')
    rscc_win = (gpu_win[0] + 0x50000, 0x10000)
    windows = [gmu_win, rscc_win, (0, 0), gpu_win, cx_win]      # ShengGmu order: core, rscc, pdc (none on A8xx), gpu, cx_misc
    def opps(text):
        out = []
        for name, body in re.findall(r'(opp-[0-9]+(?:-\d)?) \{([^{}]+)\}', text[text.index('opp-table {'):]):
            hz = int(re.search(r'opp-hz = /bits/ 64 <(\d+)>', body)[1])
            level = levels[re.search(r'opp-level = <(RPMH_REGULATOR_LEVEL_\w+)>', body)[1]]
            kbps = re.search(r'opp-peak-kBps = <(\d+)>', body); acd = re.search(r'qcom,opp-acd-level = <(0x[0-9a-f]+)>', body)
            hw = re.search(r'opp-supported-hw = <(0x[0-9a-f]+)>', body)
            out.append(dict(hz=hz, level=level, kbps=int(kbps[1]) if kbps else 0, acd_present=bool(acd),
                            acd=int(acd[1], 0) if acd else 0, supported_hw=int(hw[1], 0) if hw else 0xffffffff))
        return out
    gpu_opps, gmu_opps = opps(gpu_node), opps(gmu_node)
    for b in range(5):
        chosen = sorted((o for o in gpu_opps if o['supported_hw'] >> b & 1), key=lambda o: o['hz'])
        if not chosen or len(chosen) + 1 > 16 or len({o['hz'] for o in chosen}) != len(chosen): raise SystemExit('Bin %d OPPs invalid' % b)
        if any(chosen[i]['level'] < chosen[i - 1]['level'] for i in range(1, len(chosen))): raise SystemExit('Nonmonotonic OPP levels')
    gmu_opps = [dict(hz=0, level=0, kbps=0, acd_present=False, acd=0, supported_hw=0xffffffff)] + sorted(gmu_opps, key=lambda o: o['hz'])
    db = dts[dts.index('aop_cmd_db_mem: aop-cmd-db@81c60000 {'):]
    db = db[:db.index('};')]
    if 'compatible = "qcom,cmd-db";' not in db or 'no-map;' not in db: raise SystemExit('Command DB binding differs')
    db_base, db_bytes = reg(db)

    # Firmware block plan (AdrenoFirmwarePlanGmu's walk; ShengGmu.c re-plans it on the device).
    data = firmware.read_bytes(); offset = blocks = 0
    while offset < len(data):
        offset += 16 + struct.unpack_from('<I', data, offset + 4)[0]; blocks += 1
    if offset != len(data): raise SystemExit('GMU firmware blocks do not tile the image')

    out = ROOT / 'build/erhai-gmu'; out.mkdir(parents=True, exist_ok=True)
    text = '/* Generated by scripts/prepare-erhai-gmu.py from pinned inputs (gen8 profile). Do not commit. */\n'
    text += '#include <stdint.h>\n#include <stddef.h>\n#define SHENG_GMU_GEN8 1\n'
    text += '\n'.join('#define %s UINT32_C(%s)' % (k, hex(v)) for k, v in regs.items()) + '\n'
    text += '#define SHENG_GMU_CHIP_ID UINT32_C(%s)\n#define SHENG_GMU_FW_CHIP_ID UINT32_C(%s)\n' % (hex(CHIP_ID), hex(fw_chip))
    text += '#define SHENG_GMU_CORE_GPU_DELTA %su\n' % hex(gmu_win[0] - gpu_win[0])
    text += '#define SHENG_GMU_FIRMWARE_BYTES %du\n#define SHENG_GMU_FIRMWARE_BLOCKS %du\n' % (len(data), blocks)
    text += '#define SHENG_GMU_AO_FATAL_MASK %su\n#define SHENG_GMU_CM3_FATAL_MASK %su\n' % (hex(fatal), hex(cm3))
    text += '#define SHENG_GMU_FENCE_LOWER 0x8c0u\n#define SHENG_GMU_UNUSED_WINDOWS (1u << 2)\n'
    text += 'static const uint8_t sheng_gmu_firmware_sha256[32]={%s};\n' % ','.join('0x' + policy['sha256'][i:i + 2] for i in range(0, 64, 2))
    text += 'static const uint64_t sheng_gmu_window_physical[5]={%s};\n' % ','.join(hex(w[0]) for w in windows)
    text += 'static const size_t sheng_gmu_window_bytes[5]={%s};\n' % ','.join(hex(w[1]) for w in windows)
    text += 'typedef struct {unsigned window;size_t byte_offset;uint32_t value;} ShengGmuSetupWrite;\n'
    text += '/* A8xx: AOP owns the RSC/PDC sequence (a6xx_gmu_rpmh_init returns early). */\n'
    text += '#define SHENG_GMU_RSC_SETUP_COUNT 0u\nstatic const ShengGmuSetupWrite sheng_gmu_rsc_setup[1]={{0,0,0}};\n'
    text += '/* a840_gbif, then the a8xx writes of a6xx_gmu_fw_start: GPU window dword offset and value. */\n'
    text += '#define SHENG_GMU_GBIF_COUNT %du\nstatic const uint32_t sheng_gmu_gbif[%d][2]={%s};\n' % (
        len(gbif) + 1, len(gbif) + 1, ','.join('{%s,%s}' % (hex(r), hex(v)) for r, v in gbif + [(regs['REG_GBIF_CX_CONFIG'], 0x20023000)]))
    text += '#define SHENG_GMU_MRC_GBIF_QOS 0x33u\n'
    header = out / 'ShengGmuSource.h'; header.write_text(text)

    text = '/* Generated by scripts/prepare-erhai-gmu.py from the pinned sm8750.dtsi and A830 catalog; no command-db values. */\n'
    text += '#include <stdint.h>\n#define SHENG_GMU_VOTES_CHIP_ID 0x%xu\n#define SHENG_GMU_VOTES_GEN8 1\n' % CHIP_ID
    text += 'typedef struct {uint64_t hz;uint32_t level,kbps,acd_present,acd,supported_hw;} ShengGmuOpp;\n'
    text += '/* qfprom speed-bin fuse value -> opp-supported-hw bit (catalog speedbins). */\n'
    text += '#define SHENG_GMU_SPEEDBINS %du\nstatic const uint32_t sheng_gmu_speedbins[][2]={%s};\n' % (
        len(speedbins), ','.join('{%d,%d}' % s for s in speedbins))
    for kind, table in (('gpu', sorted(gpu_opps, key=lambda o: o['hz'])), ('gmu', gmu_opps)):
        text += '#define SHENG_%s_OPP_ENTRIES %du\nstatic const ShengGmuOpp sheng_%s_opp_entries[]={\n' % (kind.upper(), len(table), kind)
        text += '\n'.join('{%d,%d,%d,%d,%du,%su},' % (o['hz'], o['level'], o['kbps'], o['acd_present'], o['acd'], hex(o['supported_hw'])) for o in table) + '\n};\n'
    text += 'static const AdrenoHfiBcm sheng_bcm_policy[3]={\n' + '\n'.join(
        '{.buswidth=%d,.fixed=%d,.perfmode=%d,.perfmode_kbps=%d},' % (p['buswidth'], p['fixed'], p['perfmode'], p['perfmode_kbps']) for p in policies) + '\n};\n'
    votes = out / 'ShengGmuVotesSource.h'; votes.write_text(text)
    db_header = out / 'ShengGmuCommandDbSource.h'
    db_header.write_text('/* Pinned DT reservation, not observed hardware memory. */\n#define SHENG_GMU_COMMAND_DB_BASE UINT64_C(%s)\n#define SHENG_GMU_COMMAND_DB_BYTES %du\n' % (hex(db_base), db_bytes))
    report = dict(linux_revision=pins['linux']['commit'], erhai_inputs={n: e['sha256'] for n, e in erhai['files'].items()},
                  registers=regs, windows=windows, gbif=gbif, gmu_chipid=fw_chip, speedbins=speedbins, gpu_opps=gpu_opps,
                  gmu_opps=gmu_opps, bcms=policies, command_db=[db_base, db_bytes], firmware=policy, firmware_blocks=blocks,
                  headers={p.name: sha(p) for p in (header, votes, db_header)}, hardware_registers_observed=False)
    (out / 'source-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: gen8 GMU registers, A830 catalog/DT policy and the pinned OEM GMU image; source evidence only')
    return out, report


if __name__ == '__main__': prepare()
