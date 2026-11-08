/* Actual participant/STOP helper and native RX classifier, host scheduling. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <linux/mtk_cpu_lifecycle.h>
typedef unsigned long long u64;
typedef uint32_t u32;
#define EXPORT_SYMBOL(x)
#define ASSERT_RTNL() ((void)0)
#define MTK_CPU_DMA_CONTROL_RACE 2
#define PSE_QDMA_TX_PORT 5
#define spin_lock_irqsave(l,f) do{(f)=0;pthread_mutex_lock(l);}while(0)
#define spin_unlock_irqrestore(l,f) do{(void)(f);pthread_mutex_unlock(l);}while(0)
#define atomic_read(p) atomic_load(p)
#define current current_task
static bool interrupt_context, fatal_context;
#define in_interrupt() interrupt_context
static inline bool mtk_cpu_profile_is_fatal(void){return fatal_context;}
static void*current_task=(void*)0x1234;
static pthread_mutex_t cpu_profile_lock=PTHREAD_MUTEX_INITIALIZER;
static struct mtk_cpu_life_state cpu_life;
static _Atomic int cpu_profile_error;
static bool configured=true;
static const void *cpu_wifi_participant,*cpu_stop_owner;
static bool cpu_wifi_live;
static u64 cpu_stop_cookie;
static bool cpu_stop_waiting,cpu_stop_ack;
static int cpu_stop_error;
static unsigned int warnings;
struct completion {unsigned int done;};
static struct completion cpu_stop_done;
static inline void complete(struct completion*c){c->done++;}
static inline void reinit_completion(struct completion*c){c->done=0;}
static inline unsigned long wait_for_completion_timeout(struct completion*c,unsigned long t){(void)t;if(!c->done)return 0;c->done--;return 1;}
static inline bool mtk_cpu_profile_restored(void){return configured&&!atomic_read(&cpu_profile_error);}
static inline void mtk_cpu_profile_note_fault(int e){int old=0;atomic_compare_exchange_strong(&cpu_profile_error,&old,e);configured=false;}
static jmp_buf fatal;
static void __attribute__((noreturn)) mtk_cpu_dma_fatal(int why,int error){(void)why;(void)error;longjmp(fatal,1);}
struct mtk_eth{void*dev;};
#define dev_warn_ratelimited(dev,fmt,...) do{(void)(dev);(void)(fmt);warnings++;}while(0)
#include "eth_helpers.inc"
#include "eth_reasons.inc"
#include "eth_rx.inc"
int main(void)
{const void *owner=(void*)0x9876;u64 cookie;
 assert(!mtk_cpu_control_permitted(owner,1));
 cpu_life.teardown_actor=(unsigned long)current;cpu_life.teardown_depth=1;
 assert(mtk_cpu_control_permitted(owner,1));interrupt_context=true;assert(!mtk_cpu_control_permitted(owner,1));interrupt_context=false;
 fatal_context=true;assert(!mtk_cpu_control_permitted(owner,1));fatal_context=false;
 atomic_store(&cpu_profile_error,-EIO);assert(mtk_cpu_control_permitted(owner,1));atomic_store(&cpu_profile_error,0);
 cpu_life.teardown_actor=0;cpu_life.teardown_depth=0;cpu_life.wifi_actor=(unsigned long)current;cpu_life.wifi_owner=owner;cpu_life.sequence=3;cpu_life.cleanup=MTK_CPU_CLEANUP_WIFI;
 assert(mtk_cpu_control_permitted(owner,3));assert(!mtk_cpu_control_permitted((void*)9,3));assert(!mtk_cpu_control_permitted(owner,4));cpu_life.cleanup=0;assert(!mtk_cpu_control_permitted(owner,3));cpu_life=(struct mtk_cpu_life_state){0};
 configured=false;assert(mtk_cpu_wifi_participant_register(owner)==-EAGAIN);assert(!cpu_wifi_participant);configured=true;
 assert(!mtk_cpu_wifi_participant_register(owner));assert(!cpu_wifi_live);assert(!mtk_cpu_wifi_participant_register(owner));assert(mtk_cpu_wifi_participant_register((void*)9)==-EEXIST);
 assert(!mtk_cpu_life_begin_fe(&cpu_life,(unsigned long)current,&cookie));assert(mtk_cpu_wifi_stop_prepare(cookie)==0); /* registered never-exposed idle */
 assert(!mtk_cpu_life_end_fe(&cpu_life,(unsigned long)current,cookie));assert(mtk_cpu_wifi_participant_set_live(owner,true)==-EBUSY);
 struct mtk_cpu_lifetime cold={0};assert(!mtk_cpu_life_destruct_enter(&cpu_life,(unsigned long)current,&cold));assert(!mtk_cpu_wifi_participant_set_live(owner,true));assert(cpu_wifi_live);assert(!mtk_cpu_life_destruct_exit(&cpu_life,(unsigned long)current,&cold));
 assert(!mtk_cpu_life_begin_fe(&cpu_life,(unsigned long)current,&cookie));assert(mtk_cpu_wifi_stop_prepare(cookie)==1);assert(mtk_cpu_wifi_participant_set_live(owner,false)==-EBUSY);
 assert(mtk_cpu_wifi_stop_result(owner,cookie,0,true)==-ESTALE); /* no WiFi actor */
 assert(!mtk_cpu_life_begin_wifi(&cpu_life,owner,0x5678,cookie,&cookie));
 assert(mtk_cpu_wifi_stop_result(owner,cookie+1,0,true)==-ESTALE);assert(!cpu_stop_ack);
 assert(!mtk_cpu_wifi_stop_result(owner,cookie,0,true));assert(mtk_cpu_wifi_stop_result(owner,cookie,0,true)==-ESTALE);assert(cpu_stop_done.done==1);
 assert(!mtk_cpu_wifi_stop_wait(100));assert(!cpu_stop_waiting);
 assert(mtk_cpu_wifi_stop_result(owner,cookie,-ETIMEDOUT,false)==-ETIMEDOUT);assert(atomic_read(&cpu_profile_error)==-ETIMEDOUT); /* nonwaiting still faults */
 if(!setjmp(fatal)){mtk_cpu_wifi_participant_unregister(owner);assert(0);}assert(cpu_wifi_participant==owner);
 assert(!mtk_cpu_life_end_wifi(&cpu_life,owner,0x5678,cookie));assert(!mtk_cpu_life_end_fe(&cpu_life,(unsigned long)current,cookie));
 struct mtk_cpu_lifetime permit={0};assert(!mtk_cpu_life_destruct_enter(&cpu_life,(unsigned long)current,&permit));assert(!mtk_cpu_wifi_participant_set_live(owner,false));mtk_cpu_wifi_participant_unregister(owner);assert(!cpu_wifi_participant);assert(!mtk_cpu_life_destruct_exit(&cpu_life,(unsigned long)current,&permit));
 struct mtk_eth eth={0};atomic_store(&cpu_profile_error,0);configured=true;
 assert(mtk_cpu_rx_consume(&eth,0x15,1));assert(!atomic_read(&cpu_profile_error)&&!warnings);
 unsigned int private_reasons[]={0x13,0x14,0x16,0x18,0x19,0x1b};
 for(unsigned i=0;i<sizeof(private_reasons)/sizeof(private_reasons[0]);i++){atomic_store(&cpu_profile_error,0);configured=true;assert(mtk_cpu_rx_consume(&eth,private_reasons[i],1));assert(atomic_read(&cpu_profile_error)==-EPROTO&&!configured);}
 atomic_store(&cpu_profile_error,0);configured=true;assert(mtk_cpu_rx_consume(&eth,0x1e,PSE_QDMA_TX_PORT));assert(atomic_read(&cpu_profile_error)==-EPROTO);
 atomic_store(&cpu_profile_error,0);configured=true;for(unsigned i=0;i<32;i++){if(i==0x15||i==0x13||i==0x14||i==0x16||i==0x18||i==0x19||i==0x1b)continue;assert(!mtk_cpu_rx_consume(&eth,i,1));}assert(!atomic_read(&cpu_profile_error));
 puts("actual Ethernet STOP identity/duplicate/failure/teardown guards and all 32 native RX reasons passed");return 0;}
