/* Host adapters for compiling the actual kernel helper, not its algorithm.
 * These model allocation/COW, RTNL, netdev references and a two-tag device
 * graph. They do not establish Linux RCU or real skb allocator correctness.
 */
#ifndef AX6000_BRIDGE_TEST_SHIM_H
#define AX6000_BRIDGE_TEST_SHIM_H
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <arpa/inet.h>
#define CONFIG_MTK_AP_BRIDGE 1
#define CONFIG_BRIDGE_VLAN_FILTERING 1
#define __user
#define __rcu
#define __aligned_u64 uint64_t __attribute__((aligned(8)))
typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32;
typedef int32_t __s32; typedef uint64_t __u64;
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint64_t u64; typedef int64_t s64; typedef uint16_t __be16;
#define S64_MAX INT64_MAX
#define BIT(n) (1UL << (n))
#define GFP_KERNEL 0
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define EXPORT_SYMBOL(x)
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define WARN_ON_ONCE(x) ((void)assert(!(x)))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define RCU_LOCKDEP_WARN(c,m) assert(!(c))
static bool profile_ready = true, profile_fatal;
static inline bool mtk_cpu_profile_ready(void) { return profile_ready; }
static inline bool mtk_cpu_profile_is_fatal(void) { return profile_fatal; }
typedef _Atomic long long atomic64_t;
typedef _Atomic int atomic_t;
#define ATOMIC64_INIT(v) (v)
#define atomic64_read(a) atomic_load(a)
static inline long long atomic64_cmpxchg(atomic64_t *a, long long o, long long n)
{ atomic_compare_exchange_strong(a, &o, n); return o; }
#define atomic_set(a,v) atomic_store((a),(v))
#define atomic_read(a) atomic_load(a)
#define atomic_inc(a) atomic_fetch_add((a),1)
#define atomic_dec_and_test(a) (atomic_fetch_sub((a),1)==1)
typedef pthread_mutex_t spinlock_t;
#define spin_lock_init(l) pthread_mutex_init((l),NULL)
#define spin_lock_irqsave(l,f) do { (f)=0; pthread_mutex_lock(l); } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); pthread_mutex_unlock(l); } while(0)
typedef int wait_queue_head_t;
#define init_waitqueue_head(w) (*(w)=0)
#define wait_event(w,c) do { (void)(w); while(!(c)) usleep(100); } while(0)
#define wake_up(w) ((void)(w))
static bool test_rtnl = true;
#define ASSERT_RTNL() assert(test_rtnl)
static inline bool rcu_read_lock_held(void) { return true; }
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define rcu_dereference(p) (p)
#define rtnl_dereference(p) (p)
#define rcu_replace_pointer(p,v,c) ({ __typeof__(p) _old=(p); (void)(c); (p)=(v); _old; })
struct rcu_head { int unused; };
#define call_rcu(p,fn) fn(p)
#define synchronize_rcu() ((void)0)
static int alloc_fail;
static inline void *kzalloc(size_t n, int flags)
{ (void)flags; if (alloc_fail) { alloc_fail--; return NULL; } return calloc(1,n); }
#define kfree(p) free(p)
static inline void *memchr_inv(const void *p, int c, size_t n)
{ const unsigned char *s=p; for(size_t i=0;i<n;i++) if(s[i]!=(unsigned char)c) return (void *)(s+i); return NULL; }
static int copy_fail;
static void (*copy_hook)(void);
static inline void *memdup_user(const void *p, size_t n)
{ void *q=malloc(n); if(!q) return ERR_PTR(-ENOMEM); memcpy(q,p,n); return q; }
static inline int copy_to_user(void *to,const void *from,size_t n)
{ if(copy_fail) {copy_fail--;return 1;} memcpy(to,from,n); if(copy_hook) { void(*f)(void)=copy_hook;copy_hook=NULL;f();} return 0; }
#define CAP_NET_ADMIN 1
struct net_device;
struct net { int user_ns, refs; struct net_device *devices; };
static bool capable = true;
static inline bool ns_capable(int ns,int cap) { (void)ns;(void)cap;return capable; }
static inline struct net *get_net(struct net *n) {n->refs++;return n;}
static inline void put_net(struct net *n) {n->refs--;}
#define net_eq(a,b) ((a)==(b))
#define NETREG_REGISTERED 1
struct net_bridge_port;
struct net_device { int ifindex,reg_state,refs; bool running,netns_local,is_vlan;
 struct net *net; struct net_device *next,*parent,*master; void *rx_handler;
 __be16 proto; u16 vid; struct net_bridge_port *brport; };
