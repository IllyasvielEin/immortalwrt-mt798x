#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <setjmp.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define CONFIG_MTK_AP_BRIDGE 1
#define CONFIG_WED_HW_RRO_SUPPORT 1
#define WED_TX_SUPPORT 1
#define WED_RX_SUPPORT 1
#define WED_HW_TX_SUPPORT 1
#define WED_RX_D_SUPPORT 1
#define WED_DYNAMIC_RXBM_SUPPORT 1
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define EXPORT_SYMBOL(x)
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x),(v), __ATOMIC_RELEASE)
#define cmpxchg(p,o,n) __sync_val_compare_and_swap(p,o,n)
#define time_after_eq(a,b) ((long)((a)-(b)) >= 0)
#define jiffies_to_msecs(n) (n)
#define msecs_to_jiffies(n) (n)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define WARN_ON_ONCE(x) (x)
static unsigned long jiffies;
static unsigned long current = 17;
static void usleep_range(int a, int b) { (void)a; (void)b; jiffies++; }
struct mutex { pthread_mutex_t lock; };
typedef pthread_mutex_t spinlock_t;
typedef int wait_queue_head_t;
#define mutex_init(m) pthread_mutex_init(&(m)->lock, NULL)
#define mutex_lock(m) pthread_mutex_lock(&(m)->lock)
#define mutex_unlock(m) pthread_mutex_unlock(&(m)->lock)
#define spin_lock_init(m) pthread_mutex_init(m, NULL)
#define spin_lock_irqsave(m,f) do { (f)=0; pthread_mutex_lock(m); } while(0)
#define spin_unlock_irqrestore(m,f) do { (void)(f); pthread_mutex_unlock(m); } while(0)
#define init_waitqueue_head(q) (*(q)=0)
#define wake_up_all(q) ((void)(q))
#define wait_event_timeout(q,c,t) ({ unsigned long left=(t); (void)(q); while (!(c) && left) {left--; jiffies++;} (unsigned long)((c)?1:0); })
struct tasklet_struct { unsigned queued, killed; };
struct timer_list { unsigned queued; };
static void tasklet_hi_schedule(struct tasklet_struct *t) { t->queued++; }
static void tasklet_schedule(struct tasklet_struct *t) { t->queued++; }
static void mod_timer(struct timer_list *t, unsigned long expiry) { (void)expiry; t->queued++; }
#include "actual-state.inc"
#include "regs/coda/wed_hw.h"
#include "regs/coda/wdma_hw.h"
struct wed_entry { unsigned long base_addr; struct warp_checked_state checked; void *dybm_ul_tasks; };
struct wdma_entry { unsigned long base_addr; };
struct wifi_hw { void *priv; u32 int_sta, int_mask; };
struct wifi_entry { unsigned long base_addr; struct wifi_hw hw; };
struct warp_bus { u32 pcie_ints_offset; };
struct warp_wo_ctrl {u8 cur_state, prev_state;};
struct warp_entry {
 struct wed_entry wed; struct wdma_entry wdma; struct wifi_entry wifi;
 struct { bool atc_en; } cfg;
 struct { struct warp_wo_ctrl wo_ctrl; } woif;
 u8 idx;
};
static struct warp_entry entry;
static struct warp_bus bus;
static bool missing, fatal;
static int fault, wo_result, alloc_result, prepare_fail, reset_count;
static int free_count, enable_count, config_count, drain_count, message_count, wo_exit_count, wo_exit_error;
static bool cancel_in_wo, cancel_after_wo, cancel_on_reset, late_response;
static unsigned callback_counter;
static u32 regs[3][4096], stuck[3][4096];
static unsigned int mmio_writes, fail_write;
static bool control_permit;
static struct warp_entry *warp_entry_get_by_privdata(void *owner) { (void)owner; return missing ? NULL : &entry; }
static struct warp_bus *warp_bus_get(void) { return &bus; }
static bool mtk_cpu_control_permitted(void *owner,u64 cookie) {(void)owner;(void)cookie;return control_permit;}
static bool mtk_cpu_profile_is_fatal(void) { return fatal; }
static void mtk_cpu_profile_note_fault(int error) { if (!fault) fault=error; }
static jmp_buf fatal_jump;
static void __attribute__((noreturn)) mtk_cpu_dma_fatal(int reason, int error) {
 (void)reason; (void)error; fatal=true; longjmp(fatal_jump, 1);
}
#define MTK_CPU_DMA_CONTROL_RACE 1
#define MTK_CPU_DMA_STOP_FAILED 2
#define MTK_CPU_DMA_INIT_EXPOSED 3
#define WDMA_INT_AGENT 1
#define ALL_INT_AGENT 2
#define WED_HW_CAP_DYN_RXBM 1
#define IS_WED_HW_CAP(w,c) (false)
#define WARP_DMA_TXRX 3
#define WARP_OK_STATUS 0
#define MODULE_ID_WO 1
#define WARP_MSG_WAIT_TYPE_RSP_STATUS 1
struct warp_msg_cmd { struct {int cmd_id,to_id,wait_type; unsigned timeout;} param; u8 *msg; unsigned msg_len; };
static struct mtk_warp_ser_request *active_request;
static void cancel(struct mtk_warp_ser_request *r) { WRITE_ONCE(r->cancelled,1); __sync_lock_test_and_set(&r->permission,2); }
static int warp_msg_send_cmd(u8 idx, struct warp_msg_cmd *cmd) {
 (void)idx; assert(cmd->param.timeout > 0); message_count++;
 if (cancel_in_wo) cancel(active_request);
 if (wo_result == -ETIMEDOUT || late_response) jiffies+=cmd->param.timeout;
 return wo_result;
}
static void wifi_chip_atc_set(struct wifi_entry *w,bool on) {(void)w; if(on)enable_count++;}
static void warp_atc_set_hw(struct wifi_entry *w,struct warp_bus *b,u8 i,bool on) {(void)w;(void)b;(void)i;if(on)enable_count++;}
static void warp_eint_ctrl_hw(struct wed_entry *w,bool on) {(void)w;if(on)enable_count++;}
static void warp_int_ctrl_hw(struct wed_entry *w,struct wifi_entry *f,struct wdma_entry *d,int a,bool on,u32 off,u8 i) {
 (void)w;(void)f;(void)d;(void)a;(void)off;(void)i;if(on)enable_count++;
}
static void warp_eint_init_hw(struct wed_entry *w) {(void)w;}
static void warp_checked_bm_drain(struct wed_entry *w) {assert(w->checked.stopping);assert(!w->checked.active);drain_count++;}
static void warp_dma_ctrl_hw(struct wed_entry *w,int cfg) {(void)w;(void)cfg;enable_count++;}
static int wed_tx_ring_reset(struct wed_entry *w) {(void)w;reset_count++;return 0;}
static int wdma_tx_ring_reset(struct wdma_entry *w) {(void)w;reset_count++;return 0;}
static int wed_rx_ring_reset(struct wed_entry *w) {(void)w;reset_count++;return 0;}
static void wed_rx_bm_exit(struct wed_entry *w) {(void)w;free_count++;}
static int wed_rx_bm_init(struct wed_entry *w, struct wifi_hw *h) {(void)w;(void)h;return alloc_result;}
static int regist_ul_dybm_task(struct wed_entry *w) {(void)w;return 0;}
static int woif_prepare_exit(struct warp_entry *w) {assert(w->wed.checked.quiescent);wo_exit_count++;return wo_exit_error;}
static int configure(void) {config_count++;return config_count==prepare_fail ? -ENOMEM : 0;}
static void warp_wed_init_hw(struct wed_entry *w,struct wdma_entry *d) {(void)w;(void)d;}
static void warp_wdma_init_hw(struct wed_entry *w,struct wdma_entry *d,u8 i) {(void)w;(void)d;(void)i;}
static void warp_wdma_ring_init_hw(struct wed_entry *w,struct wdma_entry *d) {(void)w;(void)d;}
#define CONFIG_FN(name) static int name(struct wed_entry *w,struct wifi_entry *f) {(void)w;(void)f;return configure();}
CONFIG_FN(warp_tx_ring_init_hw)
CONFIG_FN(warp_rx_ring_init_hw)
CONFIG_FN(warp_rx_data_ring_init_hw)
CONFIG_FN(warp_rx_bm_init_hw)
CONFIG_FN(warp_rx_rro_init_hw)
CONFIG_FN(warp_rx_route_qm_init_hw)
static int warp_checked_bm_prepare_hw(struct warp_entry *w) {(void)w;return configure();}
static int warp_set_rrocfg(struct wed_entry *w,u8 i,struct wifi_hw *h,u32 t) {(void)w;(void)i;(void)h;assert(t);return configure();}
static void io_read(unsigned int wdma,u32 reg,u32 *value) {assert(reg/4<4096);*value=regs[wdma][reg/4]|stuck[wdma][reg/4];}
static void io_write(unsigned int wdma,u32 reg,u32 value) {
 assert(reg/4<4096);
 mmio_writes++;if(mmio_writes==fail_write)return;
 if(reg == WED_MOD_RST && !wdma) {reset_count++; regs[0][reg/4]=0;if(cancel_on_reset)cancel(active_request);}
 else regs[wdma][reg/4]=value;
 if(cancel_after_wo && message_count)cancel(active_request);
}
#define MMIO_BANK(obj) ((void *)(obj)==(void *)&entry.wdma?1U:((void *)(obj)==(void *)&entry.wifi?2U:0U))
#define warp_io_read32(obj,reg,out) io_read(MMIO_BANK(obj),reg,out)
#define warp_io_write32(obj,reg,value) io_write(MMIO_BANK(obj),reg,value)
/* Exact production prototypes, OS/MMIO edges above are the only adapters. */
static int warp_checked_budget(struct mtk_warp_ser_request *);
static int warp_checked_grant(struct mtk_warp_ser_request *);
static void warp_checked_release(struct mtk_warp_ser_request *);
static int warp_checked_control_irq(struct warp_entry *,bool);
static int warp_checked_hw_stop(struct warp_entry *,struct mtk_warp_ser_request *);
static int warp_checked_hw_reset(struct warp_entry *,struct mtk_warp_ser_request *);
static int warp_checked_hw_enable(struct warp_entry *,struct mtk_warp_ser_request *);
static int warp_checked_wo(struct warp_entry *,u8,struct mtk_warp_ser_request *);
static int warp_checked_prepare(struct warp_entry *,struct mtk_warp_ser_request *);
static int warp_checked_resume(struct warp_entry *,struct mtk_warp_ser_request *);
#include "actual-control.inc"

