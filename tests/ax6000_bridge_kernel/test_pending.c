/* Compile the real FE CPU worker with observable, fault-injected leaf APIs. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
typedef unsigned long long u64;
#define MTK_MAX_DEVS 3
#define MTK_RESETTING 1
#define MTK_EVENT_TOTAL_CNT 0
#define MTK_FE_START_RESET 0x2000
#define MTK_FE_RESET_DONE 0x2001
#define MTK_FE_CPU_PROFILE_ABORT 0x2010
#define MTK_CPU_WIFI_STOP_TIMEOUT_MS 5000
#define MTK_CPU_DMA_PROOF_LOST 3
#define MTK_CPU_DMA_STOP_FAILED 1
#define MTK_CPU_DMA_CONTROL_RACE 2
struct net_device {bool running;};
struct pins {void*p,*default_state;};
struct device {struct pins*pins;};
struct mtk_eth {struct device*dev;struct net_device*netdev[3];unsigned long state;struct {unsigned event;}reset;};
#define netif_running(d) ((d)->running)
#define msecs_to_jiffies(t) (t)
#define set_bit(n,p) (*(p)|=1UL<<(n))
#define __set_bit(n,p) set_bit(n,p)
#define clear_bit(n,p) (*(p)&=~(1UL<<(n)))
#define test_bit(n,p) (!!(*(p)&(1UL<<(n))))
static bool locked,restored;
static int begin_error,expected,stop_error,hw_error,restore_error,open_error_at;
static unsigned begins,ends,stops,opens,hw_calls,restore_calls,done,aborts,faults,touches,cleanup;
static int dma_users;
static jmp_buf terminal;
static void rtnl_lock(void){assert(!locked);locked=true;}
static void rtnl_unlock(void){assert(locked);locked=false;}
static int mtk_cpu_fe_ser_begin(u64*c){assert(locked);if(begin_error)return begin_error;*c=9;begins++;return 0;}
static void mtk_cpu_fe_ser_end(u64 c){assert(locked&&c==9&&!cleanup);ends++;}
static void mtk_cpu_profile_note_fault(int e){assert(e<0);faults++;}
static int mtk_cpu_wifi_stop_prepare(u64 c){assert(c==9&&locked&&!touches);return expected;}
static int mtk_cpu_wifi_stop_wait(unsigned long t){assert(!locked&&t==5000);return stop_error;}
static void call_netdevice_notifiers(unsigned event,struct net_device*d){assert(locked&&d);if(event==MTK_FE_START_RESET)assert(!touches);else if(event==MTK_FE_RESET_DONE){assert(restored&&opens==2&&dma_users==2);done++;}else if(event==MTK_FE_CPU_PROFILE_ABORT)aborts++;else assert(0);}
static void __attribute__((noreturn)) mtk_cpu_dma_fatal(int reason,int error){assert(reason&&error<0);longjmp(terminal,1);}
static int mtk_cpu_ser_cleanup_begin(u64 c){assert(c==9&&locked&&!cleanup);cleanup++;return 0;}
static void mtk_cpu_ser_cleanup_end(u64 c){assert(c==9&&locked&&cleanup==1);cleanup--;}
static void mtk_reset_event_update(struct mtk_eth*e,int i){(void)e;(void)i;}
static void mtk_hw_dump_all(struct mtk_eth*e){(void)e;touches++;}
static void mtk_save_qdma_cfg(struct mtk_eth*e){(void)e;touches++;}
static void mtk_prepare_for_reset(struct mtk_eth*e){(void)e;assert(cleanup);touches++;}
static void mtk_stop(struct net_device*d){assert(d&&cleanup&&locked&&dma_users>0);stops++;dma_users--;}
static void usleep_range(int a,int b){(void)a;(void)b;}
static int pinctrl_select_state(void*p,void*s){(void)p;(void)s;return 0;}
static int mtk_hw_init(struct mtk_eth*e,bool reset){(void)e;assert(reset&&locked&&cleanup&&dma_users==0);hw_calls++;return hw_error;}
static int mtk_cpu_profile_restore(void){assert(locked&&hw_calls==1&&!hw_error&&!opens);restore_calls++;if(!restore_error)restored=true;return restore_error;}
static int mtk_open(struct net_device*d){assert(d&&restored&&locked&&cleanup);opens++;dma_users++;return (int)opens==open_error_at?-ENODEV:0;}
static void mtk_mac_fe_reset_complete(struct mtk_eth*e,unsigned long r){(void)e;assert(r==3&&restored&&dma_users==2);}
static void mtk_restore_qdma_cfg(struct mtk_eth*e){(void)e;assert(restored&&dma_users==2);}
static void netif_carrier_off(struct net_device*d){assert(d);}
#include "eth_pending.inc"
static struct device dev;
static struct net_device n0={.running=true},n1={.running=true};
static struct mtk_eth eth={.dev=&dev,.netdev={&n0,&n1,NULL}};
static void reset(void){locked=restored=false;begin_error=stop_error=hw_error=restore_error=open_error_at=0;expected=1;begins=ends=stops=opens=hw_calls=restore_calls=done=aborts=faults=touches=cleanup=0;dma_users=2;eth.state=0;}
int main(void)
{reset();mtk_cpu_pending_work(&eth);assert(begins==1&&ends==1&&done==1&&!aborts&&!faults&&!cleanup&&!locked&&stops==2&&opens==2);
 reset();begin_error=-EBUSY;mtk_cpu_pending_work(&eth);assert(!begins&&!touches&&!faults&&!locked);
 reset();stop_error=-ETIMEDOUT;if(!setjmp(terminal)){mtk_cpu_pending_work(&eth);assert(0);}assert(!touches&&!stops&&!opens&&!done&&!ends&&!cleanup);
 reset();hw_error=-EIO;mtk_cpu_pending_work(&eth);assert(aborts==1&&!done&&!opens&&!restore_calls&&dma_users==0&&ends==1&&!cleanup&&!locked);
 reset();restore_error=-EIO;mtk_cpu_pending_work(&eth);assert(aborts==1&&!done&&!opens&&restore_calls==1&&dma_users==0&&ends==1&&!cleanup);
 reset();open_error_at=1;mtk_cpu_pending_work(&eth);assert(aborts==1&&!done&&stops==3&&opens==1&&dma_users==0&&ends==1&&!cleanup);
 reset();open_error_at=2;mtk_cpu_pending_work(&eth);assert(aborts==1&&!done&&stops==4&&opens==2&&dma_users==0&&ends==1&&!cleanup);
 reset();expected=0;mtk_cpu_pending_work(&eth);assert(!done&&!aborts&&!faults&&opens==2&&ends==1);
 puts("actual FE CPU worker: restore-before-open, STOP timeout fatal, hw/restore/open failure rollback and no false DONE passed");return 0;}
