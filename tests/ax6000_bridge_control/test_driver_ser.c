/* Execute the production D SER control helpers against deterministic peers.
 * No MMIO/network/reset occurs. The actual fatal continuation is noreturn. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <stddef.h>
#define CONFIG_MTK_AP_BRIDGE 1
#define TRUE 1
#define FALSE 0
#define NDIS_STATUS_SUCCESS 0
#define NDIS_STATUS_FAILURE 1
#define NDIS_STATUS_INVALID_DATA 2
#define MTK_CPU_DMA_STOP_FAILED 1
#define MTK_CPU_DMA_INIT_EXPOSED 2
#define MTK_CPU_DMA_CONTROL_RACE 3
#define MTK_CPU_DMA_PROOF_LOST 4
#define ERROR_DETECT_STOP_PDMA 1
#define ERROR_DETECT_RESET_DONE 2
#define ERROR_DETECT_RECOVERY_DONE 4
#define ERROR_DETECT_N9_NORMAL_STATE 8
#define MCU_INT_PDMA0_STOP_DONE 1
#define MCU_INT_PDMA0_INIT_DONE 2
#define MCU_INT_PDMA0_RECOVERY_DONE 3
#define fRTMP_ADAPTER_START_UP 1
#define fRTMP_ADAPTER_MCU_SEND_IN_BAND_CMD 2
#define WDEV_TYPE_AP 1
#define BCN_UPDATE_ALL_AP_RENEW 1
#define WDEV_NUM_MAX 2
#define ALL_DMA 3
#define DMA_TX_RX 3
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define RTMP_CLEAR_FLAG(a,f) ((a)->Flags &= ~(f))
#define RTMP_SET_FLAG(a,f) ((a)->Flags |= (f))
#define RTMP_TEST_FLAG(a,f) ((a)->Flags & (f))
#define MTWF_DBG(...) ((void)0)
#define MTK_FE_START_RESET 0x2000
#define MTK_FE_RESET_DONE 0x2001
#define MTK_FE_CPU_PROFILE_ABORT 0x2010
#define NOTIFY_DONE 0
#define NOTIFY_OK 1
#define SER_ACTION_RECOVER 0
#define SER_SET_L1_RECOVER 0
#define DBDC_BAND0 0
#define cmpxchg(p,o,n) ({__typeof__(*(p)) _old=*(p);if(_old==(o))*(p)=(n);_old;})
#define xchg(p,n) ({__typeof__(*(p)) _old=*(p);*(p)=(n);_old;})
#define atomic_read(p) (*(p))
#define atomic_set(p,n) (*(p)=(n))
#define atomic_xchg(p,n) xchg(p,n)
#define spin_lock_irqsave(l,f) do {(void)(l);(f)=0;}while(0)
#define spin_unlock_irqrestore(l,f) do {(void)(l);(void)(f);}while(0)
#define RTMP_SPIN_LOCK_IRQSAVE(l,f) do {(void)(l);*(f)=0;}while(0)
#define RTMP_SPIN_UNLOCK_IRQRESTORE(l,f) do {(void)(l);(void)(f);}while(0)
#define time_after_eq(a,b) ((long)((a)-(b))>=0)
#define msecs_to_jiffies(x) (x)
#define jiffies_to_msecs(x) (x)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct work_struct {int unused;};
struct delayed_work {struct work_struct work; bool pending;};
#define to_delayed_work(w) container_of(w,struct delayed_work,work)
#define system_wq NULL
static unsigned long jiffies;
typedef uint64_t u64;
typedef uint32_t u32;
typedef unsigned long ULONG;
typedef int NTSTATUS;
typedef int PKT_TOKEN_CB;
#ifndef MTK_WARP_SER_HEADER
#define MTK_WARP_SER_HEADER "../../.analysis/kernel-next/include/linux/mtk_warp_ser.h"
#endif
#include MTK_WARP_SER_HEADER
enum {ERR_RECOV_STAGE_STOP_IDLE,ERR_RECOV_STAGE_STOP_PDMA0,
      ERR_RECOV_STAGE_RESET_PDMA0,ERR_RECOV_STAGE_STOP_IDLE_DONE,
      ERR_RECOV_STAGE_WAIT_N9_NORMAL};
struct wifi_dev {int wdev_type;};
typedef struct {int io_remap_lock;bool bPCIclkOff;} PCI_HIF_T;
typedef struct {
 int cpu_ser_lock,notify_fe,fe_reset_done,fe_result,cpu_ser_error;
 unsigned errRecovStage,cpu_saved_flags;
 bool cpu_ser_active,cpu_cleanup,cpu_hif_live,cpu_hif_quiet;
 bool cpu_qm_live,cpu_tm_live;
 struct delayed_work cpu_ser_watchdog;
 u64 cpu_watchdog_cookie;
 bool cpu_watchdog_drained;
 u64 fe_ser_cookie;
 struct mtk_warp_ser_request warp_request;
} ERR_RECOVERY_CTRL_T;
typedef ERR_RECOVERY_CTRL_T *P_ERR_RECOVERY_CTRL_T;
typedef struct {
 ERR_RECOVERY_CTRL_T ErrRecoveryCtl;
 struct {u32 ser_status;int ser_task;} HwCtrl;
 unsigned Flags;
 void *hdev_ctrl;
 struct wifi_dev *wdev_list[WDEV_NUM_MAX];
} RTMP_ADAPTER;
static RTMP_ADAPTER ad;
static PCI_HIF_T hif;
static PKT_TOKEN_CB token;
static jmp_buf fatal_target;
static int failure,phases,acks,waits,frees,enables,fatals,faults,actors,wakes;
static int commands;
static int wf_exits,queue_exits;
static bool normal_cleanup;
static void __attribute__((noreturn)) mtk_cpu_dma_fatal(int reason,int error);
static u64 join_seen;
static bool cleanup;
static void complete(int *c) {*c=1;}
static void RTCMDUp(int *t) {(void)t;wakes++;}
static bool queue_delayed_work(void *q,struct delayed_work *w,unsigned ms)
{(void)q;assert(ms==20000);if(w->pending)return false;w->pending=true;return true;}
static void cancel_delayed_work_sync(struct delayed_work *w) {w->pending=false;}
#include DRIVER_WATCHDOG_SOURCE
static int mtk_wifi_cpu_ser_phase(RTMP_ADAPTER *,u32);
static void __attribute__((noreturn)) mtk_cpu_dma_fatal(int reason,int error)
{assert(reason && error<0);fatals++;longjmp(fatal_target,1);}
static int mtk_cpu_wifi_ser_begin(void *owner,u64 join,u64 *cookie)
{assert(owner==&ad && !actors);join_seen=join;*cookie=join?join:54;actors=1;return 0;}
static void mtk_cpu_wifi_ser_end(void *owner,u64 cookie)
{assert(owner==&ad && cookie && actors);actors=0;}
static int mtk_cpu_ser_cleanup_begin(u64 cookie)
{assert(cookie && !cleanup);cleanup=true;return 0;}
static void mtk_cpu_ser_cleanup_end(u64 cookie)
{assert(cookie && cleanup);cleanup=false;}
static void mtk_cpu_profile_note_fault(int err) {assert(err<0);faults++;}
static void mtk_ap_adapter_ser_drain(RTMP_ADAPTER *a) {assert(a==&ad && actors);}
static int mtk_cpu_wifi_stop_result(void *owner,u64 cookie,int err,bool proof)
{assert(owner==&ad && cookie==42);if(!err){assert(proof && actors);acks++;}return 0;}
static int wait_for_completion_timeout(int *c,unsigned ms)
{
 (void)c;assert(ms && acks==1);waits++;
 if(failure==4)ad.ErrRecoveryCtl.fe_result=-ECANCELED;
 else ad.ErrRecoveryCtl.fe_result=0;
 return failure==5?0:1;
}
static void hook(RTMP_ADAPTER *a,ERR_RECOVERY_CTRL_T *c)
{
 struct mtk_warp_ser_request *r=&c->warp_request;
 assert(a==&ad && actors && (!cleanup || r->phase==MTK_WARP_SER_PREPARE || r->phase==MTK_WARP_SER_RESUME));
 if(failure==1)return; /* Missing listener leaves EINPROGRESS intact. */
 phases++;
 r->result=0;
 if(r->phase==MTK_WARP_SER_STOP){
  if(failure==2){r->result=7;return;} /* Positive FW errors are errors. */
  r->stop_proven=1;
  if(failure==3){c->cpu_ser_error=-ECANCELED;r->cancelled=1;r->permission=2;}
 }
 if(r->phase==MTK_WARP_SER_CLEANUP){assert(r->stop_proven);r->cleanup_done=1;}
 if(r->phase==MTK_WARP_SER_PREPARE){assert(r->cleanup_done);r->stop_proven=0;}
 if(r->phase==MTK_WARP_SER_RESUME && failure==7){r->cancelled=1;r->permission=2;}
 if(r->phase==MTK_WARP_SER_QUIESCE && failure==8)jiffies=r->deadline;
}
#define WLAN_HOOK_SER 1
#define WLAN_HOOK_CALL(h,a,c) hook(a,c)
static int ErrRecoverySetRecovStage(ERR_RECOVERY_CTRL_T *c,unsigned s) {c->errRecovStage=s;return TRUE;}
static int ErrRecoverySetStopRxDMA(ERR_RECOVERY_CTRL_T *c,int stop) {(void)c;(void)stop;return TRUE;}
static int ErrRecoveryMcuIntEvent(RTMP_ADAPTER *a,unsigned event) {(void)a;(void)event;return TRUE;}
static void *hc_get_hif_ctrl(void *ctrl) {(void)ctrl;return &hif;}
static void *hc_get_ct_cb(void *ctrl) {(void)ctrl;return &token;}
static void RtmpusecDelay(int us) {jiffies+=(unsigned)us/1000;}
static int chip_set_hif_dma(RTMP_ADAPTER *a,int dir,bool on) {(void)a;(void)dir;(void)on;return TRUE;}
static bool chip_wait_hif_dma_idle(RTMP_ADAPTER *a,int all,int round,int us)
{(void)a;(void)all;(void)round;(void)us;return failure!=9;}
static int tm_exit(RTMP_ADAPTER *a) {(void)a;assert(cleanup||normal_cleanup);queue_exits++;return failure==11?-EIO:0;}
static int qm_exit(RTMP_ADAPTER *a) {(void)a;assert(cleanup||normal_cleanup);queue_exits++;return failure==12?-EIO:0;}
static void chip_interrupt_disable(RTMP_ADAPTER *a) {(void)a;assert(cleanup||normal_cleanup);}
static void hif_reset_task_group(void *p) {(void)p;assert(cleanup);}
static void hif_free_irq(void *p) {(void)p;assert(cleanup);}
static void hif_reset_txrx_mem(void *p) {(void)p;assert(cleanup);frees++;}
static void token_deinit(PKT_TOKEN_CB **t) {(void)t;assert(cleanup);}
static int WfHifInit(RTMP_ADAPTER *a)
{
 int r;assert(cleanup);
 a->ErrRecoveryCtl.cpu_hif_quiet=false;
 r=mtk_wifi_cpu_ser_phase(a,MTK_WARP_SER_PREPARE);
 return failure==6?-ENOMEM:r;
}
static int qm_init(RTMP_ADAPTER *a) {(void)a;return 0;}
static int tm_init(RTMP_ADAPTER *a) {(void)a;return 0;}
static void chip_interrupt_enable(RTMP_ADAPTER *a) {(void)a;assert(cleanup);enables++;}
static void hif_dma_enable(void *p) {(void)p;assert(enables);}
static void ErrRecoveryEndDriverRestore(RTMP_ADAPTER *a) {(void)a;}
static void ba_refresh_bar_all(RTMP_ADAPTER *a) {(void)a;}
static bool HcIsRadioAcq(struct wifi_dev *w) {(void)w;return true;}
static void UpdateBeaconHandler(RTMP_ADAPTER *a,struct wifi_dev *w,int why)
{(void)a;(void)w;(void)why;}
static void WfSysPosExit(RTMP_ADAPTER *a)
{assert(!a->ErrRecoveryCtl.cpu_tm_live && !a->ErrRecoveryCtl.cpu_qm_live);wf_exits++;a->ErrRecoveryCtl.cpu_hif_live=false;}
#include DRIVER_INIT_FAILURE_SOURCE
#include DRIVER_SER_SOURCE
struct notifier_block {int unused;};
struct mtk_notifier_block {struct notifier_block nb;void *priv;};
static int notifier_from_errno(int error) {return error;}
static u64 mtk_cpu_fe_ser_cookie(void) {return 42;}
static void reinit_completion(int *c) {*c=0;}
static int CmdExtSER(RTMP_ADAPTER *a,int action,int recovery,int band)
{(void)a;(void)action;(void)recovery;(void)band;commands++;return 0;}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include DRIVER_NOTIFIER_SOURCE
#pragma GCC diagnostic pop
static void reset(bool fe)
{
 memset(&ad,0,sizeof(ad));memset(&hif,0,sizeof(hif));jiffies=1000;
 ad.Flags=3;ad.ErrRecoveryCtl.cpu_hif_live=true;
 ad.ErrRecoveryCtl.cpu_qm_live=ad.ErrRecoveryCtl.cpu_tm_live=true;
 ad.ErrRecoveryCtl.notify_fe=fe;ad.ErrRecoveryCtl.fe_ser_cookie=fe?42:0;
 ad.ErrRecoveryCtl.fe_result=fe?-EINPROGRESS:0;
 phases=acks=waits=frees=enables=fatals=faults=actors=wakes=commands=0;join_seen=0;cleanup=false;
 ad.ErrRecoveryCtl.cpu_watchdog_drained=true;
 normal_cleanup=false;wf_exits=queue_exits=0;
}
static int event(u32 e) {ad.HwCtrl.ser_status=e;return mtk_wifi_cpu_recover(&ad);}
static void success(bool fe)
{
 reset(fe);failure=0;
 assert(event(ERROR_DETECT_STOP_PDMA)==0);
 assert(event(ERROR_DETECT_RESET_DONE)==0);
 assert(event(ERROR_DETECT_RECOVERY_DONE)==0);
 assert(event(ERROR_DETECT_N9_NORMAL_STATE)==0);
 assert(phases==6 && frees==1 && enables==1 && !actors && !fatals && !faults);
 assert(acks==(int)fe && waits==(int)fe && join_seen==(fe?42:0));
 assert(ad.ErrRecoveryCtl.errRecovStage==ERR_RECOV_STAGE_STOP_IDLE);
 assert(ad.ErrRecoveryCtl.cpu_watchdog_drained && !ad.ErrRecoveryCtl.cpu_ser_watchdog.pending);
}
int main(void)
{
 success(false);success(true);
 for(int mode=1;mode<=9;mode++) {
  reset(mode==4 || mode==5);failure=mode;
  if(!setjmp(fatal_target)) {
   int r=event(ERROR_DETECT_STOP_PDMA);
   if(!r)r=event(ERROR_DETECT_RESET_DONE);
   assert(r!=0);
   assert(mode==4 || mode==5); /* Both have valid old D/W stop proofs. */
   assert(!actors && !fatals && !frees && !enables);
  } else {
   assert(mode!=4 && mode!=5 && fatals==1 && !enables);
   assert(frees==(mode==6 || mode==7));
   assert(!acks);
  }
 }
 reset(false);failure=0;
 assert(event(ERROR_DETECT_RESET_DONE)==NDIS_STATUS_INVALID_DATA && !phases);
 ad.ErrRecoveryCtl.warp_request.deadline=jiffies+100;
 assert(!mtk_wifi_ser_grant(&ad.ErrRecoveryCtl));
 ad.ErrRecoveryCtl.warp_request.cancelled=1;
 xchg(&ad.ErrRecoveryCtl.warp_request.permission,2);
 mtk_wifi_ser_release(&ad.ErrRecoveryCtl);
 assert(ad.ErrRecoveryCtl.warp_request.permission==2);
 /* No subsequent MCU interrupt: the real timeout wakes the same actor and
  * revoked/new DMA proof selects fatal, with no later recovery continuation. */
 reset(false);failure=0;
 assert(!event(ERROR_DETECT_STOP_PDMA) && !event(ERROR_DETECT_RESET_DONE));
 assert(actors && enables==1 && ad.ErrRecoveryCtl.cpu_ser_watchdog.pending);
 jiffies+=20000;
 mtk_wifi_cpu_ser_timeout(&ad.ErrRecoveryCtl.cpu_ser_watchdog.work);
 assert(wakes==1 && ad.ErrRecoveryCtl.warp_request.permission==2);
 if(!setjmp(fatal_target)) {event(ERROR_DETECT_RESET_DONE);assert(false);}
 assert(fatals==1 && enables==1 && ad.ErrRecoveryCtl.cpu_watchdog_drained);
 /* A stale work cookie cannot cancel a different active request. */
 reset(false);ad.ErrRecoveryCtl.cpu_ser_active=true;
 ad.ErrRecoveryCtl.cpu_watchdog_cookie=1;ad.ErrRecoveryCtl.warp_request.cookie=2;
 mtk_wifi_cpu_ser_timeout(&ad.ErrRecoveryCtl.cpu_ser_watchdog.work);
 assert(!wakes && !ad.ErrRecoveryCtl.cpu_ser_error);
 reset(false);failure=0;normal_cleanup=true;
 mtk_wifi_cpu_init_abort(&ad,true);
 assert(wf_exits==1 && queue_exits==2 && !ad.ErrRecoveryCtl.cpu_hif_live && ad.ErrRecoveryCtl.cpu_hif_quiet);
 mtk_wifi_cpu_init_abort(&ad,false); /* WfInit-owned unwind already ran. */
 assert(wf_exits==1 && queue_exits==2 && !fatals);
 for(volatile int mode=9;mode<=11;mode+=2) {
  reset(false);failure=mode;normal_cleanup=true;
  if(!setjmp(fatal_target)){mtk_wifi_cpu_init_abort(&ad,true);assert(false);}
  assert(fatals==1 && !wf_exits && !frees && !enables);
 }
 {
  struct mtk_notifier_block nb={.priv=&ad};
  reset(false);
  assert(mtk_fe_event(&nb.nb,MTK_FE_START_RESET,NULL)==NOTIFY_OK && commands==1);
  assert(mtk_fe_event(&nb.nb,MTK_FE_START_RESET,NULL)==-EBUSY && commands==1);
  assert(ad.ErrRecoveryCtl.fe_result==-EINPROGRESS && !ad.ErrRecoveryCtl.fe_reset_done);
  mtk_fe_event(&nb.nb,MTK_FE_RESET_DONE,NULL); /* Too early: not a stopped actor. */
  assert(ad.ErrRecoveryCtl.fe_result==-EINPROGRESS);
  ad.ErrRecoveryCtl.cpu_ser_active=true;
  ad.ErrRecoveryCtl.warp_request.phase=MTK_WARP_SER_STOP;
  ad.ErrRecoveryCtl.warp_request.stop_proven=1;
  mtk_fe_event(&nb.nb,MTK_FE_RESET_DONE,NULL);
  assert(!ad.ErrRecoveryCtl.fe_result && ad.ErrRecoveryCtl.fe_reset_done);
  ad.Flags=0;
  assert(mtk_fe_event(&nb.nb,MTK_FE_CPU_PROFILE_ABORT,NULL)==NOTIFY_OK);
  assert(ad.ErrRecoveryCtl.cpu_ser_error==-ECANCELED && ad.ErrRecoveryCtl.warp_request.permission==2);
  mtk_fe_event(&nb.nb,MTK_FE_RESET_DONE,NULL);
  assert(ad.ErrRecoveryCtl.fe_result==-ECANCELED); /* Late DONE never resurrects. */
 }
 puts("driver SER: production helpers, normal FE/autonomous and injected failures passed");
 return 0;
}
