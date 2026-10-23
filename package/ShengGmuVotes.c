/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengGmuVotes.h"
#include "ShengGmuVotesSource.h"
#ifndef SHENG_GMU_VOTES_GEN8
#define SHENG_GMU_VOTES_GEN8 0
#else
/* gen8 profile (scripts/prepare-erhai-gmu.py): the DT OPPs carry opp-supported-hw, so the enabled ones depend on the
 * speed-bin fuse (the catalog speedbins map it to a bit, as adreno_gpu.c does). ShengGmuBuildStartup selects them, with
 * the off entry first and the deduplicated bandwidths, into these tables, which ShengGmuPerfVote then reads. */
static ShengGmuOpp sheng_gpu_opps[16];
static uint32_t sheng_gpu_bandwidths[16];
static unsigned sheng_gpu_opp_count,sheng_gpu_bandwidth_count;
#define SHENG_GPU_OPPS sheng_gpu_opp_count
#define SHENG_GPU_BANDWIDTHS sheng_gpu_bandwidth_count
#define SHENG_GMU_OPPS SHENG_GMU_OPP_ENTRIES
#define sheng_gmu_opps sheng_gmu_opp_entries
static int select_opps(uint32_t fuse)
{
    unsigned bin=32,count=1,bandwidths=1;
    for(unsigned i=0;i<SHENG_GMU_SPEEDBINS;++i)if(sheng_gmu_speedbins[i][0]==fuse)bin=sheng_gmu_speedbins[i][1];
    if(bin>=32)return 1;
    ShengGmuOpp opps[16]={{0}};uint32_t kbps[16]={0};
    for(unsigned i=0;i<SHENG_GPU_OPP_ENTRIES;++i) {
        const ShengGmuOpp *o=sheng_gpu_opp_entries+i;
        if(!((o->supported_hw>>bin)&1))continue;
        if(count==16)return 1;
        opps[count++]=*o;
        unsigned seen=0;
        for(unsigned j=0;j<bandwidths;++j)seen|=kbps[j]==o->kbps;
        if(!seen)kbps[bandwidths++]=o->kbps;
    }
    if(count<2)return 1;
    for(unsigned i=1;i<bandwidths;++i)for(unsigned j=i;j>1 && kbps[j]<kbps[j-1];--j) {
        uint32_t t=kbps[j];kbps[j]=kbps[j-1];kbps[j-1]=t;
    }
    for(unsigned i=0;i<16;++i){sheng_gpu_opps[i]=opps[i];sheng_gpu_bandwidths[i]=kbps[i];}
    sheng_gpu_opp_count=count;sheng_gpu_bandwidth_count=bandwidths;return 0;
}
#endif
AdrenoHfiStartupStatus ShengGmuBuildStartup(const ShengGmuVoteInputs *in,AdrenoHfiStartupConfig *out)
{
    if(!in || !out || in->chip_id!=SHENG_GMU_VOTES_CHIP_ID || in->gmxc_present>1 ||
        in->enable_acd>1 || in->enable_ifpc>1 ||
        (!in->gmxc_present && (in->gmxc.data || in->gmxc.bytes)))return AdrenoHfiStartupInvalid;
    AdrenoHfiStartupConfig c={0};uint32_t votes[16],levels[16];
#if SHENG_GMU_VOTES_GEN8
    if(select_opps(in->speed_bin_fuse))return AdrenoHfiStartupInvalid;
    c.table_format=AdrenoHfiTableA8xx;
#endif
    const ShengGmuAux *gpu_secondary=in->gmxc_present?&in->gmxc:&in->mx;
    for(unsigned i=0;i<SHENG_GPU_OPPS;++i)levels[i]=sheng_gpu_opps[i].level;
    if(AdrenoHfiArcVotes(levels,SHENG_GPU_OPPS,in->gfx.data,in->gfx.bytes,
        gpu_secondary->data,gpu_secondary->bytes,votes,16))return AdrenoHfiStartupInvalid;
    c.gpu_count=SHENG_GPU_OPPS;
#if SHENG_GMU_VOTES_GEN8
    /* a6xx_gmu_rpmh_dep_votes_init: the GPU levels' dependency votes on mx.lvl. */
    if(AdrenoHfiDependencyVotes(levels,SHENG_GPU_OPPS,in->mx.data,in->mx.bytes,c.gpu_dependency,16))return AdrenoHfiStartupInvalid;
#endif
    for(unsigned i=0;i<c.gpu_count;++i) {
        c.gpu[i]=(AdrenoHfiPerformanceLevel){sheng_gpu_opps[i].hz,votes[i]};
        if(in->enable_acd && sheng_gpu_opps[i].acd_present) {
            c.acd_enable_mask|=1u<<i;c.acd_data[c.acd_count++]=sheng_gpu_opps[i].acd;
        }
    }
    if(c.acd_count)c.acd_version=c.acd_stride=1;
    for(unsigned i=0;i<SHENG_GMU_OPPS;++i)levels[i]=sheng_gmu_opps[i].level;
    if(AdrenoHfiArcVotes(levels,SHENG_GMU_OPPS,in->cx.data,in->cx.bytes,
        in->mx.data,in->mx.bytes,votes,16))return AdrenoHfiStartupInvalid;
    c.gmu_count=SHENG_GMU_OPPS;
    for(unsigned i=0;i<c.gmu_count;++i)c.gmu[i]=(AdrenoHfiPerformanceLevel){sheng_gmu_opps[i].hz,votes[i]};
    const ShengGmuBcmInput *inputs[]={&in->sh0,&in->mc0,&in->acv};AdrenoHfiBcm bcms[3];
    for(unsigned i=0;i<3;++i) {
        bcms[i]=sheng_bcm_policy[i];bcms[i].address=inputs[i]->address;
        if(AdrenoHfiBcmAux(bcms+i,inputs[i]->aux.data,inputs[i]->aux.bytes))return AdrenoHfiStartupInvalid;
    }
    if(AdrenoHfiBuildBandwidth(sheng_gpu_bandwidths,SHENG_GPU_BANDWIDTHS,bcms,3,in->cn0_address,&c))
        return AdrenoHfiStartupInvalid;
    c.enable_ifpc=in->enable_ifpc;
    if(AdrenoHfiStartupValidate(&c))return AdrenoHfiStartupInvalid;
    *out=c;return AdrenoHfiStartupSuccess;
}
unsigned ShengGmuPerfVote(unsigned perf,unsigned *bandwidth)
{
    if(perf<1)perf=1;
    if(perf>=SHENG_GPU_OPPS)perf=SHENG_GPU_OPPS-1;
    unsigned bw=1;
    for(unsigned i=1;i<SHENG_GPU_BANDWIDTHS;++i)if(sheng_gpu_bandwidths[i]==sheng_gpu_opps[perf].kbps){bw=i;break;}
    if(bandwidth)*bandwidth=bw;
    return perf;
}
