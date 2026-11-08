/* Compiles production mtk_ap_bridge.c and the new production bridge checker. */
#include "shim.h"
#include <linux/mtk_ap_bridge.h>
#include "bridge_checker.inc"
#include MTK_AP_SOURCE

_Static_assert(sizeof(struct mtk_ap_bridge_domain) == 2112, "domain ABI");
_Static_assert(sizeof(struct mtk_ap_bridge_request) == 2184, "request ABI");
_Static_assert(offsetof(struct mtk_ap_bridge_request, owner) == 16, "u64 ABI alignment");
static struct net ns = {.refs = 1};
static struct net_device base = {.ifindex=1,.reg_state=1,.running=true,.refs=1,.net=&ns};
static struct net_device bridge = {.ifindex=2,.reg_state=1,.running=true,.refs=1,.net=&ns};
static struct net_device vlan = {.ifindex=3,.reg_state=1,.running=true,.refs=1,.net=&ns,
 .netns_local=true,.is_vlan=true,.parent=&base,.proto=0,.vid=100};
static struct net_bridge_vlan_group pvg,bvg;
static struct net_bridge_vlan pv1,bv1,pvextra;
static struct net_bridge br = {.dev=&bridge,.vlan_proto=0,.vg=&bvg};
static struct net_bridge_port bp = {.br=&br,.vg=&pvg};
static struct mtk_ap_bridge_policy policy = {.driver_ready=true,.supported_role=true};
static struct mtk_ap_bridge_gate *gate;
static unsigned int checks;
#define CHECK(c) do { checks++; if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);abort();} }while(0)
static void packet(struct sk_buff*s,u16 proto)
{memset(s,0,sizeof(*s));s->len=ETH_HLEN;((struct ethhdr*)s->bytes)->h_proto=htons(proto);}
static void packet_tag(struct sk_buff*s,u16 proto,u16 vid,u16 inner)
{packet(s,proto);s->len+=4;struct vlan_hdr*v=(void*)(s->bytes+14);v->h_vlan_TCI=htons(vid);v->h_vlan_encapsulated_proto=htons(inner);}
static void skb_release(struct sk_buff*s)
{if(s->ext&&!--s->ext->refs)free(s->ext);s->ext=NULL;}
static struct mtk_ap_bridge_request request(unsigned op)
{struct mtk_ap_bridge_request r={.abi=1,.size=sizeof(r),.op=op,.ifindex=base.ifindex,.owner=gate->owner,.transaction=gate->transaction};return r;}
static void prepare(void)
{struct mtk_ap_bridge_request r=request(MTK_AP_BRIDGE_PREPARE);r.transaction=0;CHECK(mtk_ap_bridge_ioctl(gate,&r,&policy)==0);CHECK(r.blocked&&r.transaction);}
static void map(u8*b,unsigned vid){b[vid/8]|=BIT(vid%8);}
static struct mtk_ap_bridge_domain standalone(void)
{struct mtk_ap_bridge_domain d={.port_ifindex=1};return d;}
static struct mtk_ap_bridge_domain bridged(void)
{struct mtk_ap_bridge_domain d={.port_ifindex=1,.bridge_ifindex=2,.bridge_vlan_proto=ETH_P_8021Q,.pvid=1,.bridge_pvid=1,.port_flags=bp.flags};map(d.vlans,1);map(d.untagged,1);map(d.bridge_vlans,1);map(d.bridge_untagged,1);return d;}
static int stage(unsigned index,unsigned count,struct mtk_ap_bridge_domain d)
{struct mtk_ap_bridge_request r=request(MTK_AP_BRIDGE_STAGE_DOMAIN);r.domain_index=index;r.domain_count=count;r.domain=d;return mtk_ap_bridge_ioctl(gate,&r,&policy);}
static int commit(void)
{struct mtk_ap_bridge_request r=request(MTK_AP_BRIDGE_COMMIT);return mtk_ap_bridge_ioctl(gate,&r,&policy);}
static void initialize(void)
{base.next=&bridge;ns.devices=&base;vlan.proto=htons(ETH_P_8021Q);br.vlan_proto=htons(ETH_P_8021Q);
 INIT_LIST_HEAD(&pvg.vlan_list);INIT_LIST_HEAD(&bvg.vlan_list);pvg.pvid=bvg.pvid=1;
 pv1.vid=bv1.vid=1;pv1.flags=bv1.flags=BRIDGE_VLAN_INFO_UNTAGGED;
 list_add_tail(&pv1.vlist,&pvg.vlan_list);list_add_tail(&bv1.vlist,&bvg.vlan_list);
 bp.flags=BIT(0)|BIT(5)|BIT(6)|BIT(11)|BIT(14);
 gate=mtk_ap_bridge_create(&base);CHECK(!IS_ERR(gate));CHECK(base.netns_local);}
