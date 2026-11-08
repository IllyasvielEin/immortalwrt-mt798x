/* Actual production functions; host adapters only at MMIO/allocator/IRQ edges. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint32_t __le32;
typedef uint64_t dma_addr_t;typedef uint64_t resource_size_t;typedef int spinlock_t;
struct list_head{void*next,*prev;};struct mutex{int locked;};struct tasklet_struct{int scheduled;};struct page_frag_cache{void*va;int pagecnt_bias;};struct page{};struct sk_buff{};struct device{};struct platform_device{struct device dev;};
#define CONFIG_MTK_AP_BRIDGE 1
#define CONFIG_WED_HW_RRO_SUPPORT 1
#define CONFIG_WARP_CCIF_SUPPORT 1
#define MAX_REGION_SIZE 3
#define WED_BUS_RING_SIZE 4
#define KERNEL_VERSION(a,b,c) ((a)*65536+(b)*256+(c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6,12,112)
#define CCIF_PAGE_ALLOC 1
#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define __iomem
#define BIT(n) (1U<<(n))
#define CMD_BUFFER_LEN 1504
#define WED_CTL_SD_LEN0_SHIFT 16
#define WED_CTL_SD_LEN0 0x3fff0000U
#define WED_CTL_LAST_SEC0 BIT(30)
#define WED_CTL_DMA_DONE BIT(31)
#define IRQ_HANDLED 1
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define DMA_TO_DEVICE 1
#define DMA_FROM_DEVICE 2
#define WARP_OK_STATUS 0
#define WARP_ALREADY_DONE_STATUS 1
#define WO_FWDL_FAIL 1
#define MODULE_ID_WO 1
#define WARP_MSG_WAIT_TYPE_NONE 0
#define WARP_MSG_WAIT_TYPE_RSP_STATUS 1
#define WARP_MSG_TIMEOUT_DEFAULT 3000
#define WED_DUMMY_CR_WO_STATUS 1
#define WOIF_UNDEFINED 0
#define WOIF_HALT_DONE 7
#define WOCPU_TIMEOUT 5
#define WOIF_MT7986_RESET_PA 0x15194050
#define WOIF_MT7986_WO0_RESET BIT(5)
#define WOIF_MT7986_WO1_RESET BIT(0)
#define AP2WOCCIF_DEV_NODE "mediatek,ap2woccif"
#define AP2WO_TXCH_NUM 0
#define AP2WO_RXCH_NUM 1
#define AP2WO_RXCH_NUM_WO_EXCEPTION 7
#define MTK_CPU_DMA_PROOF_LOST 3
#define WRITE_ONCE(x,v) ((x)=(v))
#define READ_ONCE(x) (x)
#define unlikely(x) (x)
#define upper_32_bits(x) ((u32)((uint64_t)(x)>>32))
#define cpu_to_le32(x) (x)
#define warp_dbg(...) ((void)0)
#define spin_lock_init(x) (*(x)=0)
#define spin_lock_irqsave(l,f) do{(f)=0;assert(!*(l));*(l)=1;}while(0)
#define spin_unlock_irqrestore(l,f) do{(void)(f);assert(*(l));*(l)=0;}while(0)
#define spin_lock(l) do{assert(!*(l));*(l)=1;}while(0)
#define spin_unlock(l) do{assert(*(l));*(l)=0;}while(0)
#define spin_lock_bh(l) do{assert(!*(l));*(l)=1;}while(0)
#define spin_unlock_bh(l) do{assert(*(l));*(l)=0;}while(0)
#define time_before(a,b) ((a)<(b))
#define dma_wmb() ((void)0)
static struct mutex woif_reset_lock;
static void mutex_lock(struct mutex*m){assert(!m->locked);m->locked=1;}
static void mutex_unlock(struct mutex*m){assert(m->locked);m->locked=0;}
#include "actual-wo-state.inc"
struct warp_checked_state{struct mutex control;bool quiescent;};
struct wed_entry{struct warp_checked_state checked;};
struct warp_entry{u8 idx;struct woif_entry woif;struct platform_device*pdev;struct wed_entry wed;int wifi;};
struct warp_bus{unsigned long trig_flag;};struct warp_io{u32*base_addr;};
struct warp_msg_cmd{struct{int cmd_id,to_id,wait_type,timeout;void*rsp_hdlr;}param;void*msg;u32 msg_len;};struct wo_cmd_query{u32 query_arg0,query_arg1;};
struct device_node{};static struct device_node node;
typedef int irqreturn_t;typedef irqreturn_t(*irq_handler_t)(int,void*);
static int allocations,alloc_calls,fail_alloc_at,map_calls,mappings,fail_map_at,irq_owned,irq_mapping,taskkills,irq_sync,ccif_masks,ccif_enables;
static bool fail_node,fail_iomap,fail_irq,fail_request_irq,fail_base_read,fail_reset_read,reset_map_fail,halt_timeout;
static u32 reset_register,ccif_mmio[64],dummy;
static unsigned long jiffies;
static int command_result,command_count,fwdl_result,clear_result,clear_count;
static struct platform_device pdev;
static jmp_buf fatal;
static void*alloc(size_t n){alloc_calls++;if(alloc_calls==fail_alloc_at)return NULL;void*p=calloc(1,n);assert(p);allocations++;return p;}
static void dealloc(void*p){if(p){allocations--;free(p);}}
static void warp_os_alloc_mem(unsigned char**p,size_t n,int flag){*p=alloc(n);}
static void warp_os_free_mem(void*p){dealloc(p);}
static void*page_frag_alloc(struct page_frag_cache*p,size_t n,int flag){return alloc(n);}
static void page_frag_free(void*p){dealloc(p);}
static dma_addr_t dma_map_single(struct device*d,void*p,size_t n,int dir){map_calls++;if(map_calls==fail_map_at)return UINT64_MAX;mappings++;return 0x10000+map_calls*0x1000;}
static bool dma_mapping_error(struct device*d,dma_addr_t a){return a==UINT64_MAX;}
static void dma_unmap_single(struct device*d,dma_addr_t a,size_t n,int dir){assert(a!=UINT64_MAX);mappings--;}
static int warp_dma_buf_alloc(struct platform_device*p,struct warp_dma_buf*d,u32 n){d->alloc_va=alloc(n);if(!d->alloc_va)return -ENOMEM;d->alloc_size=n;d->alloc_pa=0x20000+alloc_calls*0x1000;return 0;}
static void warp_dma_buf_free(struct platform_device*p,struct warp_dma_buf*d){dealloc(d->alloc_va);memset(d,0,sizeof(*d));}
static struct page*virt_to_page(void*p){return p;}
static void __page_frag_cache_drain(struct page*p,int n){assert(0);}
static struct device_node*of_find_compatible_node(void*a,void*b,const char*c){return fail_node?NULL:&node;}
static void of_node_put(struct device_node*n){assert(n==&node);}
static void*of_iomap(struct device_node*n,int idx){return fail_iomap?NULL:ccif_mmio;}
static unsigned int irq_of_parse_and_map(struct device_node*n,int idx){if(fail_irq)return 0;irq_mapping++;return 300;}
static void irq_dispose_mapping(unsigned irq){assert(irq==300);irq_mapping--;}
static int request_irq(unsigned irq,irq_handler_t fn,unsigned long flags,const char*n,void*data){assert(irq==300);if(fail_request_irq)return -EBUSY;irq_owned++;return 0;}
static void synchronize_irq(unsigned irq){assert(irq==300);irq_sync++;}
static void free_irq(unsigned irq,void*data){assert(irq==300);assert(irq_owned);irq_owned--;}
static void*ioremap(uint64_t addr,size_t n){assert(addr==WOIF_MT7986_RESET_PA);return reset_map_fail?NULL:&reset_register;}
static void iounmap(void*p){assert(p==&reset_register||p==ccif_mmio);}
static u32 readl(void*p){return p==&reset_register&&fail_reset_read?reset_register^BIT(5):*(u32*)p;}
static void writel(u32 v,void*p){*(u32*)p=v;}
static void warp_ccif_isr_ctrl(void*hw,bool en){assert(hw);if(en)ccif_enables++;else ccif_masks++;}
static void warp_ccif_tx_ring_get_hw(void*h,struct warp_bus_ring*r){r->hw_desc_base=0;r->hw_cnt_addr=4;r->hw_cidx_addr=8;}
static void warp_ccif_rx_ring_get_hw(void*h,struct warp_bus_ring*r){r->hw_desc_base=16;r->hw_cnt_addr=20;r->hw_cidx_addr=24;}
static void warp_ccif_kickout(void*h){}
static void warp_ccif_set_ack(void*h){}
static u32 warp_ccif_get_rxchnum(void*h){return 0;}
static void warp_ccif_clear_int(void*h){}
static void tasklet_init(struct tasklet_struct*t,void(*f)(unsigned long),unsigned long d){t->scheduled=0;}
static void tasklet_kill(struct tasklet_struct*t){taskkills++;t->scheduled=0;}
static void tasklet_hi_schedule(struct tasklet_struct*t){t->scheduled++;}
static struct warp_entry*current_warp;
static struct warp_entry*warp_entry_get_by_idx(u8 idx){return current_warp;}
static u32 rx_didx,rx_cidx;
static int rx_messages,rx_cpu_sync,rx_device_sync;
static void warp_woif_bus_get_rx_res(struct woif_bus*b,u32*d,u32*c){*d=rx_didx;*c=rx_cidx;}
static void warp_woif_bus_set_rx_res(struct woif_bus*b,u32 c){rx_cidx=c;}
static void dma_sync_single_for_cpu(struct device*d,dma_addr_t a,size_t n,int dir){rx_cpu_sync++;}
static void dma_sync_single_for_device(struct device*d,dma_addr_t a,size_t n,int dir){rx_device_sync++;}
static void warp_cmd_process_wo_ring(u8 idx,u8*msg,u32 len){assert(len==4);rx_messages++;}
static void wifi_chip_reset(int*wifi){}
static void warp_dump_dmacb(struct warp_dma_cb*c){}
static void warp_dump_raw(const char*n,u8*a,u32 size){}
#define bus_to_tx_ring(b) (&(b)->tx_ring.ring)
#define bus_to_rx_ring(b) (&(b)->rx_ring.ring)
static void warp_woif_bus_init_hw(struct woif_bus*b){struct ccif_entry*c=b->hw;struct warp_bus_ring*r[2]={bus_to_tx_ring(b),bus_to_rx_ring(b)};for(int i=0;i<2;i++){c->base_addr=ccif_mmio;ccif_mmio[r[i]->hw_desc_base/4]=r[i]->cell[0].alloc_pa;ccif_mmio[r[i]->hw_cnt_addr/4]=WED_BUS_RING_SIZE;ccif_mmio[r[i]->hw_cidx_addr/4]=0;}}
#define warp_io_read32(a,r,p) (*(p)=((struct warp_io*)(a))->base_addr[(r)/4]^(fail_base_read?1:0))
static int warp_dummy_cr_set(struct wed_entry*w,u8 idx,u32 v){dummy=v;return 0;}
static int warp_dummy_cr_get(struct wed_entry*w,u8 idx,u32*v){*v=dummy;return 0;}
static void usleep_range(int a,int b){jiffies++;}
static int warp_msg_send_cmd(u8 idx,struct warp_msg_cmd*c){command_count++;if(!command_result&&c->param.cmd_id==WO_CMD_CHANGE_STATE&&*(u8*)c->msg==WO_STATE_HALT&&!halt_timeout)dummy=WOIF_HALT_DONE;return command_result;}
static int warp_fwdl(struct warp_entry*w){return fwdl_result;}
static int warp_fwdl_clear(struct warp_entry*w){clear_count++;return clear_result;}
static void warp_woctrl_enter_state(struct warp_entry*w,u8 state){assert(0);}
static void warp_woctrl_init_state(struct warp_entry*w){w->woif.wo_ctrl.cur_state=WO_STATE_ENABLE;}
static void wo_proc_init(struct warp_entry*w){}
static void wo_exep_proc_init(struct warp_entry*w){}
static void wo_log_event_register(struct warp_entry*w){}
static void __attribute__((noreturn)) mtk_cpu_dma_fatal(int why,int err){longjmp(fatal,1);}
#include "actual-wo.inc"
static struct warp_entry fresh(void){struct warp_entry w={.pdev=&pdev};w.wed.checked.quiescent=true;return w;}
static void reset_faults(void){alloc_calls=map_calls=0;fail_alloc_at=fail_map_at=0;fail_node=fail_iomap=fail_irq=fail_request_irq=fail_base_read=fail_reset_read=reset_map_fail=halt_timeout=false;command_result=command_count=fwdl_result=clear_result=clear_count=0;dummy=0;reset_register=0x21;jiffies=0;}
static void no_resources(void){assert(!allocations&&!mappings&&!irq_owned&&!irq_mapping);}
int main(void){struct warp_bus b={0};
 /* Every allocation and mapping in our cold chain is before BASE exposure. */
 for(int i=1;i<=12;i++){reset_faults();struct warp_entry w=fresh();fail_alloc_at=i;int ret=woif_init(&w,&b);assert(ret<0);assert(!woif_dma_exposed(&w.woif));no_resources();}
 for(int i=1;i<=9;i++){reset_faults();struct warp_entry w=fresh();fail_map_at=i;assert(woif_init(&w,&b)<0);assert(!woif_dma_exposed(&w.woif));no_resources();}
 for(int i=0;i<4;i++){reset_faults();struct warp_entry w=fresh();if(i==0)fail_node=true;if(i==1)fail_iomap=true;if(i==2)fail_irq=true;if(i==3)fail_request_irq=true;assert(woif_init(&w,&b)<0);assert(!woif_dma_exposed(&w.woif));no_resources();}
 reset_faults();struct warp_entry w=fresh();fail_base_read=true;assert(woif_init(&w,&b)==-EIO);assert(woif_dma_exposed(&w.woif));assert(allocations&&mappings&&irq_owned);fail_base_read=false;w.woif.exit_prepared=true;w.woif.reset_held=true;woif_exit(&w.woif);no_resources();
 assert(!w.woif.bus.hw&&!w.woif.bus.exposed&&!w.woif.bus.ready&&!w.woif.exit_prepared&&!w.woif.bus.task_initialized&&!w.woif.bus.irq_requested&&!w.woif.bus.irq_mapped&&!w.woif.bus.tx_ring.initialized&&!w.woif.bus.rx_ring.initialized);
 /* A complete close removes this HIF before the next normal SYS_UP. */
 assert(!woif_init(&w,&b));assert(w.woif.bus.ready);assert(!woif_prepare_exit(&w));woif_exit(&w.woif);no_resources();
 reset_faults();w=fresh();assert(!woif_init(&w,&b));assert(woif_dma_exposed(&w.woif));assert(w.woif.bus.ready&&irq_owned==1);assert(!(reset_register&BIT(5))); /* cold parked until actual FWDL boot release */
 assert(!woif_boot_release(&w));assert(reset_register&BIT(5));
 current_warp=&w;int before=mappings;rx_didx=1;rx_cidx=0;
 struct bus_dmad*rxd=w.woif.bus.rx_ring.ring.cell[1].alloc_va;rxd->ctrl=WED_CTL_DMA_DONE|(4U<<WED_CTL_SD_LEN0_SHIFT);bus_rx_task((unsigned long)&w.woif);assert(rx_messages==1&&rx_cpu_sync==1&&rx_device_sync==1&&mappings==before);
 w.woif.bus.stopping=true;bus_rx_schedule(&w.woif.bus);assert(!w.woif.bus.rx_task.scheduled);bus_rx_task((unsigned long)&w.woif);assert(rx_messages==1);w.woif.bus.stopping=false;
 w.woif.wo_ctrl.cur_state=WO_STATE_SER_RESET;halt_timeout=true;assert(woif_prepare_exit(&w)==-ETIMEDOUT);assert(allocations&&mappings&&irq_owned);assert(!w.woif.exit_prepared);halt_timeout=false;
 command_result=-EIO;assert(woif_prepare_exit(&w)==-EIO);assert(!w.woif.exit_prepared);command_result=0;
 assert(!woif_prepare_exit(&w));assert(w.woif.exit_prepared&&w.woif.reset_held);assert(!(reset_register&BIT(5)));assert(reset_register&BIT(0)); /* sibling not reset */
 int count=command_count;assert(!woif_prepare_exit(&w));assert(command_count==count);assert(!w.woif.bus.ready&&w.woif.bus.stopping);woif_exit(&w.woif);no_resources();
 reset_faults();w=fresh();assert(!woif_init(&w,&b));fail_reset_read=true;assert(woif_prepare_exit(&w)==-EIO);assert(allocations&&mappings&&irq_owned);fail_reset_read=false;w.woif.exit_prepared=true;w.woif.reset_held=true;woif_exit(&w.woif);no_resources();
 puts("actual WO/CCIF: every cold alloc/map/IRQ failure unwinds before exposure; published failures retain; fresh HALT/timeout/reset polarity/idempotent shutdown passed");return 0;}
