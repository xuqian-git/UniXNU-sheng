#!/usr/bin/env python3
"""Generate the inputs of the erhai (Adreno 830) GPU hw_init board code from pinned sources; no device I/O.

The msm-next sources of scripts/data/erhai-gpu-inputs.json give:
- the register database (registers/adreno/*.xml), turned into C headers by its own gen_header.py in build/erhai-gpu/gen;
- the A830 catalog lists (a830_protect, a830_nonctxt_regs, a830_pwrup/ifpc/dyn_pwrup reglists);
- the masks a8xx_gpu.c programs (A8XX_INT_MASK, the APRIV and CP interrupt/fault masks);
- the sm8750 UBWC configuration (ubwc_config.c, ubwc.h);
- the uche_trap_base and ring defaults of a6xx_gpu.c / msm_ringbuffer.h.

A host program built from those headers evaluates every value, so build/erhai-gpu/ErhaiGpuSource.h holds plain numbers only.
That header is generated, never committed, and is no board evidence: the tablet run is."""
import hashlib, json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/erhai-gpu'
MN = ROOT / '.cache/erhai-gpu-source/msm-next'
REGS = MN / 'drivers/gpu/drm/msm/registers'
ADRENO = MN / 'drivers/gpu/drm/msm/adreno'
XMLS = ['adreno_common', 'adreno_pm4', 'a6xx_enums', 'a7xx_enums', 'a8xx_enums', 'a6xx_perfcntrs', 'a7xx_perfcntrs', 'a8xx_perfcntrs',
        'a6xx', 'a6xx_descriptors', 'a8xx_descriptors', 'a6xx_gmu']

