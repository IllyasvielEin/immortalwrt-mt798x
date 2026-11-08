/* Full native config_init_all -> real UCI/device/bridge/ap-bridge reload.
 * Only external system/ubus/driver peers are sandboxed. No host networking. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "netifd.h"
#include "device.h"
#include "config.h"
#include "ucode.h"
#include "mtk_ap_bridge.h"

struct peer {
 char name[IFNAMSIZ]; unsigned index, prepares, commits, staged, domains;
 uint64_t transaction, lease; bool ready;
 struct mtk_ap_bridge_domain domain[MTK_AP_BRIDGE_MAX_DOMAINS];
};
static struct peer peers[32];
static unsigned npeer;
static bool in_reload, fail_detach, static_child;
static unsigned reload_commits;
static struct device *ap[3], *child;
static struct device_user wireless_claim[3];
static struct peer *lookup(const char *name)
{
 for(unsigned i=0;i<npeer;i++)if(!strcmp(name,peers[i].name))return &peers[i];
 assert(npeer<ARRAY_SIZE(peers));struct peer *p=&peers[npeer++];
 snprintf(p->name,sizeof(p->name),"%s",name);p->index=npeer;return p;
}
unsigned __wrap_if_nametoindex(const char *name) { return lookup(name)->index; }
int __real_open(const char *, int, ...);
int __wrap_open(const char *name,int flags,...)
{
 if(!strcmp(name,"/usr/share/ax6000/forwarding-mode"))return 989;
 mode_t mode=0;if(flags&O_CREAT){va_list va;va_start(va,flags);mode=va_arg(va,int);va_end(va);}
 return __real_open(name,flags,mode);
}
ssize_t __real_read(int,void *,size_t);
ssize_t __wrap_read(int fd,void *b,size_t n)
{if(fd==989){assert(n>=9);memcpy(b,"cpu-only\n",9);return 9;}return __real_read(fd,b,n);}
int __real_close(int);
int __wrap_close(int fd){return fd==989||fd==990?0:__real_close(fd);}
int __wrap_socket(int domain,int type,int protocol)
{assert(domain==AF_INET && (type&SOCK_DGRAM) && !protocol);return 990;}
int __wrap_ioctl(int fd,unsigned long cmd,...)
{
 va_list va;va_start(va,cmd);struct ifreq *ifr=va_arg(va,struct ifreq *);va_end(va);
 assert(fd==990 && cmd==MTK_AP_BRIDGE_IOCTL);
 struct peer *p=lookup(ifr->ifr_name);struct mtk_ap_bridge_request *r=(void*)ifr->ifr_data;
 assert(r->ifindex==p->index && r->abi==MTK_AP_BRIDGE_ABI && r->size==sizeof(*r));
 if(r->op==MTK_AP_BRIDGE_PREPARE){p->ready=false;p->transaction++;p->prepares++;p->staged=0;}
 else if(r->op==MTK_AP_BRIDGE_STAGE_DOMAIN){
  assert(!p->ready && r->transaction==p->transaction && r->domain_index==p->staged);
  p->domain[p->staged++]=r->domain;p->domains=r->domain_count;
 }else if(r->op==MTK_AP_BRIDGE_COMMIT){
  assert(!in_reload); /* End of the entire config/member/VLAN batch is mandatory. */
  assert(!p->ready && p->staged==r->domain_count && p->staged && r->transaction==p->transaction);
  p->ready=true;p->lease++;p->commits++;
 }else assert(r->op==MTK_AP_BRIDGE_GET);
 r->owner=p->index+100;r->transaction=p->transaction;r->lease=p->ready?p->lease:0;
 r->state=p->ready?MTK_AP_BRIDGE_READY:MTK_AP_BRIDGE_BLOCKED;r->blocked=!p->ready;
 r->driver_ready=r->profile_ready=1;r->error=0;return 0;
}
void __wrap_netifd_ubus_get_procd_data(const char *type,void *cb){(void)type;(void)cb;}
void __wrap_netifd_ubus_device_notify(const char *e,struct blob_attr *b,int t){(void)e;(void)b;(void)t;}
void __wrap_netifd_ucode_config_load(bool start)
{
 unsigned commits=0;for(unsigned i=0;i<npeer;i++)commits+=peers[i].commits;
 assert(in_reload && commits==reload_commits);(void)start;
}
static void stop_loop(struct uloop_timeout *t){(void)t;uloop_end();}
static void drain(void)
{struct uloop_timeout t={.cb=stop_loop};uloop_timeout_set(&t,30);uloop_run();}
static int reload(void)
{
 reload_commits=0;for(unsigned i=0;i<npeer;i++)reload_commits+=peers[i].commits;
 in_reload=true;int ret=config_init_all();in_reload=false;return ret;
}
static void write_config(const char *path,int mtu,bool explicit_ap,bool only_unrelated)
{
 char file[512];snprintf(file,sizeof(file),"%s/network",path);FILE *f=fopen(file,"w");assert(f);
 if(!only_unrelated)fprintf(f,"config device\n option name 'br-test'\n option type 'bridge'\n option bridge_empty '1'\n");
 if(static_child)fprintf(f," list ports 'ap0.10'\n");
 fprintf(f,"config device\n option name 'unrelated'\n option mtu '%d'\n",mtu);
 if(explicit_ap)fprintf(f,"config device\n option name 'ap0'\n option mtu '%d'\n",mtu);
 assert(!fclose(f));
}
int __wrap_system_bridge_delif(struct device *bridge,struct device *dev)
{(void)bridge;return fail_detach && dev==child?-EIO:0;}
static void add_ap(struct device *bridge,unsigned i)
{
 char name[16];snprintf(name,sizeof(name),"ap%u",i);ap[i]=device_get(name,true);assert(ap[i]);
 ap[i]->external=2;ap[i]->wireless_local=(struct device_wireless_policy){.wireless=true,.ap=true,.forwarding=true};
 device_wireless_refresh(ap[i],NULL);
 device_add_user(&wireless_claim[i],ap[i]);assert(!device_claim(&wireless_claim[i]));
 assert(!bridge->hotplug_ops->add(bridge,ap[i],NULL));
 assert(!ap_bridge_prepare_new(ap[i]) && !ap_bridge_commit(ap[i]));
}
int main(int argc,char **argv)
{
 assert(argc==2);config_path=argv[1];assert(!uloop_init());
 write_config(argv[1],1500,false,false);assert(!reload());
 struct device *bridge=device_get("br-test",false);assert(bridge && bridge->hotplug_ops);
 struct device_user claim={0};device_add_user(&claim,bridge);assert(!device_claim(&claim));
 add_ap(bridge,0);
 /* Exact reported case: committed/up hotplug AP, no bridge VLANs, timer done. */
 drain();unsigned old=lookup("ap0")->commits;uint64_t lease=lookup("ap0")->lease;
 assert(lookup("ap0")->ready);assert(!reload());assert(!lookup("ap0")->ready);
 assert(lookup("ap0")->commits==old);drain();
 if(!lookup("ap0")->ready)fprintf(stderr,"P1-2 reproduced: no-change config_init_all left committed/up hotplug AP BLOCKED after timers\n");
 assert(lookup("ap0")->ready && lookup("ap0")->commits==old+1 && lookup("ap0")->lease>lease);
 /* Multiple BSS plus a real VLAN device/parent callback and complete snapshot. */
 add_ap(bridge,1);child=device_get("ap0.10",true);assert(child && child->wireless_parent==ap[0]);
 assert(!bridge->hotplug_ops->add(bridge,child,NULL));
 assert(!ap_bridge_prepare_new(ap[0]) && !ap_bridge_commit(ap[0]));drain();
 old=lookup("ap0")->commits;unsigned other=lookup("ap1")->commits;
 assert(!reload());assert(!lookup("ap0")->ready && !lookup("ap1")->ready);drain();
 assert(lookup("ap0")->commits==old+1 && lookup("ap1")->commits==other+1);
 assert(lookup("ap0")->domains==2 && lookup("ap0")->domain[1].depth==1);
 assert(lookup("ap0")->domain[1].tag[0].vid==10 && lookup("ap0")->domain[1].bridge_ifindex==lookup("br-test")->index);
 /* Explicit simple AP NO_CHANGE follows the same full reload completion. */
 write_config(argv[1],1500,true,false);assert(!reload());drain();
 old=lookup("ap0")->commits;assert(!reload());drain();assert(lookup("ap0")->commits==old+1);
 /* Use real bridge member-update/removal after PREPARE, not an AP API
  * substitute. Failed DELIF must retain its vlist owner and block the batch. */
 static_child=true;write_config(argv[1],1500,true,false);assert(!reload());drain();
 static_child=false;fail_detach=true;write_config(argv[1],1500,true,false);
 old=lookup("ap0")->commits;other=lookup("ap1")->commits;
 assert(reload()<0);drain();assert(!lookup("ap0")->ready && !lookup("ap1")->ready);
 ap_bridge_reconcile(ap[0]);ap_bridge_reconcile(ap[1]);drain();
 assert(lookup("ap0")->commits==old && lookup("ap1")->commits==other);
 fail_detach=false;assert(!reload());drain();
 assert(lookup("ap0")->ready && lookup("ap1")->ready && lookup("ap0")->commits==old+1);
 /* No AP/bridge config touched: unrelated device work does not PREPARE or
  * COMMIT an otherwise live standalone AP. */
 add_ap(bridge,2);assert(!bridge->hotplug_ops->del(bridge,ap[2],NULL));
 assert(!ap_bridge_prepare_new(ap[2]) && !ap_bridge_commit(ap[2]));drain();
 old=lookup("ap2")->commits;unsigned prepared=lookup("ap2")->prepares;
 write_config(argv[1],1400,true,false);assert(!reload());drain();
 assert(lookup("ap2")->ready && lookup("ap2")->commits==old && lookup("ap2")->prepares==prepared);
 /* A controlled wireless PREPARE is never stolen by network completion. */
 assert(!ap_bridge_prepare_new(ap[1]));old=lookup("ap1")->commits;
 assert(!reload());drain();assert(!lookup("ap1")->ready && lookup("ap1")->commits==old);
 assert(!ap_bridge_commit(ap[1]));
 puts("reload: actual UCI/config_init_all/device/bridge/VLAN/AP path passed");
 return 0;
}
