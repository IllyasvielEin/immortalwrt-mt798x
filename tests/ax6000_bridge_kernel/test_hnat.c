/* Actual CPU-profile helpers, with MMIO rather than actual hardware. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __iomem
#define BIT(n) (1U << (n))
#define U32_MAX UINT32_MAX
#define FIELD_PREP(m,v) (((uint32_t)(v)<<__builtin_ctz(m))&(m))
#define lower_32_bits(v) ((uint32_t)(v))
#define upper_32_bits(v) ((uint32_t)((uint64_t)(v)>>32))
#define ASSERT_RTNL() ((void)0)
#define dma_rmb() ((void)0)
#define MTK_MAX_DEVS 3
#define MTK_GDMA_FWD_CFG(i) ((i)==2?0x540:0x500+(i)*0x1000)
#define MTK_FE_GLO_CFG(x) ((x)>=8?0x24:0x0)
#define MTK_FE_LINK_DOWN_P(x) BIT(((x)+8)%16)
#define BITS_GDM_ALL_FRC_P_CPU_PDMA 0
#define NR_PPE0_PORT 4
#define NR_PPE1_PORT 8
#define ENTRY_96B 1
#define SMA_ONLY_FWD_CPU 2
typedef uint32_t u32;
struct foe_entry {unsigned char bytes[96];};
struct mtk_eth {void*mac[3];};
struct mtk_hnat {struct mtk_eth*eth;void*fe_base,*ppe_base[2];struct foe_entry*foe_table_cpu[2];uint64_t foe_table_dev[2];unsigned foe_etry_num,etry_num_cfg,ppe_num;};
static bool cpu_exposed;
static int cpu_error,noted_fault;
static unsigned int writes;
static void *bad_read;
static bool poll_fail;
static inline u32 readl(void*r){return *(u32*)r ^ (r==bad_read?1:0);}
static inline void writel(u32 v,void*r){*(u32*)r=v;writes++;}
#define readl_poll_timeout(r,v,cond,delay,timeout) ({(void)(delay);(void)(timeout);(v)=readl(r);poll_fail||!(cond)?-ETIMEDOUT:0;})
static inline void *memchr_inv(const void*p,int c,size_t n){const unsigned char*s=p;for(size_t i=0;i<n;i++)if(s[i]!=(unsigned char)c)return(void*)(s+i);return NULL;}
static inline void mtk_cpu_profile_note_fault(int e){noted_fault=e;}
#include "hnat_registers.inc"
#include "hnat_helpers.inc"
int main(void)
{unsigned char *mmio=calloc(1,0x4000);struct mtk_eth eth={.mac={(void*)1,(void*)2,NULL}};
 struct mtk_hnat h={.eth=&eth,.fe_base=mmio,.ppe_base={mmio+0x2200,mmio+0x2600},.foe_table_dev={0x12340000,0x12740000},.foe_etry_num=4,.etry_num_cfg=7,.ppe_num=2};
 for(unsigned i=0;i<2;i++)h.foe_table_cpu[i]=calloc(4,sizeof(struct foe_entry));
 assert(!hnat_cpu_restore(&h));assert(cpu_exposed&&!cpu_error&&!noted_fault);
 for(unsigned i=0;i<2;i++){void*b=h.ppe_base[i];assert(*(u32*)(b+PPE_TB_BASE)==h.foe_table_dev[i]);assert(!*(u32*)(b+PPE_FLOW_CFG));assert((*(u32*)(b+PPE_TB_CFG)&SMA)==FIELD_PREP(SMA,2));assert(!(*(u32*)(b+PPE_TB_CFG)&(KA_CFG|SCAN_MODE|TCP_AGE|UDP_AGE|UNBD_AGE|FIN_AGE)));assert(!*(u32*)(b+PPE_CAH_CTRL));assert(!*(u32*)(b+PPE_DFT_CPORT));assert(!*(u32*)(b+PPE_MIB_CFG));assert((*(u32*)(b+PPE_GLO_CFG)&(PPE_EN|MCAST_TB_EN))==PPE_EN);}
 assert(!*(u32*)(mmio+MTK_GDMA_FWD_CFG(0)));assert(!*(u32*)(mmio+MTK_GDMA_FWD_CFG(1)));
 unsigned before=writes;h.foe_table_cpu[0][0].bytes[0]=1;assert(hnat_cpu_restore(&h)==-EUCLEAN);assert(cpu_error==-EUCLEAN&&noted_fault==-EUCLEAN&&writes==before);h.foe_table_cpu[0][0].bytes[0]=0;
 assert(hnat_cpu_restore(&h)==-EUCLEAN&&writes==before);cpu_error=noted_fault=0;
 poll_fail=true;assert(hnat_cpu_restore(&h)==-ETIMEDOUT);assert(cpu_error==-ETIMEDOUT);poll_fail=false;cpu_error=noted_fault=0;
 bad_read=h.ppe_base[0]+PPE_TB_BASE;assert(hnat_cpu_restore(&h)==-EIO);assert(cpu_error==-EIO&&noted_fault==-EIO);bad_read=NULL;
 for(unsigned i=0;i<2;i++)free(h.foe_table_cpu[i]);free(mmio);
 puts("actual HNAT CPU restore: fixed mode, zero table proof, poll/readback failures and sticky refusal passed");return 0;}