# Values the hw_init board code uses: (generated name, C expression over the pinned register headers). Array registers
# are emitted as their base and stride.
VALUES = [
    ('CP_APERTURE_CNTL_HOST', 'REG_A8XX_CP_APERTURE_CNTL_HOST'),
    ('CP_APERTURE_PIPEID_SHIFT', 'A8XX_CP_APERTURE_CNTL_HOST_PIPEID__SHIFT'),
    ('CP_APERTURE_SLICEID_SHIFT', 'A8XX_CP_APERTURE_CNTL_HOST_SLICEID__SHIFT'),
    ('GBIF_HALT', 'REG_A6XX_GBIF_HALT'),
    ('RBBM_GBIF_HALT', 'REG_A8XX_RBBM_GBIF_HALT'),
    ('RBBM_SECVID_TSB_CNTL', 'REG_A6XX_RBBM_SECVID_TSB_CNTL'),
    ('RBBM_SECVID_TSB_TRUSTED_BASE', 'REG_A6XX_RBBM_SECVID_TSB_TRUSTED_BASE'),
    ('RBBM_SECVID_TSB_TRUSTED_SIZE', 'REG_A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE'),
    ('RBBM_SECVID_TRUST_CNTL', 'REG_A6XX_RBBM_SECVID_TRUST_CNTL'),
    ('RBBM_PERFCTR_GPU_BUSY_MASKED', 'REG_A8XX_RBBM_PERFCTR_GPU_BUSY_MASKED'),
    ('UCHE_CCHE_GC_GMEM_RANGE_MIN', 'REG_A8XX_UCHE_CCHE_GC_GMEM_RANGE_MIN'),
    ('SP_HLSQ_GC_GMEM_RANGE_MIN', 'REG_A8XX_SP_HLSQ_GC_GMEM_RANGE_MIN'),
    ('UCHE_TRAP_BASE', 'REG_A8XX_UCHE_TRAP_BASE'),
    ('UCHE_WRITE_THRU_BASE', 'REG_A8XX_UCHE_WRITE_THRU_BASE'),
    ('UCHE_CCHE_TRAP_BASE', 'REG_A8XX_UCHE_CCHE_TRAP_BASE'),
    ('UCHE_CCHE_WRITE_THRU_BASE', 'REG_A8XX_UCHE_CCHE_WRITE_THRU_BASE'),
    ('RBBM_PERFCTR_CNTL', 'REG_A8XX_RBBM_PERFCTR_CNTL'),
    ('RBBM_SLICE_PERFCTR_CNTL', 'REG_A8XX_RBBM_SLICE_PERFCTR_CNTL'),
    ('GMU_POWER_COUNTER_SELECT_XOCLK_0', 'REG_A8XX_GMU_CX_GMU_POWER_COUNTER_SELECT_XOCLK_0'),
    ('GMU_POWER_COUNTER_SELECT_XOCLK_1', 'REG_A8XX_GMU_CX_GMU_POWER_COUNTER_SELECT_XOCLK_1'),
    ('GMU_POWER_COUNTER_ENABLE', 'REG_A8XX_GMU_CX_GMU_POWER_COUNTER_ENABLE'),
    ('GMU_AO_GPU_CX_BUSY_MASK', 'REG_A6XX_GPU_GMU_AO_GPU_CX_BUSY_MASK'),
    ('GMU_AO_CGC_MODE_CNTL', 'REG_A6XX_GPU_GMU_AO_GMU_CGC_MODE_CNTL'),
    ('GMU_AO_CGC_DELAY_CNTL', 'REG_A6XX_GPU_GMU_AO_GMU_CGC_DELAY_CNTL'),
    ('GMU_AO_CGC_HYST_CNTL', 'REG_A6XX_GPU_GMU_AO_GMU_CGC_HYST_CNTL'),
    ('GMU_HOST2GMU_INTR_SET', 'REG_A6XX_GMU_HOST2GMU_INTR_SET'),
    ('CP_PERFCTR_CP_SEL_0', 'REG_A8XX_CP_PERFCTR_CP_SEL(0)'),
    ('GRAS_NC_MODE_CNTL', 'REG_A8XX_GRAS_NC_MODE_CNTL'),
    ('RB_CCU_NC_MODE_CNTL', 'REG_A8XX_RB_CCU_NC_MODE_CNTL'),
    ('RB_CMP_NC_MODE_CNTL', 'REG_A8XX_RB_CMP_NC_MODE_CNTL'),
    ('SP_NC_MODE_CNTL', 'REG_A6XX_SP_NC_MODE_CNTL'),
    ('TPL1_NC_MODE_CNTL', 'REG_A6XX_TPL1_NC_MODE_CNTL'),
    ('TPL1_BICUBIC_WEIGHTS_TABLE_0', 'REG_A8XX_TPL1_BICUBIC_WEIGHTS_TABLE(0)'),
    ('TPL1_BICUBIC_WEIGHTS_TABLE_STRIDE', 'REG_A8XX_TPL1_BICUBIC_WEIGHTS_TABLE(1)-REG_A8XX_TPL1_BICUBIC_WEIGHTS_TABLE(0)'),
    ('UCHE_CLIENT_PF', 'REG_A8XX_UCHE_CLIENT_PF'),
    ('RBBM_INTERFACE_HANG_INT_CNTL', 'REG_A8XX_RBBM_INTERFACE_HANG_INT_CNTL'),
    ('RBBM_SLICE_INTERFACE_HANG_INT_CNTL', 'REG_A8XX_RBBM_SLICE_INTERFACE_HANG_INT_CNTL'),
    ('CP_PROTECT_CNTL_PIPE', 'REG_A8XX_CP_PROTECT_CNTL_PIPE'),
    ('CP_PROTECT_CNTL', 'A8XX_CP_PROTECT_CNTL_PIPE_ACCESS_PROT_EN | A8XX_CP_PROTECT_CNTL_PIPE_ACCESS_FAULT_ON_VIOL_EN | '
                        'A8XX_CP_PROTECT_CNTL_PIPE_LAST_SPAN_INF_RANGE | A8XX_CP_PROTECT_CNTL_PIPE_HALT_SQE_RANGE__MASK'),
    ('CP_PROTECT_GLOBAL_0', 'REG_A8XX_CP_PROTECT_GLOBAL(0)'),
    ('CP_PROTECT_GLOBAL_STRIDE', 'REG_A8XX_CP_PROTECT_GLOBAL(1)-REG_A8XX_CP_PROTECT_GLOBAL(0)'),
    ('CP_PROTECT_PIPE_15', 'REG_A8XX_CP_PROTECT_PIPE(15)'),
    ('RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE', 'REG_A6XX_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE'),
    ('CP_APRIV_CNTL_PIPE', 'REG_A8XX_CP_APRIV_CNTL_PIPE'),
    ('CP_INTERRUPT_STATUS_MASK_PIPE', 'REG_A8XX_CP_INTERRUPT_STATUS_MASK_PIPE'),
    ('CP_HW_FAULT_STATUS_MASK_PIPE', 'REG_A8XX_CP_HW_FAULT_STATUS_MASK_PIPE'),
    ('CP_INTERRUPT_STATUS_MASK_GLOBAL', 'REG_A8XX_CP_INTERRUPT_STATUS_MASK_GLOBAL'),
    ('RBBM_INT_0_MASK', 'REG_A8XX_RBBM_INT_0_MASK'),
    ('CP_SQE_INSTR_BASE', 'REG_A8XX_CP_SQE_INSTR_BASE'),
    ('CP_AQE_INSTR_BASE_0', 'REG_A8XX_CP_AQE_INSTR_BASE_0'),
    ('CP_RB_BASE', 'REG_A6XX_CP_RB_BASE'),
    ('CP_RB_CNTL', 'REG_A6XX_CP_RB_CNTL'),
    ('CP_RB_RPTR_ADDR', 'REG_A6XX_CP_RB_RPTR_ADDR'),
    ('CP_RB_RPTR_ADDR_BV', 'REG_A8XX_CP_RB_RPTR_ADDR_BV'),
    ('CP_RB_WPTR', 'REG_A6XX_CP_RB_WPTR'),
    ('CP_SQE_CNTL', 'REG_A8XX_CP_SQE_CNTL'),
    ('RB_GC_GMEM_PROTECT', 'REG_A8XX_RB_GC_GMEM_PROTECT'),
    ('RBBM_CGC_0_PC', 'REG_A8XX_RBBM_CGC_0_PC'),
    ('RBBM_CLOCK_CNTL_GLOBAL', 'REG_A8XX_RBBM_CLOCK_CNTL_GLOBAL'),
    ('RBBM_CGC_GLOBAL_LOAD_CMD', 'REG_A8XX_RBBM_CGC_GLOBAL_LOAD_CMD'),
    ('RBBM_CGC_P2S_TRIG_CMD', 'REG_A8XX_RBBM_CGC_P2S_TRIG_CMD'),
    ('RBBM_CGC_P2S_STATUS', 'REG_A8XX_RBBM_CGC_P2S_STATUS'),
    ('RBBM_CGC_P2S_STATUS_TXDONE', 'A8XX_RBBM_CGC_P2S_STATUS_TXDONE'),
    ('RBBM_STATUS', 'REG_A8XX_RBBM_STATUS'),
    ('RBBM_INT_0_STATUS', 'REG_A8XX_RBBM_INT_0_STATUS'),
    ('CP_INTERRUPT_STATUS_GLOBAL', 'REG_A8XX_CP_INTERRUPT_STATUS_GLOBAL'),
    ('CP_HW_FAULT_STATUS_PIPE', 'REG_A8XX_CP_HW_FAULT_STATUS_PIPE'),
    ('CP_RB_RPTR', 'REG_A6XX_CP_RB_RPTR'),
    ('CP_SET_SECURE_MODE', 'CP_SET_SECURE_MODE'),
    ('INT_MASK', 'A8XX_INT_MASK'),
    ('APRIV_MASK', 'A8XX_APRIV_MASK'),
    ('BR_APRIV_MASK', 'A8XX_BR_APRIV_MASK'),
    ('CP_GLOBAL_INT_MASK', 'A8XX_CP_GLOBAL_INT_MASK'),
    ('CP_INTERRUPT_STATUS_MASK_PIPE_VALUE', 'A8XX_CP_INTERRUPT_STATUS_MASK_PIPE'),
    ('CP_HW_FAULT_STATUS_MASK_PIPE_VALUE', 'A8XX_CP_HW_FAULT_STATUS_MASK_PIPE'),
    ('PIPE_NONE', 'PIPE_NONE'), ('PIPE_BR', 'PIPE_BR'), ('PIPE_BV', 'PIPE_BV'), ('PIPE_LPAC', 'PIPE_LPAC'),
    ('PIPE_DDE_BV', 'PIPE_DDE_BV'),
    ('UBWC_ENC_VERSION', 'erhai_ubwc.ubwc_enc_version'), ('UBWC_HIGHEST_BANK_BIT', 'erhai_ubwc.highest_bank_bit'),
    ('UBWC_SWIZZLE', 'qcom_ubwc_swizzle(&erhai_ubwc)'), ('UBWC_AMSBC', 'qcom_ubwc_enable_amsbc(&erhai_ubwc)'),
    ('UBWC_MACROTILE', 'qcom_ubwc_macrotile_mode(&erhai_ubwc)'),
    ('UBWC_MODE', 'qcom_ubwc_get_ubwc_mode(&erhai_ubwc)'), ('UBWC_VERSION_TAG', 'qcom_ubwc_version_tag(&erhai_ubwc)'),
    ('UBWC_SWIZZLE_LVL2', 'UBWC_SWIZZLE_ENABLE_LVL2'), ('UBWC_SWIZZLE_LVL3', 'UBWC_SWIZZLE_ENABLE_LVL3'),
    ('UBWC_3_0', 'UBWC_3_0'), ('UBWC_4_0', 'UBWC_4_0'), ('UBWC_4_3', 'UBWC_4_3'), ('UBWC_5_0', 'UBWC_5_0'), ('UBWC_6_0', 'UBWC_6_0'),
    ('UCHE_TRAP_BASE_VALUE', 'ERHAI_UCHE_TRAP_BASE'),
    ('GMEM_RANGE_MIN_VALUE', 'SZ_64M'),
    ('RB_CNTL_DEFAULT', 'MSM_GPU_RB_CNTL_DEFAULT'),
    ('CHIP_ID', 'ERHAI_A830_CHIP_ID'), ('GMEM_BYTES', 'ERHAI_A830_GMEM'),
]
LISTS = ['a830_nonctxt_regs', 'a830_protect_regs', 'a830_pwrup_reglist_regs', 'a830_ifpc_reglist_regs', 'a830_dyn_pwrup_reglist_regs']


