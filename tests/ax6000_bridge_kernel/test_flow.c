/* The functions under test are extracted, unchanged, from kernel-next. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#define CONFIG_MTK_AP_BRIDGE 1
#define IS_ENABLED(x) (x)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define likely(x) (x)
#define unlikely(x) (x)
#define HZ 100
#define FLOW_OFFLOAD_DIR_REPLY 1
#define FLOW_OFFLOAD_DIR_ORIGINAL 0
#define NF_FLOW_HW 1
#define NF_FLOW_TEARDOWN 2
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
typedef uint32_t u32;
struct nf_flowtable {int rhashtable;};
struct flow_offload_tuple {int dir;};
struct flow_offload_tuple_rhash {int node;struct flow_offload_tuple tuple;};
struct flow_offload {u32 timeout;unsigned long flags;void*ct;struct flow_offload_tuple_rhash tuplehash[2];};
static unsigned int effects;
static u32 nf_flowtable_time_stamp = 99;
static const int nf_flow_offload_rhash_params;
static inline u32 flow_offload_get_timeout(struct flow_offload*f){(void)f;effects++;return 100;}
static inline int rhashtable_insert_fast(int*h,int*n,int p){(void)h;(void)n;(void)p;effects++;return 0;}
static inline void rhashtable_remove_fast(int*h,int*n,int p){(void)h;(void)n;(void)p;effects++;}
static inline void *rhashtable_lookup(int*h,void*t,int p){(void)h;(void)t;(void)p;effects++;return NULL;}
static inline void nf_ct_offload_timeout(void*c){(void)c;effects++;}
static inline bool nf_flowtable_hw_offload(struct nf_flowtable*t){(void)t;effects++;return true;}
static inline void __set_bit(int n,unsigned long*p){*p|=1UL<<n;effects++;}
static inline void nf_flow_offload_add(struct nf_flowtable*t,struct flow_offload*f){(void)t;(void)f;effects++;}
static inline bool test_bit(int n,unsigned long*p){return !!(*p&(1UL<<n));}
static inline bool nf_ct_is_dying(void*c){(void)c;effects++;return false;}
#include "flow_functions.inc"
int main(void)
{struct nf_flowtable t={0};struct flow_offload f={.timeout=77};struct flow_offload_tuple tuple={0};
 assert(flow_offload_add(&t,&f)==-EOPNOTSUPP);assert(f.timeout==77&&!f.flags&&!effects);
 flow_offload_refresh(&t,&f,true);assert(f.timeout==77&&!f.flags&&!effects);
 assert(!flow_offload_lookup(&t,&tuple));assert(!effects);
 puts("fixed nf_flow add/refresh/lookup: actual functions reject before side effects");return 0;}
