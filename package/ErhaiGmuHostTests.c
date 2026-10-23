/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host tests of the gen8 (Adreno 830) profile of ShengGmu.c / ShengGmuVotes.c, built with build/erhai-gmu's headers and
 * the pinned gen80000_gmu.bin. Software register/time peers only; no erhai MMIO or GMU execution. */
#include "ShengGmu.h"
#include "ShengGmuVotes.h"
#include "ShengGmuSource.h"
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#if !SHENG_GMU_GEN8
#error build with the gen8 headers of scripts/prepare-erhai-gmu.py
#endif
typedef struct {unsigned kind,window;size_t at;uint32_t value;} Event;
static struct {
    uint32_t *regs[5];Event events[4096];unsigned events_count,calls,fail_call,hold_firmware,hold_hfi,release_count;
    unsigned protocol_peer,commands,protocol_fail;uint64_t time;
} f;
static ShengGmuDevice device;static ShengGmuConfig config;static uint8_t *firmware;static size_t firmware_bytes;
static size_t offset(uint32_t word){return (size_t)word*4-SHENG_GMU_CORE_GPU_DELTA;}
static uint32_t *core(uint32_t word){return f.regs[0]+offset(word)/4;}
static int event(unsigned kind,unsigned window,size_t at,uint32_t value)
{CHECK(f.events_count<4096);f.events[f.events_count++]=(Event){kind,window,at,value};return ++f.calls==f.fail_call;}
static int read32(void *cookie,unsigned window,size_t at,uint32_t *out)
{
    CHECK(cookie==&f && window<5 && window!=ShengGmuPdc && !(at&3) && at+4<=sheng_gmu_window_bytes[window]);
    if(event(0,window,at,0))return 1;
    *out=f.regs[window][at/4];return 0;
}
/* The A8xx startup: TABLE (perf), BW table, ACD feature + table, IFPC feature, CORE_FW_START, START. */
static void protocol_response(void)
{
    volatile uint32_t *s=config.hfi.cpu;unsigned at=s[16];uint32_t header=s[1024+at];
    static const unsigned ids[]={15,3,11,7,11,14,10};
    CHECK(f.commands<7 && (header&255)==ids[f.commands]);
    if(!f.commands)CHECK(((header>>8)&255)==1+4+3*2+2+2*2 && s[1024+((at+2)&1023)]==1);   /* type GPU_PERF */
    ++f.commands;s[16]=s[17];
    if(f.commands==f.protocol_fail)return;                                             /* no ACK */
    at=s[29];s[2048+at]=(3u<<8)|126u;s[2048+((at+1)&1023)]=header;s[2048+((at+2)&1023)]=0;
    s[2048+((at+3)&1023)]=0xfafafafa;s[29]=(at+4)&1023;
}
static int write32(void *cookie,unsigned window,size_t at,uint32_t value)
{
    CHECK(cookie==&f && window<5 && window!=ShengGmuPdc && !(at&3) && at+4<=sheng_gmu_window_bytes[window]);
    int failed=event(1,window,at,value);if(failed)return 1;
    f.regs[window][at/4]=value;
    if(window==0 && at==offset(REG_GMU_CM3_SYSRESET) && !value) {
        ++f.release_count;if(!f.hold_firmware)*core(REG_GMU_CM3_FW_INIT_RESULT)=0x100;
    }
    if(window==0 && at==offset(REG_GMU_HFI_CTRL_INIT) && value && !f.hold_hfi)*core(REG_GMU_HFI_CTRL_STATUS)=1;
    if(window==0 && at==offset(REG_GMU_HOST2GMU_INTR_SET) && value==1 && f.protocol_peer)protocol_response();
    return 0;
}
static int barrier(void *cookie){CHECK(cookie==&f);return event(2,0,0,0);}
static int hash(void *cookie,const void *input,size_t bytes,uint8_t digest[32])
{CHECK(cookie==&f && bytes<=UINT32_MAX);return CC_SHA256(input,(CC_LONG)bytes,digest)?0:1;}
static int now(void *cookie,uint64_t *value){CHECK(cookie==&f);*value=f.time;return 0;}
static int wait_us(void *cookie,unsigned us){CHECK(cookie==&f);f.time+=us;return 0;}
static void *allocate(size_t bytes){void *p=NULL;CHECK(!posix_memalign(&p,4096,bytes ? bytes : 4096));memset(p,0,bytes);return p;}
static void reset(void)
{
    for(unsigned i=0;i<5;++i)memset(f.regs[i],0,sheng_gmu_window_bytes[i]);
    f.events_count=f.calls=f.fail_call=f.hold_firmware=f.hold_hfi=f.release_count=0;
    f.protocol_peer=f.commands=f.protocol_fail=0;f.time=1000000;
    *core(REG_GMU_CM3_SYSRESET)=1;
    device=(ShengGmuDevice){0};device.io=(ShengGmuIo){&f,{0},read32,write32,barrier};
    for(unsigned i=0;i<5;++i)device.io.bytes[i]=sheng_gmu_window_bytes[i];
    device.services=(ShengGmuServices){&f,hash,now,wait_us};config.firmware=firmware;config.firmware_bytes=firmware_bytes;config.poll_budget=4;
}
static unsigned find_write(unsigned window,size_t at,uint32_t value)
{
    for(unsigned i=0;i<f.events_count;++i)if(f.events[i].kind==1 && f.events[i].window==window &&
        f.events[i].at==at && f.events[i].value==value)return i+1;
    CHECK(0);return 0;
}
static unsigned count_writes(unsigned window){unsigned n=0;for(unsigned i=0;i<f.events_count;++i)n+=f.events[i].kind==1 && f.events[i].window==window;return n;}
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void load(void)
{
    reset();ShengGmuResult r;
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuSuccess);
    CHECK(r.phase==7 && r.complete_hash_verified && r.firmware_ready_observed && r.hfi_ready_observed && f.release_count==1);
    /* a6xx_gmu_fw_start's a8xx path: FAL veto first, no RSC/PDC sequence and no TCM retention write. */
    CHECK(!count_writes(ShengGmuRscc) && !count_writes(ShengGmuCxMisc));
    unsigned fal=find_write(0,offset(REG_GPU_GMU_CX_GMU_CX_FALNEXT_INTF),1);
    CHECK(fal<find_write(0,offset(REG_GPU_GMU_CX_GMU_CX_FAL_INTF),1));
    for(unsigned i=0;i<f.events_count;++i)if(f.events[i].kind==1){CHECK(i+1==fal);break;}
    CHECK(*core(REG_GMU_AHB_FENCE_RANGE_0)==(UINT32_C(0x80000000)|(0x32u<<18)|0x8c0));
    CHECK(*core(REG_GMU_GENERAL_10)==0x8000100 && *core(REG_GMU_GENERAL_8)==(0x61000000u|3) && *core(REG_GMU_CM3_CFG)==0x4052);
    CHECK(*core(REG_GMU_HFI_QTBL_ADDR)==config.hfi.iova && *core(REG_GMU_MRC_GBIF_QOS_CTRL)==0x33);
    /* a840_gbif then GBIF_CX_CONFIG, all before the CM3 release. */
    unsigned last=0;
    for(unsigned i=0;i<4;++i){unsigned at=find_write(ShengGmuGpu,(REG_GBIF_QSB_SIDE0+i)*4,0x71e20);CHECK(at>last);last=at;}
    CHECK(f.regs[ShengGmuGpu][REG_GBIF_CX_CONFIG]==0x20023000 && count_writes(ShengGmuGpu)==6);
    CHECK(last<find_write(0,offset(REG_GMU_CM3_SYSRESET),0) && find_write(0,offset(REG_GMU_MRC_GBIF_QOS_CTRL),0x33)<find_write(0,offset(REG_GMU_CM3_SYSRESET),0));
    /* Every firmware payload word: ITCM/DTCM through the core window, icache in memory. */
    size_t at=0;unsigned blocks=0;
    while(at<firmware_bytes) {
        uint32_t address=le32(firmware+at),length=le32(firmware+at+4);
        for(uint32_t i=0;i<length;i+=4) {
            uint32_t word=le32(firmware+at+16+i),actual;
            if(address<0x4000)actual=*core(REG_GMU_CM3_ITCM_START+(address+i)/4);
            else if(address>=0x10004000)actual=*core(REG_GMU_CM3_DTCM_START+(address-0x10004000+i)/4);
            else actual=config.icache.cpu[(address-0x4000+i)/4];
            CHECK(actual==word);
        }
        at+=16+length;++blocks;
    }
    CHECK(blocks==SHENG_GMU_FIRMWARE_BLOCKS);
    /* Every mutation boundary fails closed: poisoned, reset reasserted. */
    unsigned writes=0,normal=f.calls,points[64];
    for(unsigned i=0;i<f.events_count && writes<64;++i)if(f.events[i].kind==1 && f.events[i].window!=0)points[writes++]=i+1;
    points[writes++]=find_write(0,offset(REG_GPU_GMU_CX_GMU_CX_FALNEXT_INTF),1);
    points[writes++]=find_write(0,offset(REG_GMU_CM3_SYSRESET),0);
    points[writes++]=find_write(0,offset(REG_GMU_HFI_CTRL_INIT),1);
    for(unsigned i=0;i<writes;++i) {
        reset();f.fail_call=points[i];CHECK(f.fail_call<=normal);
        CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuIoError);
        CHECK(device.poisoned && r.reset_verified && *core(REG_GMU_CM3_SYSRESET)==1);
    }
    reset();f.hold_firmware=1;CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuTimeout && r.reset_verified);
    reset();f.hold_hfi=1;CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuTimeout && r.reset_verified);
    reset();f.regs[ShengGmuCxMisc][REG_CX_MISC_TCM_RET_CNTL]=1;                     /* warm-boot state is refused */
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuStateMismatch && !count_writes(0));
    reset();f.regs[ShengGmuRscc][REG_RSCC_SEQ_BUSY_DRV0]=1;
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuBusy && !count_writes(0));
    reset();firmware[firmware_bytes-1]^=1;
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuFirmware && !f.events_count);firmware[firmware_bytes-1]^=1;
}
static void cold_start(void)
{
    AdrenoHfiStartupConfig c={.gpu_count=2,.gmu_count=2,.gpu={{0,0},{100000000,1}},.gmu={{0,0},{200000000,1}},
        .bandwidth_count=1,.cnoc_count=1,.ddr_count=1,.cnoc_addresses={0x1000},.ddr_addresses={0x2000},.acd_version=1,
        .acd_enable_mask=2,.acd_stride=1,.acd_count=1,.acd_data={41},.enable_ifpc=1,.table_format=AdrenoHfiTableA8xx,
        .gpu_dependency={UINT32_MAX,0x0fffc1ff}};
    ShengGmuStartupLimits limits={4,100,1000,10000};ShengGmuStartupResult r;
    reset();f.protocol_peer=1;
    CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuSuccess && f.commands==7 && device.startup.completed);
    reset();f.protocol_peer=1;f.protocol_fail=1;
    CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuTimeout && device.poisoned && *core(REG_GMU_CM3_SYSRESET)==1);
}
static void votes(void)
{
    /* Synthetic rail/BCM data for software only. */
    uint8_t gfx[64]={0},mx[64]={0};
    for(unsigned i=1;i<32;++i){gfx[2*i]=(uint8_t)(i*16);gfx[2*i+1]=(uint8_t)((i*16)>>8);mx[2*i]=(uint8_t)(i*16);mx[2*i+1]=(uint8_t)((i*16)>>8);}
    uint8_t sh0[]={0xe8,3,0,0,16,0,0,0},mc0[]={0xe8,3,0,0,4,0,1,0},acv[]={0,0,0,0,0,0,1,0};
    ShengGmuVoteInputs in={.chip_id=SHENG_GMU_CHIP_ID,.gfx={gfx,sizeof gfx},.cx={gfx,sizeof gfx},.mx={mx,sizeof mx},
        .gmxc={mx,sizeof mx},.gmxc_present=1,.sh0={0x1000,{sh0,8}},.mc0={0x1004,{mc0,8}},.acv={0x1008,{acv,8}},.cn0_address=0x2000};
    AdrenoHfiStartupConfig c;
    static const struct {uint32_t fuse;unsigned count;uint64_t top;} bins[]={{232,15,1100000000},{252,16,1200000000},{190,12,900000000},{0,16,1200000000}};
    for(unsigned b=0;b<4;++b) {
        in.speed_bin_fuse=bins[b].fuse;
        CHECK(ShengGmuBuildStartup(&in,&c)==AdrenoHfiStartupSuccess && c.table_format==AdrenoHfiTableA8xx);
        CHECK(c.gpu_count==bins[b].count && !c.gpu[0].hz && c.gpu[c.gpu_count-1].hz==bins[b].top && c.gmu_count==3);
        CHECK(c.gpu_dependency[0]==UINT32_MAX && !c.acd_enable_mask && !c.enable_ifpc);
        for(unsigned i=1;i<c.gpu_count;++i)CHECK((c.gpu_dependency[i]&0xfffc00ffu)==0x0ffc00ffu && c.gpu[i].hz>c.gpu[i-1].hz);
        unsigned bw=99,p=ShengGmuPerfVote(1,&bw);CHECK(p==1 && bw==1);
        CHECK(ShengGmuPerfVote(99,&bw)==c.gpu_count-1 && bw==c.bandwidth_count-1);
    }
    in.speed_bin_fuse=233;CHECK(ShengGmuBuildStartup(&in,&c)==AdrenoHfiStartupInvalid);   /* unknown bin */
    in.speed_bin_fuse=232;in.mx.bytes=8;CHECK(ShengGmuBuildStartup(&in,&c)==AdrenoHfiStartupInvalid);  /* Mx below the top level */
}
static void adapter(void)
{
    ShengGmuMmio m={0};ShengGmuIo io;uint32_t v;
    for(unsigned i=0;i<5;++i)if(i!=ShengGmuPdc)m.windows[i]=(ShengGmuMmioWindow){(volatile unsigned char *)f.regs[i],sheng_gmu_window_physical[i],sheng_gmu_window_bytes[i]};
    CHECK(ShengGmuBindMmio(&m,&io)==ShengGmuSuccess && io.read32(io.cookie,ShengGmuPdc,0,&v) && io.write32(io.cookie,ShengGmuPdc,0,1));
    CHECK(!io.read32(io.cookie,0,0,&v));
    m.windows[ShengGmuPdc]=(ShengGmuMmioWindow){(volatile unsigned char *)f.regs[1],0x3d50000,0x1000};
    CHECK(ShengGmuBindMmio(&m,&io)==ShengGmuArgument);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);FILE *in=fopen(argv[1],"rb");CHECK(in && !fseek(in,0,SEEK_END));long size=ftell(in);CHECK(size>0 && !fseek(in,0,SEEK_SET));
    firmware_bytes=(size_t)size;firmware=malloc(firmware_bytes);CHECK(firmware && fread(firmware,1,firmware_bytes,in)==firmware_bytes && !fclose(in));
    for(unsigned i=0;i<5;++i)f.regs[i]=allocate(sheng_gmu_window_bytes[i]);
    config=(ShengGmuConfig){.chip_id=SHENG_GMU_CHIP_ID,
        .icache={allocate(0xffc000),0xffc000,0x4000},.dummy={allocate(0x4000),0x4000,0x60000000},
        .debug={allocate(0x7000),0x7000,0x60400000},.log={allocate(0x4000),0x4000,0x61000000},
        .hfi={allocate(0x4000),0x4000,0x61004000}};
    load();cold_start();votes();adapter();
    puts("PASS: gen8 GMU profile: pinned gen80000_gmu.bin load/readback, a8xx register sequence (no RSC/TCM, fence 0x8c0, CX GBIF, MRC QoS),"
         " fail-closed boundaries, A8xx TABLE startup with ACKs, speed-bin OPP selection and dependency votes, empty PDC window");
    return 0;
}
