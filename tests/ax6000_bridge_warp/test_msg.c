/* Actual complete warp_msg.c; deterministic scheduling only at OS/transport edges. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;
typedef pthread_mutex_t spinlock_t;typedef int wait_queue_head_t;
struct task_struct{int pid;};
struct completion{pthread_mutex_t lock;pthread_cond_t event;unsigned done;};
typedef struct{atomic_uint count;}refcount_t;
static void refcount_set(refcount_t*r,unsigned n){atomic_init(&r->count,n);}
static void refcount_inc(refcount_t*r){assert(atomic_fetch_add(&r->count,1)>0);}
static bool refcount_dec_and_test(refcount_t*r){unsigned n=atomic_fetch_sub(&r->count,1);assert(n);return n==1;}
#define CONFIG_WED_HW_RRO_SUPPORT 1
#define WED_HWRRO_MODE 1
#define WED_HWRRO_MODE_WOCPU 1
#define WED_RX_D_SUPPORT 1
#define GFP_ATOMIC 0
#define WARP_OK_STATUS 0
#define WARP_INVALID_LENGTH_STATUS (-2)
#define WARP_NULL_POINTER_STATUS (-3)
#define WARP_INVALID_PARA_STATUS (-4)
#define WARP_FAIL_STATUS (-1)
#define EXPORT_SYMBOL(x)
#define warp_dbg(...) ((void)0)
#define msecs_to_jiffies(x) (x)
#define wait_event_interruptible(q,c) do{(void)(q);(void)(c);}while(0)
#define init_waitqueue_head(q) (*(q)=0)
#define wake_up(q) ((void)(q))
#include "warp_msg.h"
#include "warp_cmd.h"
struct warp_entry{struct warp_msg_ctrl msg_ctrl[MAX_MODULE_ID];};
static struct warp_entry entry;
static _Thread_local int actor; /* 1 sender, 2 response, 0 coordinator */
static pthread_mutex_t schedule_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schedule_event=PTHREAD_COND_INITIALIZER;
static bool sent,wait_entered,response_claimed,release_response,force_timeout;
static bool pause_response,hold_transport,release_transport;
static int transport_result;
static u16 last_uni_id;
static struct completion*waiting_completion;
static atomic_uint allocations,callbacks,payloads;
static atomic_bool fail_payload,fail_node;
static pthread_t sender_thread;
static struct timespec deadline(void){struct timespec t;assert(!clock_gettime(CLOCK_REALTIME,&t));t.tv_sec+=3;return t;}
static void wait_predicate(bool*flag){struct timespec t=deadline();while(!*flag)assert(pthread_cond_timedwait(&schedule_event,&schedule_lock,&t)!=ETIMEDOUT);}
static void signal_flag(bool*flag){pthread_mutex_lock(&schedule_lock);*flag=true;pthread_cond_broadcast(&schedule_event);pthread_mutex_unlock(&schedule_lock);}
static void spin_lock_bh(spinlock_t*l){assert(!pthread_mutex_lock(l));}
static void spin_unlock_bh(spinlock_t*l){
 assert(!pthread_mutex_unlock(l));
 if(actor==2&&pause_response){pthread_mutex_lock(&schedule_lock);if(!response_claimed){response_claimed=true;pthread_cond_broadcast(&schedule_event);wait_predicate(&release_response);}pthread_mutex_unlock(&schedule_lock);}
}
#define spin_lock(l) spin_lock_bh(l)
#define spin_unlock(l) spin_unlock_bh(l)
#define spin_lock_init(l) assert(!pthread_mutex_init(l,NULL))
static void init_completion(struct completion*c){assert(!pthread_mutex_init(&c->lock,NULL));assert(!pthread_cond_init(&c->event,NULL));c->done=0;}
static void complete(struct completion*c){pthread_mutex_lock(&c->lock);c->done++;pthread_cond_broadcast(&c->event);pthread_mutex_unlock(&c->lock);}
static unsigned long wait_for_completion_timeout(struct completion*c,unsigned long timeout){
 assert(timeout>0);pthread_mutex_lock(&c->lock);
 pthread_mutex_lock(&schedule_lock);waiting_completion=c;wait_entered=true;pthread_cond_broadcast(&schedule_event);pthread_mutex_unlock(&schedule_lock);
 struct timespec t=deadline();while(!c->done&&!force_timeout)assert(pthread_cond_timedwait(&c->event,&c->lock,&t)!=ETIMEDOUT);
 unsigned long ret=c->done&&!force_timeout?1:0;if(ret)c->done--;
 pthread_mutex_lock(&schedule_lock);waiting_completion=NULL;pthread_mutex_unlock(&schedule_lock);
 pthread_mutex_unlock(&c->lock);return ret;
}
struct allocation{size_t size;};
static void warp_os_alloc_mem(unsigned char**out,size_t size,int flags){
 if((size==4&&atomic_exchange(&fail_payload,false))||(size==sizeof(struct warp_msg_wait_node)&&atomic_exchange(&fail_node,false))){*out=NULL;return;}
 struct allocation*a=malloc(sizeof(*a)+size);assert(a);a->size=size;*out=(unsigned char*)(a+1);atomic_fetch_add(&allocations,1);if(size==4)atomic_fetch_add(&payloads,1);
}
static void warp_os_free_mem(void*p){assert(p);struct allocation*a=(struct allocation*)p-1;
#ifndef TEST_WARP_MSG_OLD
 if(a->size==sizeof(struct warp_msg_wait_node)){struct warp_msg_wait_node*n=p;assert(!pthread_mutex_destroy(&n->recv_done.lock));assert(!pthread_cond_destroy(&n->recv_done.event));}
#endif
 atomic_fetch_sub(&allocations,1);free(a);
}
static struct warp_entry*warp_entry_get_by_idx(u8 idx){return idx?NULL:&entry;}
static bool kthread_should_stop(void){return false;}
static struct task_struct task={.pid=1};
static struct task_struct*kthread_run(int(*run)(void*),void*arg,const char*name){return &task;}
static void kthread_stop(struct task_struct*t){}
static void warp_evt_hdlr_queue_init(u8 idx){}
static void warp_evt_hdlr_queue_deinit(u8 idx){}
int warp_cmd_process_msg(u8 idx,unsigned char*m,u32 n){return 0;}
static int woif_bus_tx(u8 idx,u8*msg,u32 len){
 assert(len>=sizeof(struct warp_cmd_header));struct warp_cmd_header hdr;memcpy(&hdr,msg,sizeof(hdr));
 pthread_mutex_lock(&schedule_lock);last_uni_id=hdr.uni_id;sent=true;pthread_cond_broadcast(&schedule_event);if(hold_transport)wait_predicate(&release_transport);pthread_mutex_unlock(&schedule_lock);return transport_result;
}
#include "warp_msg.c"
static void callback(char*msg,u16 len,void*user){assert(actor==1&&pthread_equal(pthread_self(),sender_thread));assert(len==4&&msg&&*(u32*)user==0x13572468);u32 value;memcpy(&value,msg,4);assert(value==0x87654321);atomic_fetch_add(&callbacks,1);}
struct call{enum warp_msg_wait_type type;bool handler;int result;u32 output;};
static void*sender(void*data){actor=1;struct call*c=data;u32*user=malloc(4),*out=malloc(4);assert(user&&out);*user=0x13572468;*out=0xaaaaaaaa;
 struct warp_msg_cmd cmd={.param={.to_id=MODULE_ID_WO,.cmd_id=14,.wait_type=c->type,.timeout=10,.rsp_hdlr=c->handler?callback:NULL,.user_data=user},.rsp_msg=(u8*)out,.rsp_msg_len=c->type==WARP_MSG_WAIT_TYPE_RSP_MSG?4:0};
 c->result=warp_msg_send_cmd(0,&cmd);c->output=*out;free(user);free(out);return NULL;}
