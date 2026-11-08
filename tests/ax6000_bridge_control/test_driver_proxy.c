#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_MTK_AP_BRIDGE 1
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define time_after_eq(a,b) ((long)((a)-(b))>=0)
#define cmpxchg(p,o,n) ({__typeof__(*(p)) v=*(p);if(v==(o))*(p)=(n);v;})
#define MTK_CPU_DMA_PROOF_LOST 1
#define MTK_CPU_DMA_CONTROL_RACE 2
#define proxy_print(...) ((void)0)
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
#ifndef MTK_WARP_SER_HEADER
#define MTK_WARP_SER_HEADER "../../.analysis/kernel-next/include/linux/mtk_warp_ser.h"
#endif
#include MTK_WARP_SER_HEADER
static unsigned long jiffies=100;
struct module {int refs;};
static struct module module;
#define THIS_MODULE (&module)
typedef struct {struct mtk_warp_ser_request warp_request;
 struct module *cpu_proxy_module;bool cpu_proxy_release,cpu_hif_live,cpu_hif_quiet,cpu_ser_active;
} ERR_RECOVERY_CTRL_T;
typedef struct {ERR_RECOVERY_CTRL_T ErrRecoveryCtl;} RTMP_ADAPTER;
enum {WLAN_HOOK_SYS_UP=1,WLAN_HOOK_SYS_DOWN,WLAN_HOOK_SER,WLAN_HOOK_HIF_INIT,
 WLAN_HOOK_HIF_EXIT,WLAN_HOOK_TX,WLAN_HOOK_RX,WLAN_HOOK_ISR,WLAN_HOOK_DMA_SET,
 WLAN_HOOK_SUSPEND,WLAN_HOOK_RESUME};
#include DRIVER_PROXY_STATE_SOURCE
static bool whnat,probe_fail;
static int cold_calls,ser_calls,ring_exits,removes,next_status;
static bool try_module_get(struct module *m) {m->refs++;return true;}
static bool client_whnat_en_get(void *a) {(void)a;return whnat;}
static void client_driver_probe(void *a) {(void)a;if(!probe_fail)cpu_proxy_resources=1;}
static void client_driver_remove(void *a) {(void)a;removes++;}
static int warp_ring_init(void *a) {(void)a;cold_calls++;return next_status;}
int warp_ser_checked(void *a,struct mtk_warp_ser_request *r)
{(void)a;ser_calls++;if(r->phase==MTK_WARP_SER_STOP)r->stop_proven=1;return next_status;}
static void warp_ring_exit(void *a) {(void)a;ring_exits++;}
static void warp_wlan_tx(void *a,void *p) {(void)a;(void)p;}
static void warp_wlan_rx(void *a,void *p) {(void)a;(void)p;}
static void warp_isr_handler(void *a) {(void)a;}
static void client_dma_handler(void *a,void *p) {(void)a;(void)p;}
static void client_ser_handler(void *a,void *p) {(void)a;(void)p;}
static void warp_suspend_handler(void *a) {(void)a;}
static void warp_resume_handler(void *a) {(void)a;}
static void __attribute__((noreturn)) mtk_cpu_dma_fatal(int reason,int error)
{(void)reason;(void)error;assert(false);__builtin_trap();}
#include DRIVER_PROXY_HANDLE_SOURCE
static void release_after_return(RTMP_ADAPTER *a)
{
 assert(a->ErrRecoveryCtl.cpu_proxy_release && module.refs==1);
 module.refs--;a->ErrRecoveryCtl.cpu_proxy_module=NULL;a->ErrRecoveryCtl.cpu_proxy_release=false;
}
static void reset(RTMP_ADAPTER *a)
{memset(a,0,sizeof(*a));a->ErrRecoveryCtl.cpu_hif_quiet=true;next_status=0;probe_fail=false;}
int main(void)
{
 RTMP_ADAPTER a;reset(&a);
 assert(!cpu_proxy_fw_registered);
 a.ErrRecoveryCtl.warp_request.result=-EINPROGRESS;
 client_handle(WLAN_HOOK_HIF_INIT,&a,NULL);
 assert(a.ErrRecoveryCtl.warp_request.result==-ENODEV && !cold_calls);
 whnat=true;client_handle(WLAN_HOOK_SYS_UP,&a,NULL);
 assert(module.refs==1 && cpu_proxy_resources==1);
 whnat=false;a.ErrRecoveryCtl.warp_request.phase=MTK_WARP_SER_QUIESCE;
 client_handle(WLAN_HOOK_SER,&a,NULL);
 assert(ser_calls==1 && !a.ErrRecoveryCtl.warp_request.result);
 next_status=7;client_handle(WLAN_HOOK_HIF_INIT,&a,NULL);
 assert(a.ErrRecoveryCtl.warp_request.result==-EIO);
 next_status=0;client_handle(WLAN_HOOK_HIF_EXIT,&a,NULL);assert(ring_exits==1);
 client_handle(WLAN_HOOK_SYS_DOWN,&a,NULL);
 assert(removes==1 && !cpu_proxy_owner && module.refs==1);
 release_after_return(&a);
 reset(&a);whnat=false;client_handle(WLAN_HOOK_SYS_UP,&a,NULL);
 assert(cpu_proxy_resources==0 && module.refs==1);
 a.ErrRecoveryCtl.warp_request.cookie=41;a.ErrRecoveryCtl.warp_request.deadline=10000;
 for(unsigned phase=1;phase<=6;phase++) {
  a.ErrRecoveryCtl.warp_request.phase=phase;
  client_handle(WLAN_HOOK_SER,&a,NULL);
  assert(!a.ErrRecoveryCtl.warp_request.result);
 }
 assert(ser_calls==1 && a.ErrRecoveryCtl.warp_request.stop_proven &&
        a.ErrRecoveryCtl.warp_request.cleanup_done && !cpu_proxy_phase);
 a.ErrRecoveryCtl.warp_request.phase=1;a.ErrRecoveryCtl.warp_request.permission=2;
 client_handle(WLAN_HOOK_SER,&a,NULL);
 assert(a.ErrRecoveryCtl.warp_request.result==-ECANCELED);
 client_handle(WLAN_HOOK_SYS_DOWN,&a,NULL);assert(removes==1);
 release_after_return(&a);
 reset(&a);whnat=true;probe_fail=true;client_handle(WLAN_HOOK_SYS_UP,&a,NULL);
 assert(cpu_proxy_resources==2);whnat=false;
 client_handle(WLAN_HOOK_HIF_INIT,&a,NULL);
 assert(a.ErrRecoveryCtl.warp_request.result==-ENODEV);
 client_handle(WLAN_HOOK_SYS_DOWN,&a,NULL);release_after_return(&a);
 puts("proxy: actual history before config bypass, null/partial owner and ref handoff passed");
 return 0;
}