def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()


def text_between(source, start, end):
    at = source.index(start)
    return source[at:source.index(end, at) + len(end)]


def macro(source, name):
    """A multi-line #define from the pinned source, as written."""
    at = source.index('#define ' + name + ' ')
    end = at
    while True:
        line_end = source.index('\n', end)
        if not source[end:line_end].rstrip().endswith('\\'): return source[at:line_end]
        end = line_end + 1


def braces(source, start):
    """From `start` through the brace that closes the first `{` after it."""
    at = source.index('{', start) + 1; depth = 1
    while depth:
        depth += {'{': 1, '}': -1}.get(source[at], 0); at += 1
    return source[start:at]


def function(source, signature): return braces(source, source.index(signature))


def define(source, name):
    """A #define as written, continuation lines included (the host oracles of check-erhai-gpu-*.py)."""
    at = re.search(r'#define ' + re.escape(name) + r'\b', source).start(); end = at
    while True:
        line_end = source.index('\n', end)
        if not source[end:line_end].rstrip().endswith('\\'): return source[at:line_end]
        end = line_end + 1


def struct(source, name): return braces(source, source.index('struct ' + name + ' {')) + ';'


def generate_headers():
    (OUT / 'gen').mkdir(parents=True, exist_ok=True)
    for name in XMLS:
        header = subprocess.run([sys.executable, '-I', str(REGS / 'gen_header.py'), '--rnn', str(REGS), '--xml',
                                 str(REGS / 'adreno' / (name + '.xml')), 'c-defines'], capture_output=True, text=True, check=True)
        (OUT / 'gen' / (name + '.xml.h')).write_text(header.stdout)