struct response{u16 id;int status;u32 bytes;};
static void*respond(void*data){actor=2;struct response*r=data;struct{struct warp_cmd_header hdr;u32 payload;}msg={.hdr={.cmd_id=14,.length=sizeof(msg),.uni_id=r->id,.status=r->status},.payload=r->bytes};warp_msg_handle_rsp(0,MODULE_ID_WO,(u8*)&msg,sizeof(msg));return NULL;}
static void await(bool*flag){pthread_mutex_lock(&schedule_lock);wait_predicate(flag);pthread_mutex_unlock(&schedule_lock);}
static void expire(void){pthread_mutex_lock(&schedule_lock);struct completion*c=waiting_completion;assert(c);pthread_mutex_unlock(&schedule_lock);pthread_mutex_lock(&c->lock);force_timeout=true;pthread_cond_broadcast(&c->event);pthread_mutex_unlock(&c->lock);}
static void fresh(void){assert(!atomic_load(&allocations));memset(&entry,0,sizeof(entry));warp_msg_wait_queue_init(0);sent=wait_entered=response_claimed=release_response=force_timeout=false;pause_response=hold_transport=release_transport=false;last_uni_id=0;transport_result=0;waiting_completion=NULL;atomic_store(&callbacks,0);atomic_store(&payloads,0);atomic_store(&fail_payload,false);atomic_store(&fail_node,false);actor=0;}
static void start(struct call*c){assert(!pthread_create(&sender_thread,NULL,sender,c));await(&sent);}
static void finish(void){warp_msg_wait_queue_deinit(0);assert(!entry.msg_ctrl[MODULE_ID_WO].msg_wait_q.head&&!entry.msg_ctrl[MODULE_ID_WO].msg_wait_q.size);assert(!atomic_load(&allocations));}
int main(void){pthread_t response_thread;struct call c;struct response response;
 /* Response takes the queue reference, then pauses before status/payload/complete.
  * Sender deadline expires and its stack/output are gone before it resumes. */
 for(int type=WARP_MSG_WAIT_TYPE_RSP_STATUS;type<=WARP_MSG_WAIT_TYPE_RSP_MSG;type++){
  fresh();pause_response=true;c=(struct call){.type=type,.handler=true};start(&c);await(&wait_entered);response=(struct response){last_uni_id,0,0x87654321};assert(!pthread_create(&response_thread,NULL,respond,&response));await(&response_claimed);expire();assert(!pthread_join(sender_thread,NULL));assert(c.result==-ETIMEDOUT&&c.output==0xaaaaaaaa&&!atomic_load(&callbacks));
#ifndef TEST_WARP_MSG_OLD
 assert(atomic_load(&allocations)==1);
#endif
 signal_flag(&release_response);assert(!pthread_join(response_thread,NULL));assert(!atomic_load(&allocations)&&!atomic_load(&callbacks));finish();
 }
 /* Timeout cancels the queue reference before a late response can find it. */
 fresh();c=(struct call){.type=WARP_MSG_WAIT_TYPE_RSP_MSG,.handler=true};start(&c);await(&wait_entered);response=(struct response){last_uni_id,0,0x87654321};expire();assert(!pthread_join(sender_thread,NULL));assert(!atomic_load(&allocations));respond(&response);assert(c.result==-ETIMEDOUT&&!atomic_load(&callbacks));finish();
 /* Successful synchronous handler uses sender context and a private payload. */
 fresh();c=(struct call){.type=WARP_MSG_WAIT_TYPE_RSP_MSG,.handler=true};start(&c);await(&wait_entered);response=(struct response){last_uni_id,0,0x87654321};respond(&response);assert(!pthread_join(sender_thread,NULL));assert(c.result==0&&c.output==0x87654321&&atomic_load(&callbacks)==1&&atomic_load(&payloads)==1);finish();
 /* Completion before send transport returns still owns private data safely. */
 fresh();hold_transport=true;c=(struct call){.type=WARP_MSG_WAIT_TYPE_RSP_STATUS,.handler=true};start(&c);response=(struct response){last_uni_id,0,0x87654321};respond(&response);assert(atomic_load(&allocations)==2&&!atomic_load(&callbacks));signal_flag(&release_transport);assert(!pthread_join(sender_thread,NULL));assert(!c.result&&atomic_load(&callbacks)==1);finish();
 /* Payload allocation failure must wake, return ENOMEM and never callback. */
 fresh();c=(struct call){.type=WARP_MSG_WAIT_TYPE_RSP_MSG,.handler=true};start(&c);await(&wait_entered);atomic_store(&fail_payload,true);response=(struct response){last_uni_id,0,0x87654321};respond(&response);assert(!pthread_join(sender_thread,NULL));assert(c.result==-ENOMEM&&c.output==0xaaaaaaaa&&!atomic_load(&callbacks));finish();
 /* Send failed after a concurrent response already claimed the queued node. */
 fresh();hold_transport=pause_response=true;transport_result=-EIO;c=(struct call){.type=WARP_MSG_WAIT_TYPE_RSP_STATUS,.handler=true};start(&c);response=(struct response){last_uni_id,0,0x87654321};assert(!pthread_create(&response_thread,NULL,respond,&response));await(&response_claimed);signal_flag(&release_transport);assert(!pthread_join(sender_thread,NULL));assert(c.result!=0&&atomic_load(&allocations)==1&&!atomic_load(&callbacks));signal_flag(&release_response);assert(!pthread_join(response_thread,NULL));finish();
 fresh();transport_result=-EIO;c=(struct call){.type=WARP_MSG_WAIT_TYPE_RSP_STATUS};start(&c);assert(!pthread_join(sender_thread,NULL));assert(c.result!=0);finish();
 /* Queue exit completes pending senders with shutdown without stealing their ref. */
 fresh();c=(struct call){.type=WARP_MSG_WAIT_TYPE_RSP_STATUS,.handler=true};start(&c);await(&wait_entered);warp_msg_wait_queue_deinit(0);assert(!pthread_join(sender_thread,NULL));assert(c.result==-ESHUTDOWN&&!atomic_load(&callbacks));finish();
 /* A claimed response remains independent of queue exit and a later timeout. */
 fresh();pause_response=true;c=(struct call){.type=WARP_MSG_WAIT_TYPE_RSP_MSG,.handler=true};start(&c);await(&wait_entered);response=(struct response){last_uni_id,0,0x87654321};assert(!pthread_create(&response_thread,NULL,respond,&response));await(&response_claimed);warp_msg_wait_queue_deinit(0);expire();assert(!pthread_join(sender_thread,NULL));assert(c.result==-ETIMEDOUT);signal_flag(&release_response);assert(!pthread_join(response_thread,NULL));finish();
 /* Local allocation and output-length validation fail before queue publication. */
 fresh();atomic_store(&fail_node,true);struct warp_msg_cmd failed={.param={.to_id=MODULE_ID_WO,.wait_type=WARP_MSG_WAIT_TYPE_RSP_STATUS}};assert(warp_msg_send_cmd(0,&failed)==-ENOMEM&&!sent&&!atomic_load(&allocations));finish();
 /* Response length error completes the waiter without copying caller data. */
 fresh();c=(struct call){.type=WARP_MSG_WAIT_TYPE_RSP_MSG,.handler=true};start(&c);await(&wait_entered);struct warp_cmd_header short_payload={.length=sizeof(short_payload),.uni_id=last_uni_id};actor=2;warp_msg_handle_rsp(0,MODULE_ID_WO,(u8*)&short_payload,sizeof(short_payload));actor=0;assert(!pthread_join(sender_thread,NULL));assert(c.result==WARP_INVALID_LENGTH_STATUS&&c.output==0xaaaaaaaa&&!atomic_load(&callbacks));finish();
#ifndef TEST_WARP_MSG_OLD
 /* Exit detaches all pending nodes, preserving each sender reference. */
 fresh();struct warp_msg_cmd pending={.param={.wait_type=WARP_MSG_WAIT_TYPE_RSP_STATUS}};
 struct warp_msg_wait_queue*q=&entry.msg_ctrl[MODULE_ID_WO].msg_wait_q;
 struct warp_msg_wait_node*a=warp_msg_wait_node_init(&pending),*b=warp_msg_wait_node_init(&pending);assert(a&&b);
 assert(warp_msg_wait_node_enq(q,a)>0&&warp_msg_wait_node_enq(q,b)>0&&q->size==2);warp_msg_wait_queue_deinit(0);
 assert(atomic_load(&allocations)==2&&a->rsp_status==-ESHUTDOWN&&b->rsp_status==-ESHUTDOWN&&atomic_load(&a->refs.count)==1&&atomic_load(&b->refs.count)==1);
 assert(warp_msg_wait_node_enq(q,a)==-ESHUTDOWN);warp_msg_wait_node_put(a);warp_msg_wait_node_put(b);finish();
#endif
 /* No ownership contract exists for an async callback's caller context. */
 fresh();struct warp_msg_cmd cmd={.param={.to_id=MODULE_ID_WO,.wait_type=WARP_MSG_WAIT_TYPE_NONE,.rsp_hdlr=callback}};assert(warp_msg_send_cmd(0,&cmd)==-EOPNOTSUPP&&!sent&&!atomic_load(&allocations));cmd.param.rsp_hdlr=NULL;assert(!warp_msg_send_cmd(0,&cmd)&&sent&&!last_uni_id&&!atomic_load(&allocations));finish();
 puts("actual full warp_msg.c: deterministic claim/timeout orders, stack/caller-buffer safety, sender callbacks, payload OOM, send-failure race and queue exit passed");return 0;}