#define dev_net(d) ((d)->net)
#define dev_hold(d) ((d)->refs++)
#define dev_put(d) ((d)->refs--)
#define netif_running(d) ((d)->running)
#define is_vlan_dev(d) ((d)->is_vlan)
#define for_each_netdev(n,d) for((d)=(n)->devices;(d);(d)=(d)->next)
static inline struct net_device *dev_get_by_index(struct net *n,u32 idx)
{ struct net_device *d;for_each_netdev(n,d) if((u32)d->ifindex==idx){dev_hold(d);return d;}return NULL; }
static inline struct net_device *netdev_master_upper_dev_get(const struct net_device *d) {return d->master;}
#define netdev_master_upper_dev_get_rcu(d) netdev_master_upper_dev_get(d)
static inline struct net_device *vlan_dev_direct_real_dev(const struct net_device *d){return d->parent;}
static inline __be16 vlan_dev_vlan_proto(const struct net_device *d){return d->proto;}
static inline u16 vlan_dev_vlan_id(const struct net_device *d){return d->vid;}
static inline struct net_device *vlan_find_dev_rcu(struct net_device *p,__be16 proto,u16 vid)
{struct net_device*d;for_each_netdev(p->net,d)if(d->is_vlan&&d->parent==p&&d->proto==proto&&d->vid==vid)return d;return NULL;}
#define VLAN_VID_MASK 0xfff
#define ETH_HLEN 14
#define ETH_P_8021Q 0x8100
#define ETH_P_8021AD 0x88a8
struct ethhdr { u8 dest[6],src[6]; __be16 h_proto; } __attribute__((packed));
struct vlan_hdr {__be16 h_vlan_TCI,h_vlan_encapsulated_proto;} __attribute__((packed));
static inline bool eth_type_vlan(__be16 p){return p==htons(ETH_P_8021Q)||p==htons(ETH_P_8021AD);}
struct host_ext {int refs;unsigned char data[64] __attribute__((aligned(8)));};
struct sk_buff {unsigned char bytes[96]; int len,dataoff,macoff; bool mac_set,tagged; __be16 vlan_proto;u16 vid;struct host_ext*ext;};
#define SKB_EXT_MTK_BSS 0
static int ext_fail;
static inline void *skb_ext_find(const struct sk_buff*s,int id){(void)id;return s->ext?s->ext->data:NULL;}
static inline void *skb_ext_add(struct sk_buff*s,int id)
{struct host_ext*n;(void)id;if(ext_fail){ext_fail--;return NULL;}if(s->ext&&s->ext->refs==1)return s->ext->data;n=malloc(sizeof(*n));if(!n)return NULL;memset(n,0xa5,sizeof(*n));n->refs=1;if(s->ext){memcpy(n->data,s->ext->data,sizeof(n->data));s->ext->refs--;}s->ext=n;return n->data;}
static inline bool skb_vlan_tag_present(const struct sk_buff*s){return s->tagged;}
static inline u16 skb_vlan_tag_get_id(const struct sk_buff*s){return s->vid;}
static inline bool skb_mac_header_was_set(const struct sk_buff*s){return s->mac_set;}
static inline int skb_mac_offset(const struct sk_buff*s){return s->macoff;}
static inline void *skb_header_pointer(const struct sk_buff*s,int off,int len,void*buf)
{if(off < -s->dataoff || off+len>s->len)return NULL;memcpy(buf,s->bytes+s->dataoff+off,len);return buf;}
struct list_head {struct list_head *next,*prev;};
#define INIT_LIST_HEAD(h) ((h)->next=(h)->prev=(h))
#define list_for_each_entry(p,h,m) for((p)=container_of((h)->next,__typeof__(*(p)),m); &(p)->m!=(h); (p)=container_of((p)->m.next,__typeof__(*(p)),m))
static inline void list_add_tail(struct list_head*n,struct list_head*h){n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
struct net_bridge_vlan {u16 vid,flags;struct list_head vlist;};
struct net_bridge_vlan_group {struct list_head vlan_list;u16 pvid;};
struct net_bridge {struct net_device*dev;bool filtering;__be16 vlan_proto;struct net_bridge_vlan_group*vg;};
struct net_bridge_port {struct net_bridge*br;u64 flags;struct net_bridge_vlan_group*vg;};
#define BRIDGE_VLAN_INFO_UNTAGGED 4
#define BROPT_VLAN_ENABLED 1
static inline struct net_bridge_port*br_port_get_rtnl(const struct net_device*d){return d->brport;}
#define br_port_get_rcu(d) br_port_get_rtnl(d)
#define br_port_get_check_rcu(d) br_port_get_rtnl(d)
static inline bool br_opt_get(struct net_bridge*b,int o){(void)o;return b->filtering;}
#define nbp_vlan_group(p) ((p)->vg)
#define nbp_vlan_group_rcu(p) ((p)->vg)
#define br_vlan_group(b) ((b)->vg)
#define br_vlan_group_rcu(b) ((b)->vg)
static inline u16 br_get_pvid(const struct net_bridge_vlan_group*vg){return vg?vg->pvid:0;}
static inline bool br_vlan_should_use(const struct net_bridge_vlan*v){(void)v;return true;}
static inline struct net_bridge_vlan*br_vlan_find(struct net_bridge_vlan_group*vg,u16 vid){struct net_bridge_vlan*v;if(!vg)return NULL;list_for_each_entry(v,&vg->vlan_list,vlist)if(v->vid==vid)return v;return NULL;}
#define hweight8(v) __builtin_popcount((unsigned char)(v))
#endif