static void fresh(struct mtk_warp_ser_request *r) {
 memset(&entry,0,sizeof(entry));memset(regs,0,sizeof(regs));memset(stuck,0,sizeof(stuck));
 memset(r,0,sizeof(*r));jiffies=1;r->deadline=100;r->cookie=42;active_request=r;
 missing=fatal=false;fault=wo_result=alloc_result=prepare_fail=reset_count=0;
 control_permit=false;mmio_writes=fail_write=0;
 entry.wed.base_addr=entry.wdma.base_addr=entry.wifi.base_addr=1;
 entry.wifi.hw.int_mask=0x300;entry.wifi.hw.int_sta=0x304;
 free_count=enable_count=config_count=drain_count=message_count=wo_exit_count=wo_exit_error=0;
 cancel_in_wo=cancel_after_wo=cancel_on_reset=late_response=false;
 regs[0][WED_TX_TKID_INTF/4]=0x2000000;
 entry.woif.wo_ctrl.cur_state=WO_STATE_ENABLE;
 warp_checked_init(&entry);
 entry.wed.checked.probe_complete=true;entry.wed.checked.stopping=false;
}
static int phase(struct mtk_warp_ser_request *r,u32 p) {r->phase=p;r->result=-EINPROGRESS;return warp_ser_checked(&entry,r);}
static void stopped(struct mtk_warp_ser_request *r) {
 assert(!phase(r,MTK_WARP_SER_QUIESCE));assert(!phase(r,MTK_WARP_SER_STOP));assert(r->stop_proven);
 assert(!free_count && !reset_count && !enable_count);
}
int main(void) {
 struct mtk_warp_ser_request r;
 /* Narrow control IRQ changes no DMA register and never enables data triggers. */
 fresh(&r);
 assert(warp_checked_control_irq(NULL,true)==-ENODEV&&!mmio_writes);
 const u32 trigger[]={WED_WPDMA_INT_CTRL_TX,WED_WPDMA_INT_CTRL_RX,WED_WPDMA_INT_CTRL_TX_FREE};
 const u32 enabled=BIT(0)|BIT(8);
 for(unsigned int i=0;i<ARRAY_SIZE(trigger);i++)regs[0][trigger[i]/4]=0xffffffff;
 regs[0][WED_CTRL/4]=0xfffffffe;
 regs[0][WED_GLO_CFG/4]=0x12345678;regs[1][WDMA_GLO_CFG/4]=0x87654321;
 assert(!warp_checked_control_irq(&entry,true));unsigned int writes=mmio_writes;
 assert(regs[0][WED_INT_MSK/4]==WARP_CONTROL_IRQ&&regs[0][WED_WPDMA_INT_MSK/4]==WARP_CONTROL_IRQ&&regs[2][entry.wifi.hw.int_mask/4]==WARP_CONTROL_IRQ);
 for(unsigned int i=0;i<ARRAY_SIZE(trigger);i++)assert(regs[0][trigger[i]/4]==(0xffffffff&~enabled));
 assert((regs[0][WED_CTRL/4]&(BIT(WED_CTRL_FLD_WDMA_INT_AGT_EN)|BIT(WED_CTRL_FLD_WPDMA_INT_AGT_EN)))==BIT(WED_CTRL_FLD_WPDMA_INT_AGT_EN));
 assert(!regs[0][WED_WDMA_INT_TRIG/4]&&!regs[1][WDMA_INT_MASK/4]&&!regs[1][WDMA_INT_GRP2/4]);
 assert(regs[0][WED_GLO_CFG/4]==0x12345678&&regs[1][WDMA_GLO_CFG/4]==0x87654321);
 assert(!warp_checked_control_irq(&entry,false));assert(!regs[0][WED_INT_MSK/4]&&!regs[0][WED_WPDMA_INT_MSK/4]&&!regs[2][entry.wifi.hw.int_mask/4]);
 for(unsigned int n=1;n<=writes;n++){
  fresh(&r);for(unsigned int i=0;i<ARRAY_SIZE(trigger);i++)regs[0][trigger[i]/4]=0xffffffff;
  regs[0][WED_INT_MSK/4]=regs[0][WED_WPDMA_INT_MSK/4]=regs[2][entry.wifi.hw.int_mask/4]=0xffffffff;
  regs[0][WED_CTRL/4]=0xfffffffe;regs[0][WED_WDMA_INT_TRIG/4]=regs[1][WDMA_INT_MASK/4]=regs[1][WDMA_INT_GRP2/4]=0xffffffff;
  fail_write=n;assert(warp_checked_control_irq(&entry,true)==-EIO);
  assert(!free_count&&!reset_count&&!enable_count);
 }

 fresh(&r);stopped(&r);assert(!phase(&r,MTK_WARP_SER_CLEANUP));assert(free_count==1&&r.cleanup_done);
 assert(!phase(&r,MTK_WARP_SER_PREPARE));assert(!r.stop_proven&&!enable_count);
 assert(!phase(&r,MTK_WARP_SER_RESUME));assert(enable_count>0);
 assert(!phase(&r,MTK_WARP_SER_COMPLETE));assert(!entry.wed.checked.phase);
 fresh(&r);missing=true;assert(phase(&r,MTK_WARP_SER_QUIESCE)==-ENODEV);assert(r.result==-ENODEV);
 for(int status=1;status<=7;status++) {
  fresh(&r);assert(!phase(&r,MTK_WARP_SER_QUIESCE));wo_result=status;
  assert(phase(&r,MTK_WARP_SER_STOP)==-EREMOTEIO);assert(!r.stop_proven);
  assert(!reset_count&&!free_count&&!enable_count);assert(entry.woif.wo_ctrl.cur_state==WO_STATE_ENABLE);
 }
 fresh(&r);assert(!phase(&r,MTK_WARP_SER_QUIESCE));wo_result=-ETIMEDOUT;
 assert(phase(&r,MTK_WARP_SER_STOP)==-ETIMEDOUT);assert(!r.stop_proven&&!free_count&&!reset_count);
 fresh(&r);assert(!phase(&r,MTK_WARP_SER_QUIESCE));cancel_in_wo=true;
 assert(phase(&r,MTK_WARP_SER_STOP)==-ECANCELED);assert(!r.stop_proven&&!free_count&&!reset_count);
 fresh(&r);assert(!phase(&r,MTK_WARP_SER_QUIESCE));late_response=true;
 assert(phase(&r,MTK_WARP_SER_STOP)==-ETIMEDOUT);assert(!r.stop_proven&&!free_count&&!reset_count);
 assert(entry.woif.wo_ctrl.cur_state==WO_STATE_ENABLE);
 fresh(&r);assert(!phase(&r,MTK_WARP_SER_QUIESCE));cancel_after_wo=true;
 assert(phase(&r,MTK_WARP_SER_STOP)==-ECANCELED);assert(!r.stop_proven&&!free_count&&!reset_count);
 const struct {bool wdma;u32 reg,bit;} busy[]={
  {0,WED_GLO_CFG,WED_GLO_CFG_FLD_TX_DMA_BUSY},{1,WDMA_GLO_CFG,WDMA_GLO_CFG_FLD_RX_DMA_BUSY},
  {0,WED_WDMA_GLO_CFG,WED_WDMA_GLO_CFG_FLD_RX_DRV_BUSY},{0,WED_CTRL,WED_CTRL_FLD_WED_TX_FREE_AGT_BUSY},
  {0,WED_CTRL,WED_CTRL_FLD_WED_TX_BM_BUSY},{0,WED_WPDMA_GLO_CFG,WED_WPDMA_GLO_CFG_FLD_TX_DRV_BUSY},
  {0,WED_WPDMA_GLO_CFG,WED_WPDMA_GLO_CFG_FLD_RX_DRV_BUSY},{0,WED_CTRL,WED_CTRL_FLD_WPDMA_INT_AGT_BUSY},
  {0,WED_WPDMA_RX_D_GLO_CFG,WED_WPDMA_RX_D_GLO_CFG_FLD_RX_DRV_BUSY},
  {0,WED_CTRL,WED_CTRL_FLD_WED_RX_RRO_QM_BUSY},{0,WED_CTRL,WED_CTRL_FLD_WED_RX_ROUTE_QM_BUSY},
  {1,WDMA_GLO_CFG,WDMA_GLO_CFG_FLD_TX_DMA_BUSY},{0,WED_CTRL,WED_CTRL_FLD_WDMA_INT_AGT_BUSY},
  {0,WED_GLO_CFG,WED_GLO_CFG_FLD_RX_DMA_BUSY},{0,WED_CTRL,WED_CTRL_FLD_WED_RX_BM_BUSY}};
 for(unsigned i=0;i<sizeof(busy)/sizeof(busy[0]);i++) {
  fresh(&r);assert(!phase(&r,MTK_WARP_SER_QUIESCE));stuck[busy[i].wdma][busy[i].reg/4]=BIT(busy[i].bit);
  assert(phase(&r,MTK_WARP_SER_STOP)==-ETIMEDOUT);assert(!r.stop_proven);
  assert(!reset_count&&!free_count&&!enable_count);
 }
 fresh(&r);stopped(&r);cancel(&r);assert(phase(&r,MTK_WARP_SER_CLEANUP)==-ECANCELED);
 assert(!reset_count&&!free_count&&!enable_count);
 fresh(&r);stopped(&r);/* FE reset invalidates WDMA: recheck before any cleanup. */
 stuck[1][WDMA_GLO_CFG/4]=BIT(WDMA_GLO_CFG_FLD_TX_DMA_BUSY);
 assert(phase(&r,MTK_WARP_SER_CLEANUP)==-ETIMEDOUT);assert(!r.stop_proven&&!entry.wed.checked.quiescent);assert(!reset_count&&!free_count);
 fresh(&r);stopped(&r);cancel_on_reset=true;
 assert(phase(&r,MTK_WARP_SER_CLEANUP)==-ECANCELED);assert(!r.stop_proven&&!free_count&&!enable_count);
 fresh(&r);stopped(&r);assert(!phase(&r,MTK_WARP_SER_CLEANUP));
 assert(!phase(&r,MTK_WARP_SER_PREPARE));cancel_in_wo=true;
 assert(phase(&r,MTK_WARP_SER_RESUME)==-ECANCELED);assert(!enable_count&&!r.stop_proven);
 for(int n=1;n<=8;n++) {
  fresh(&r);stopped(&r);assert(!phase(&r,MTK_WARP_SER_CLEANUP));int before=free_count;
  prepare_fail=n;assert(phase(&r,MTK_WARP_SER_PREPARE)<0);assert(!r.stop_proven);
  assert(free_count==before&&!enable_count);assert(phase(&r,MTK_WARP_SER_RESUME)<0&&!enable_count);
 }
 fresh(&r);stopped(&r);assert(!phase(&r,MTK_WARP_SER_CLEANUP));
 r.stop_proven=0;/* D's new HIF BASE can precede the W PREPARE callback. */
 assert(!phase(&r,MTK_WARP_SER_PREPARE));r.cookie=0;
 int messages=message_count;assert(!warp_checked_resume(&entry,&r));assert(message_count==messages+1);
 fresh(&r);stopped(&r);assert(!phase(&r,MTK_WARP_SER_CLEANUP));assert(!phase(&r,MTK_WARP_SER_PREPARE));
 r.cookie=0;entry.woif.wo_ctrl.cur_state=WO_STATE_DISABLE;
 assert(warp_checked_resume(&entry,&r)==-EPROTO&&!enable_count);
 fresh(&r);entry.wed.checked.exposed=true;entry.wed.checked.resources=true;
 warp_checked_shutdown(&entry);assert(wo_exit_count==1&&!message_count&&!free_count&&!reset_count);
 fresh(&r);entry.wed.checked.exposed=true;wo_exit_error=-ETIMEDOUT;
 if(!setjmp(fatal_jump)) {warp_checked_shutdown(&entry);assert(0);}
 assert(fatal&&wo_exit_count==1&&!free_count&&!reset_count);
 fresh(&r);r.deadline=jiffies;assert(warp_checked_budget(&r)==-ETIMEDOUT&&!message_count);
 fresh(&r);assert(!warp_checked_grant(&r));cancel(&r);warp_checked_release(&r);
 assert(r.permission==2&&warp_checked_grant(&r)==-ECANCELED);
 fresh(&r);struct tasklet_struct task={0};struct timer_list timer={0};
 assert(warp_checked_enter(&entry.wed));callback_counter++;warp_checked_exit(&entry.wed);
 warp_checked_tasklet(&entry.wed,&task,true);warp_checked_timer(&entry.wed,&timer,4);assert(task.queued==1&&timer.queued==1);
 assert(!phase(&r,MTK_WARP_SER_QUIESCE));assert(!warp_checked_enter(&entry.wed));
 warp_checked_tasklet(&entry.wed,&task,false);warp_checked_timer(&entry.wed,&timer,5);assert(task.queued==1&&timer.queued==1);
 fresh(&r);entry.wed.checked.active=1;assert(phase(&r,MTK_WARP_SER_QUIESCE)==-ETIMEDOUT);assert(!drain_count&&!free_count&&!reset_count);
 fresh(&r);r.phase=MTK_WARP_SER_STOP;assert(warp_ser_checked(&entry,&r)==-EPROTO);assert(!free_count&&!reset_count);
 puts("WARP actual control/MMIO helpers: lifecycle, 15 busy bits, FW statuses, cancellation, proof revocation and failed preparation passed");
 return 0;
}
