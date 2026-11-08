/* Production generic destruction/replacement and bridge preflight/tombstone
 * paths. System, user-list and AVL calls are deterministic local peers. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define D(...) ((void)0)
struct device;
struct blob_attr {int value;};
enum dev_change_type {DEV_CONFIG_NO_CHANGE,DEV_CONFIG_APPLIED,DEV_CONFIG_RESTART,DEV_CONFIG_RECREATE,DEV_CONFIG_FAILED};
struct device_type {int (*prepare_change)(struct device *,bool);void (*free)(struct device *);
 struct device *(*create)(const char *,struct device_type *,struct blob_attr *);
 void (*config_init)(struct device *);};
struct list_head {struct list_head *next,*prev;};
struct safe_list {struct list_head list;};
struct avl_node {bool linked;const void *key;};
struct vlist_node {struct avl_node avl;int version;};
struct device {
	char ifname[16];
	int active;
	struct avl_node avl;
	bool current_config,config_pending;
 struct device_type *type;
 void *auth_vlans,*config,*tags,*config_auth_vlans,*extra_vlan;
 bool present,fail_prepare;
 int (*set_state)(struct device *,bool);
 struct safe_list users;
};
struct device_user {struct device *dev;struct safe_list list;};
struct bridge_member;
struct vlist_tree {struct bridge_member *head;int version,avl;};
struct bridge_state {struct device dev;struct vlist_tree members;};
struct bridge_member {struct vlist_node node;struct bridge_state *bst;
 struct device_user dev;bool active,present,retired;struct bridge_member *next;};
struct bridge_vlan {int unused;};
#define vlist_for_each_element(t,p,m) for((p)=(t)->head;(p);(p)=(p)->next)
#define list_empty(h) ((h)->next==(h))
#define list_first_entry(h,t,m) container_of((h)->next,t,m)
static int __devlock,freed,cleaned,moved,present_events,releases,ap_frees,detaches,config_changes,member_frees;
static bool fail_detach;
static bool create_fails,config_init;
static int devices,created_count,checked;
static struct device *existing,*created;
static int ap_bridge_prepare(struct device *d) {return d->fail_prepare?-EIO:0;}
static void ap_bridge_free(struct device *d) {(void)d;ap_frees++;}
static void device_cleanup(struct device *d) {(void)d;cleaned++;}
static void device_release(struct device_user *d) {(void)d;releases++;}
static void device_set_present(struct device *d,bool p) {d->present=p;present_events++;}
static void safe_list_del(struct safe_list *l) {l->list.prev->next=l->list.next;l->list.next->prev=l->list.prev;}
static void __device_add_user(struct device_user *u,struct device *d)
{u->dev=d;u->list.list.next=d->users.list.next;u->list.list.prev=&d->users.list;d->users.list.next->prev=&u->list.list;d->users.list.next=&u->list.list;moved++;}
static int bridge_disable_member(struct bridge_member *bm,bool keep)
{(void)keep;if(!bm->active)return 0;if(fail_detach)return -EIO;bm->active=false;detaches++;return 0;}
static int bridge_remove_member(struct bridge_member *bm)
{int r=bridge_disable_member(bm,false);if(!r)bm->present=false;return r;}
static int avl_insert(int *tree,struct avl_node *node)
{(void)tree;assert(!node->linked);node->linked=true;return 0;}
static void avl_delete(int *tree,struct avl_node *node) {(void)tree;assert(node->linked);node->linked=false;}
static struct device *device_find(const char *name)
{(void)name;return existing && existing->avl.linked?existing:NULL;}
static void device_check_state(struct device *d) {(void)d;checked++;}
static struct device *create_peer(const char *name,struct device_type *type,struct blob_attr *attr)
{(void)attr;created_count++;if(create_fails)return NULL;created->type=type;strcpy(created->ifname,name);
created->avl.key=created->ifname;created->avl.linked=true;existing=created;return created;}
static void *blob_memdup(void *p) {(void)p;return malloc(8);}
static enum dev_change_type device_set_config(struct device *d,struct device_type *t,struct blob_attr *c)
{(void)d;(void)t;(void)c;config_changes++;return DEV_CONFIG_RECREATE;}
#include NETIFD_BRIDGE_PREPARE_SOURCE
#include NETIFD_GENERIC_FREE_SOURCE
#include NETIFD_REPLACE_SOURCE
#include NETIFD_APPLY_SOURCE
#include NETIFD_MEMBER_FAILURE_SOURCE
#include NETIFD_DELETE_SOURCE
#include NETIFD_CREATE_SOURCE
static void destroy_peer(struct device *d)
{struct bridge_state *b=container_of(d,struct bridge_state,dev);for(struct bridge_member *p=b->members.head;p;p=p->next)assert(!p->active);freed++;}
static void init(struct device *d,struct device_type *t)
{memset(d,0,sizeof(*d));d->type=t;d->config=malloc(16);memcpy(d->config,"config retained",16);d->users.list.next=d->users.list.prev=&d->users.list;}
int main(void)
{
 struct device_type type={.prepare_change=bridge_prepare_change,.free=destroy_peer,.create=create_peer};
 struct bridge_state old={0},replacement={0};struct device a={0},b={0};
 struct bridge_member m={.bst=&old,.dev={.dev=&a},.active=true,.present=true},
                      n={.bst=&old,.dev={.dev=&b},.active=true,.present=true};
 struct device_user user={0};
 init(&old.dev,&type);init(&replacement.dev,&type);old.dev.present=true;
 m.next=&n;old.members.head=&m;old.members.version=3;
 __device_add_user(&user,&old.dev);moved=0;
 b.fail_prepare=true;
 assert(device_free(&old.dev)==-EIO);
 assert(!freed && !cleaned && !ap_frees && !detaches && !present_events && !releases);
 assert(!strcmp(old.dev.config,"config retained") && user.dev==&old.dev);
 assert(device_replace(&replacement.dev,&old.dev)==-EIO && !moved && !releases);
 assert(device_apply_config(&old.dev,&type,NULL)==DEV_CONFIG_FAILED && !config_changes);
 assert(m.active && n.active);
 b.fail_prepare=false;fail_detach=true;
 /* vlist_delete has removed the node immediately before its callback. */
 m.node.avl.linked=false;
 bridge_free_member_failure(&m);
 assert(m.retired && m.node.avl.linked && m.node.version==2 && !member_frees);
 assert(device_free(&old.dev)==-EIO && !freed && !cleaned && user.dev==&old.dev);
 fail_detach=false;
 assert(!device_replace(&replacement.dev,&old.dev));
 assert(detaches==2 && cleaned==1 && freed==1 && ap_frees==1 && moved==1 && releases==1);
 assert(user.dev==&replacement.dev && !m.active && !n.active);
 free(replacement.dev.config);
 /* Real device_create also prepares BEFORE deleting the old AVL entry and
  * restores it if construction fails. A later reload can genuinely retry. */
 {
  struct bridge_state o={0},n={0};struct device p={0};struct device_user u={0};
  struct bridge_member member={.bst=&o,.dev={.dev=&p},.active=true,.present=true};
  init(&o.dev,&type);init(&n.dev,&type);free(n.dev.config);n.dev.config=NULL;
  strcpy(o.dev.ifname,"br-test");o.dev.avl.key=o.dev.ifname;o.dev.avl.linked=true;
  o.members.head=&member;existing=&o.dev;created=&n.dev;__device_add_user(&u,&o.dev);
  p.fail_prepare=true;int previous_free=freed;
  assert(device_create("br-test",&type,NULL)==NULL && !created_count);
  assert(o.dev.avl.linked && u.dev==&o.dev && !strcmp(o.dev.config,"config retained"));
  p.fail_prepare=false;create_fails=true;
  assert(device_create("br-test",&type,NULL)==NULL && created_count==1);
  assert(o.dev.avl.linked && u.dev==&o.dev && freed==previous_free);
  create_fails=false;
  assert(device_create("br-test",&type,NULL)==&n.dev && created_count==2);
  assert(u.dev==&n.dev && freed==previous_free+1 && checked==1);
  free(n.dev.config);
 }
 puts("netifd: actual generic free/replace/config preflight and failed-member retry retention passed");
 return 0;
}
