#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#define CONFIG_MTK_AP_BRIDGE 1
#define WHNAT_SUPPORT 1
#define TRUE 1
#define FALSE 0
#define DMA_TX 1
#define DMA_RX 2
#define DMA_TX_RX 3
#define TOKEN_RX 1
#define DEFAULT_RID 0
#define WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_ADDR 0
#define WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_ADDR 1
#define WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_TX_DMA_EN_MASK 1
#define WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_RX_DMA_EN_MASK 2
#define WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_TX_DMA_EN_MASK 1
#define WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_RX_DMA_EN_MASK 2
#define MTWF_DBG(...) ((void)0)
typedef int INT;
typedef int INT32;
typedef bool BOOLEAN;
typedef uint32_t UINT32;
typedef struct {struct {bool whnat_en;} CommonCfg;void *hdev_ctrl;} RTMP_ADAPTER;
typedef RTMP_ADAPTER *PRTMP_ADAPTER;
typedef char RTMP_STRING;
typedef struct {struct {unsigned feature;} tkn_info;} RTMP_CHIP_CAP;
static UINT32 regs[2];
static RTMP_CHIP_CAP cap;
static int bad_port=-1,hooks;
static RTMP_CHIP_CAP *hc_get_chip_cap(void *p) {(void)p;return &cap;}
static int MT7986_get_rid_value(void) {return 1;}
#define RTMP_IO_READ32(c,r,v) do {(void)(c);*(v)=regs[r];}while(0)
#define RTMP_IO_WRITE32(c,r,v) do {(void)(c);regs[r]=(r)==bad_port?((v)^1):(v);}while(0)
#define WLAN_HOOK_DMA_SET 1
#define WLAN_HOOK_CALL(h,a,p) do {(void)(h);(void)(a);(void)(p);hooks++;}while(0)
#include DRIVER_DMA_SOURCE
#include DRIVER_FQ_SOURCE
int main(void)
{
 RTMP_ADAPTER a={0};regs[0]=regs[1]=3;
 assert(set_fq_enable(&a,"1")==FALSE && set_fq_enable(&a,NULL)==FALSE);
 assert(hif_set_WFDMA(&a,DMA_TX_RX,FALSE)==TRUE && !regs[0] && !regs[1] && hooks==1);
 a.CommonCfg.whnat_en=true;cap.tkn_info.feature=TOKEN_RX;hooks=0;
 assert(hif_set_WFDMA(&a,DMA_TX_RX,TRUE)==TRUE && regs[0]==1 && regs[1]==1 && hooks==1);
 for(int port=0;port<2;port++) {
  bad_port=port;hooks=0;regs[0]=regs[1]=3;
  assert(hif_set_WFDMA(&a,DMA_TX_RX,FALSE)==FALSE && !hooks);
 }
 bad_port=-1;hooks=0;
 assert(hif_set_WFDMA(&a,99,FALSE)==FALSE && !hooks);
 puts("DMA: actual MT7986 dual-port readback, disable, WED RX enable policy and failures passed");
 return 0;
}