static void activation(void)
{struct mtk_ap_bridge_reader r;struct mtk_ap_bridge_request q;
 CHECK(mtk_ap_bridge_is_blocked(gate));CHECK(!mtk_ap_bridge_enter(gate,&r));
 q=request(MTK_AP_BRIDGE_GET);q.owner=0;CHECK(!mtk_ap_bridge_ioctl(gate,&q,&policy));CHECK(q.owner&&q.blocked);
 capable=false;CHECK(mtk_ap_bridge_ioctl(gate,&q,&policy)==-EPERM);capable=true;
 q.abi=99;CHECK(mtk_ap_bridge_ioctl(gate,&q,&policy)==-EOPNOTSUPP);
 prepare();CHECK(!stage(0,1,standalone()));CHECK(!commit());CHECK(!mtk_ap_bridge_is_blocked(gate));
 CHECK(mtk_ap_bridge_enter(gate,&r));mtk_ap_bridge_exit(&r);
 prepare();CHECK(commit()==-EINVAL);CHECK(mtk_ap_bridge_is_blocked(gate));
 alloc_fail=1;CHECK(stage(0,1,standalone())==-ENOMEM);CHECK(mtk_ap_bridge_is_blocked(gate));
 CHECK(!stage(0,1,standalone()));copy_fail=1;CHECK(commit()==-EFAULT);CHECK(mtk_ap_bridge_is_blocked(gate));CHECK(!commit());
 prepare();CHECK(!stage(0,1,standalone()));profile_ready=false;CHECK(commit()==-EAGAIN);profile_ready=true;CHECK(!commit());
}
static void queue_leases(void)
{struct mtk_ap_bridge_reader old,now;struct sk_buff a,b,c,rx;u64 lease;
 packet(&a,0x0800);packet(&rx,0x0800);CHECK(mtk_ap_bridge_enter(gate,&old));
 CHECK(mtk_ap_bridge_stamp(&rx,&old,true));CHECK(!((struct mtk_ap_bridge_skb_ext*)skb_ext_find(&rx,0))->tx_owner);
 CHECK(mtk_ap_bridge_tx_domain_stamp(&a,&old));lease=old.lease;CHECK(mtk_ap_bridge_tx_domain_validate(&a,&old));
 b=a;a.ext->refs++;CHECK(mtk_ap_bridge_stamp(&b,&old,true));CHECK(a.ext!=b.ext);CHECK(!((struct mtk_ap_bridge_skb_ext*)a.ext->data)->rx_owner);
 packet(&c,0x0800);CHECK(mtk_ap_bridge_inherit(&c,&b));CHECK(mtk_ap_bridge_tx_domain_validate(&c,&old));mtk_ap_bridge_exit(&old);
 prepare();CHECK(!stage(0,1,standalone()));CHECK(!commit());CHECK(mtk_ap_bridge_enter(gate,&now));CHECK(now.lease!=lease);
 CHECK(!mtk_ap_bridge_tx_domain_stamp(&a,&now));CHECK(!mtk_ap_bridge_validate(&rx,&now,true));
 ext_fail=1;struct sk_buff oom;packet(&oom,0x0800);CHECK(!mtk_ap_bridge_stamp(&oom,&now,true));mtk_ap_bridge_exit(&now);
 skb_release(&a);skb_release(&b);skb_release(&c);skb_release(&rx);
}
static void bridge_policy(void)
{struct mtk_ap_bridge_info info;struct sk_buff skb;struct mtk_ap_bridge_reader r;
 prepare();base.master=&bridge;base.brport=&bp;base.rx_handler=&bp;CHECK(!stage(0,1,bridged()));
 pvextra.vid=99;list_add_tail(&pvextra.vlist,&pvg.vlan_list);CHECK(commit()==-EAGAIN);
 pvextra.vlist.prev->next=pvextra.vlist.next;pvextra.vlist.next->prev=pvextra.vlist.prev;CHECK(!commit());
 packet(&skb,0x0800);CHECK(!mtk_ap_bridge_resolve_rcu(gate,&base,&skb,&info));CHECK(info.bridged&&info.port==&base);
 CHECK(mtk_ap_bridge_enter(gate,&r));CHECK(mtk_ap_bridge_tx_domain_stamp(&skb,&r));
 base.brport=NULL;base.master=NULL;base.rx_handler=NULL;
 CHECK(mtk_ap_bridge_resolve_rcu(gate,&base,&skb,&info)==-ESTALE);CHECK(!mtk_ap_bridge_tx_domain_validate(&skb,&r));
 base.brport=&bp;base.master=&bridge;base.rx_handler=&bp;bp.flags^=BIT(0);
 CHECK(mtk_ap_bridge_resolve_rcu(gate,&base,&skb,&info)==-ESTALE);bp.flags^=BIT(0);
 mtk_ap_bridge_exit(&r);skb_release(&skb);
}
static void vlan_domains(void)
{struct mtk_ap_bridge_domain d;struct mtk_ap_bridge_info info;struct mtk_ap_bridge_reader r;
 struct sk_buff skb;prepare();base.brport=NULL;base.master=NULL;base.rx_handler=NULL;
 bridge.next=&vlan;vlan.master=&bridge;vlan.brport=&bp;vlan.rx_handler=&bp;
 CHECK(!stage(0,1,standalone()));CHECK(commit()==-EAGAIN); /* omitted child */
 prepare();CHECK(!stage(0,2,standalone()));d=bridged();d.port_ifindex=3;d.depth=1;d.tag[0]=(struct mtk_ap_bridge_tag){3,ETH_P_8021Q,100};CHECK(!stage(1,2,d));CHECK(!commit());
 packet_tag(&skb,ETH_P_8021Q,100,0x0800);CHECK(!mtk_ap_bridge_resolve_rcu(gate,&base,&skb,&info));CHECK(info.port==&vlan&&info.bridged);
 CHECK(mtk_ap_bridge_enter(gate,&r));CHECK(mtk_ap_bridge_tx_domain_stamp(&skb,&r));
 /* Vendor VLAN rewrite must not replace the committed TX domain. */
 ((struct ethhdr*)skb.bytes)->h_proto=htons(0x0800);skb.len=14;CHECK(mtk_ap_bridge_tx_domain_validate(&skb,&r));
 vlan.master=NULL;vlan.brport=NULL;vlan.rx_handler=NULL;CHECK(!mtk_ap_bridge_tx_domain_validate(&skb,&r));
 mtk_ap_bridge_exit(&r);skb_release(&skb);
 packet_tag(&skb,ETH_P_8021Q,777,0x0800);CHECK(mtk_ap_bridge_resolve_rcu(gate,&base,&skb,&info)==-EXDEV);
 struct mtk_ap_bridge_tag tags[2];skb.len=15;CHECK(mtk_ap_tags(&skb,tags)==-EINVAL);
 packet_tag(&skb,ETH_P_8021Q,0,0x0800);CHECK(mtk_ap_tags(&skb,tags)==1&&tags[0].vid==0);
 bridge.next=NULL;
}
static void stacked_tags(void)
{struct net_device inner={.ifindex=4,.reg_state=1,.running=true,.refs=1,.net=&ns,.netns_local=true,.is_vlan=true,.parent=&vlan,.proto=htons(ETH_P_8021AD),.vid=200,.master=&bridge,.brport=&bp,.rx_handler=&bp};
 struct mtk_ap_bridge_domain d;struct mtk_ap_bridge_info info;struct sk_buff skb;
 prepare();bridge.next=&vlan;vlan.next=&inner;vlan.master=NULL;vlan.brport=NULL;vlan.rx_handler=NULL;
 CHECK(!stage(0,3,standalone()));d=standalone();d.port_ifindex=3;d.depth=1;d.tag[0]=(struct mtk_ap_bridge_tag){3,ETH_P_8021Q,100};CHECK(!stage(1,3,d));
 d=bridged();d.port_ifindex=4;d.depth=2;d.tag[0]=(struct mtk_ap_bridge_tag){3,ETH_P_8021Q,100};d.tag[1]=(struct mtk_ap_bridge_tag){4,ETH_P_8021AD,200};CHECK(!stage(2,3,d));CHECK(!commit());
 packet_tag(&skb,ETH_P_8021AD,200,0x0800);skb.tagged=true;skb.vlan_proto=htons(ETH_P_8021Q);skb.vid=100;
 CHECK(!mtk_ap_bridge_resolve_rcu(gate,&base,&skb,&info));CHECK(info.port==&inner);
 packet_tag(&skb,ETH_P_8021Q,100,ETH_P_8021AD);struct vlan_hdr*second=(void*)(skb.bytes+18);second->h_vlan_TCI=htons(200);second->h_vlan_encapsulated_proto=htons(0x0800);skb.len+=4;
 CHECK(!mtk_ap_bridge_resolve_rcu(gate,&base,&skb,&info));CHECK(info.port==&inner);
 skb.tagged=true;skb.vlan_proto=htons(ETH_P_8021Q);skb.vid=0;CHECK(mtk_ap_bridge_resolve_rcu(gate,&base,&skb,&info)==-E2BIG);
 packet_tag(&skb,ETH_P_8021Q,100,0x0800);skb.tagged=true;skb.vlan_proto=htons(ETH_P_8021Q);skb.vid=0;
 CHECK(!mtk_ap_bridge_resolve_rcu(gate,&base,&skb,&info));CHECK(info.port==&vlan&&!info.bridged);
 skb.vlan_proto=htons(0x0800);CHECK(mtk_ap_bridge_resolve_rcu(gate,&base,&skb,&info)==-EINVAL);
 vlan.running=false;skb.tagged=false;CHECK(mtk_ap_bridge_resolve_rcu(gate,&base,&skb,&info)==-ESTALE);vlan.running=true;
 /* Drop the committed snapshot before stack-owned mock device disappears. */
 prepare();bridge.next=NULL;vlan.next=NULL;CHECK(!stage(0,1,standalone()));CHECK(!commit());
 CHECK(inner.refs==1);
}
static void *blocking(void*p){mtk_ap_bridge_block(p);return NULL;}
static void revoke_during_copy(void){mtk_ap_bridge_block(gate);}
static void drains_and_publication(void)
{struct mtk_ap_bridge_reader r;pthread_t t;prepare();CHECK(!stage(0,1,standalone()));CHECK(!commit());
 CHECK(mtk_ap_bridge_enter(gate,&r));CHECK(!pthread_create(&t,NULL,blocking,gate));usleep(10000);CHECK(!gate->open);CHECK(atomic_read(&gate->readers)==1);mtk_ap_bridge_exit(&r);CHECK(!pthread_join(t,NULL));CHECK(!gate->transaction);
 prepare();CHECK(!stage(0,1,standalone()));copy_hook=revoke_during_copy;CHECK(commit()==-EAGAIN);CHECK(mtk_ap_bridge_is_blocked(gate));
 prepare();CHECK(!stage(0,1,standalone()));CHECK(!commit());u64 lease=gate->lease,txn=gate->transaction;
 profile_ready=false;mtk_ap_bridge_profile_drain(gate);CHECK(mtk_ap_bridge_is_blocked(gate));CHECK(!mtk_ap_bridge_config_blocked(gate));CHECK(gate->open&&gate->lease==lease&&gate->transaction==txn);CHECK(!mtk_ap_bridge_enter(gate,&r));profile_ready=true;CHECK(mtk_ap_bridge_enter(gate,&r));mtk_ap_bridge_exit(&r);
}
int main(void)
{initialize();activation();queue_leases();bridge_policy();vlan_domains();stacked_tags();drains_and_publication();
 mtk_ap_bridge_destroy(gate);CHECK(ns.refs==1&&base.refs==1&&bridge.refs==1&&vlan.refs==1);
 printf("%u actual-helper checks passed (host adapters, ASan/UBSan)\n",checks);return 0;}