def evaluator():
    """Host C source that prints ErhaiGpuSource.h from the pinned headers, catalog lists and mask definitions."""
    catalog = (ADRENO / 'a6xx_catalog.c').read_text()
    a8xx = (ADRENO / 'a8xx_gpu.c').read_text()
    a6xx_h = (ADRENO / 'a6xx_gpu.h').read_text()
    ubwc_h = (MN / 'include/linux/soc/qcom/ubwc.h').read_text()
    ubwc_c = (MN / 'drivers/soc/qcom/ubwc_config.c').read_text()
    a6xx_c = (ADRENO / 'a6xx_gpu.c').read_text()
    if 'adreno_gpu->uche_trap_base = 0x1fffffffff000ull;' not in a6xx_c: raise SystemExit('uche_trap_base changed')
    if '{ .compatible = "qcom,sm8750", .data = &ubwc_5_0_hbb16, },' not in ubwc_c: raise SystemExit('sm8750 UBWC entry changed')
    lists = '\n'.join(text_between(catalog, 'static const ' + ('struct adreno_reglist_pipe ' if 'nonctxt' in n or 'dyn' in n else 'u32 ')
                                   + n + '[] = {', '};') for n in LISTS)
    masks = '\n'.join(macro(a8xx, m) for m in ['A8XX_INT_MASK', 'A8XX_APRIV_MASK', 'A8XX_BR_APRIV_MASK', 'A8XX_CP_GLOBAL_INT_MASK',
                                                 'A8XX_CP_INTERRUPT_STATUS_MASK_PIPE', 'A8XX_CP_HW_FAULT_STATUS_MASK_PIPE'])
    protect = macro(a6xx_h, 'A6XX_PROTECT_NORDWR(_reg, _len)').replace('(_reg, _len) ', '(_reg, _len) ') + '\n' + \
        macro(a6xx_h, 'A6XX_PROTECT_RDONLY(_reg, _len)')
    ubwc_struct = text_between(ubwc_h, 'struct qcom_ubwc_cfg_data {', '};')
    ubwc_inline = ubwc_h[ubwc_h.index('#define UBWC_1_0'):ubwc_h.index('#endif /* __QCOM_UBWC_H__ */')]
    ubwc_inline = re.sub(r'#if IS_ENABLED\(CONFIG_QCOM_UBWC_CONFIG\).*?#endif\n', '', ubwc_inline, flags=re.S)
    ubwc_data = text_between(ubwc_c, 'static const struct qcom_ubwc_cfg_data ubwc_5_0_hbb16 = {', '};').replace(
        'ubwc_5_0_hbb16', 'erhai_ubwc')
    includes = '\n'.join(f'#include "{n}.xml.h"' for n in XMLS)
    # The A830 catalog entry: its chip id and gmem, as the device info the user client reports.
    entry = catalog[:catalog.index('.chip_ids = ADRENO_CHIP_IDS(0x44050001),')]
    entry = catalog[entry.rindex('{'):catalog.index('}, {', len(entry))]
    gmem = re.search(r'\.gmem = ([^,]+),', entry).group(1)
    bicubic = re.findall(r'REG_A8XX_TPL1_BICUBIC_WEIGHTS_TABLE\((\d+)\), (0x[0-9a-f]+|0)\);', a8xx[a8xx.index('static int hw_init('):])
    if [int(i) for i, _ in bicubic] != list(range(20)): raise SystemExit('hw_init bicubic table changed')
    bicubic = ','.join(v for _, v in bicubic)
    values = '\n'.join(f'    printf("#define ERHAI_GPU_{n} 0x%llxu\\n", (unsigned long long)({e}));' for n, e in VALUES)
    return f'''#define FD_NO_DEPRECATED_PACK 1
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
static inline uint32_t fui(float f) {{union {{float f;uint32_t u;}} v={{f}};return v.u;}}
static inline uint16_t _mesa_float_to_half(float f) {{return (uint16_t)fui(f);}}
static inline uint16_t util_float_to_half(float f) {{return (uint16_t)fui(f);}}
#define BIT(n) (1u<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define SZ_64M 0x04000000u
#define SZ_1M 0x00100000u
#define ERHAI_A830_CHIP_ID 0x44050001u
#define ERHAI_A830_GMEM ({gmem})
#define ERHAI_UCHE_TRAP_BASE 0x1fffffffff000ull
#define MSM_GPU_RINGBUFFER_SZ 0x8000u
#define MSM_GPU_RINGBUFFER_BLKSIZE 32u
#define MSM_GPU_RB_CNTL_DEFAULT (AXXX_CP_RB_CNTL_BUFSZ(__builtin_ctz(MSM_GPU_RINGBUFFER_SZ/8)) | AXXX_CP_RB_CNTL_BLKSZ(__builtin_ctz(MSM_GPU_RINGBUFFER_BLKSIZE/8)))
{includes}
struct adreno_reglist_pipe {{u32 offset;u32 value;u32 pipe;}};
{protect}
{masks}
{lists}
{ubwc_struct}
{ubwc_inline}
{ubwc_data}
static void list32(const char *name,const u32 *v,unsigned n) {{
    printf("#define ERHAI_GPU_%s_COUNT %uu\\nstatic const uint32_t erhai_gpu_%s[]={{",name,n,name);
    for(unsigned i=0;i<n;++i)printf("%s0x%x",i?",":"",v[i]);printf("}};\\n");
}}
static void listpipe(const char *name,const struct adreno_reglist_pipe *v) {{
    unsigned n=0;while(v[n].offset)++n;
    printf("#define ERHAI_GPU_%s_COUNT %uu\\nstatic const uint32_t erhai_gpu_%s[][3]={{",name,n,name);
    for(unsigned i=0;i<n;++i)printf("%s{{0x%x,0x%x,0x%x}}",i?",":"",v[i].offset,v[i].value,v[i].pipe);printf("}};\\n");
}}
int main(void) {{
    printf("/* Generated by scripts/prepare-erhai-gpu.py from the pinned msm-next sources; numbers only. */\\n#include <stdint.h>\\n");
{values}
    printf("static const uint32_t erhai_gpu_BICUBIC[20]={{{bicubic}}};\\n");
    listpipe("NONCTXT",a830_nonctxt_regs);
    list32("PROTECT",a830_protect_regs,ARRAY_SIZE(a830_protect_regs));
    list32("PWRUP",a830_pwrup_reglist_regs,ARRAY_SIZE(a830_pwrup_reglist_regs));
    list32("IFPC",a830_ifpc_reglist_regs,ARRAY_SIZE(a830_ifpc_reglist_regs));
    unsigned dyn=0;while(a830_dyn_pwrup_reglist_regs[dyn].offset)++dyn;
    printf("#define ERHAI_GPU_DYN_PWRUP_COUNT %uu\\nstatic const uint32_t erhai_gpu_DYN_PWRUP[][2]={{",dyn);
    for(unsigned i=0;i<dyn;++i)printf("%s{{0x%x,0x%x}}",i?",":"",a830_dyn_pwrup_reglist_regs[i].offset,a830_dyn_pwrup_reglist_regs[i].pipe);
    printf("}};\\n");
    return 0;
}}
'''


def prepare():
    erhai = json.loads((ROOT / 'scripts/data/erhai-gpu-inputs.json').read_text())
    for name, expected in erhai['files'].items():
        if name.startswith('msm-next/') and sha(ROOT / '.cache/erhai-gpu-source' / name) != expected['sha256']:
            raise SystemExit('Pinned erhai input differs: ' + name)
    generate_headers()
    (OUT / 'evaluate.c').write_text(evaluator())
    subprocess.run(['xcrun', 'clang', '-std=gnu11', '-Wall', '-Wno-unused-function', '-Wno-unused-const-variable', '-I', str(OUT / 'gen'),
                    str(OUT / 'evaluate.c'), '-o', str(OUT / 'evaluate')], check=True, timeout=120)
    header = subprocess.run([str(OUT / 'evaluate')], capture_output=True, text=True, check=True).stdout
    (OUT / 'ErhaiGpuSource.h').write_text(header)
    return OUT


if __name__ == '__main__':
    out = prepare()
    print(f'PASS: erhai A830 hw_init inputs -> {out / "ErhaiGpuSource.h"}')
