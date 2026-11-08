/* Runs the actual netifd activation implementation against an ioctl peer.
 * No kernel/network interfaces are opened or changed. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#define __NETIFD_H
#define __NETIFD_DEVICE_H
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define DEV_OPT_MULTICAST_TO_UNICAST 1
#define DEV_OPT_ISOLATE 2
#define DEV_OPT_LEARNING 4
#define DEV_OPT_UNICAST_FLOOD 8
struct uloop_timeout { void (*cb)(struct uloop_timeout *); bool pending; };
static void uloop_timeout_cancel(struct uloop_timeout *t) { t->pending = false; }
static void uloop_timeout_set(struct uloop_timeout *t, int ms) { t->pending = true; assert(ms > 0); }
struct ap_bridge_state;
struct device_wireless_policy { bool wireless, ap, forwarding, proxyarp, isolate, bridge_isolate, multicast_set, multicast; };
struct device {
	char ifname[IFNAMSIZ];
	unsigned index;
	struct device *wireless_parent;
	struct ap_bridge_state *ap_bridge;
	struct device_wireless_policy wireless_local;
	bool present;
	bool wireless, wireless_ap, wireless_bridge_forwarding, wireless_bridge_isolate;
	bool wireless_proxyarp, wireless_isolate, bpdu_filter;
	bool wireless_multicast_set, wireless_multicast;
	uint16_t wireless_vlan_proto, wireless_vlan_id;
	uint32_t wireless_policy;
	struct { uint64_t flags; bool multicast_to_unicast, isolate, learning, unicast_flood; } settings;
};
static struct device *devices[40];
static unsigned ndev, stages, commits, prepares, gets;
static bool ready, inaccessible, ambiguous_commit, ambiguous_prepare, reject_stage, missing_mode;
static uint64_t transaction, lease;
#include "mtk_ap_bridge.h"
uint64_t ap_bridge_port_flags(struct device *, struct device *);
int ap_bridge_commit(struct device *);
int ap_bridge_prepare(struct device *);
struct device *ap_bridge_owner(struct device *);
void device_foreach(void (*cb)(struct device *, void *), void *arg)
{
	for (unsigned i = 0; i < ndev; i++) cb(devices[i], arg);
}
int bridge_ap_expected(struct device *d, struct mtk_ap_bridge_domain *out)
{
	out->bridge_ifindex = 100;
	out->port_flags = ap_bridge_port_flags(d, d);
	return 0;
}
static unsigned mock_index(const char *s)
{
	for (unsigned i = 0; i < ndev; i++)
		if (!strcmp(s, devices[i]->ifname)) return devices[i]->index;
	return 0;
}
static int mock_open(const char *p, int flags, ...)
{ (void)p; (void)flags; if (missing_mode) { errno = ENOENT; return -1; } return 71; }
static ssize_t mock_read(int fd, void *buf, size_t n)
{ (void)fd; assert(n >= 9); memcpy(buf, "cpu-only\n", 9); return 9; }
static int mock_close(int fd) { (void)fd; return 0; }
static int mock_socket(int domain, int type, int protocol)
{ (void)domain; (void)type; (void)protocol; return 72; }
static int mock_ioctl(int fd, unsigned long cmd, ...)
{
	va_list ap;
	struct ifreq *ifr;
	struct mtk_ap_bridge_request *r;
	(void)fd;
	assert(cmd == MTK_AP_BRIDGE_IOCTL);
	va_start(ap, cmd); ifr = va_arg(ap, struct ifreq *); va_end(ap);
	r = (void *)ifr->ifr_data;
	assert(r->abi == 1 && r->size == sizeof(*r));
	if (inaccessible) { errno = EIO; return -1; }
	if (r->op == MTK_AP_BRIDGE_GET) gets++;
	if (r->op == MTK_AP_BRIDGE_PREPARE) {
		prepares++; ready = false; transaction++; stages = 0;
		if (ambiguous_prepare) { ambiguous_prepare = false; errno = EFAULT; return -1; }
	}
	if (r->op == MTK_AP_BRIDGE_STAGE_DOMAIN) {
		assert(!ready && r->transaction == transaction);
		if (reject_stage) { errno = EINVAL; return -1; }
		assert(r->domain_index == stages++);
		assert(r->domain.port_ifindex);
	}
	if (r->op == MTK_AP_BRIDGE_COMMIT) {
		assert(stages == r->domain_count && stages && !ready);
		commits++; ready = true; lease++;
		if (ambiguous_commit) { ambiguous_commit = false; errno = EFAULT; return -1; }
	}
	r->owner = 42;
	r->transaction = transaction;
	r->lease = ready ? lease : 0;
	r->state = ready ? MTK_AP_BRIDGE_READY : MTK_AP_BRIDGE_BLOCKED;
	r->blocked = !ready;
	r->driver_ready = r->profile_ready = 1;
	r->error = 0;
	return 0;
}
#define if_nametoindex mock_index
#define open mock_open
#define read mock_read
#define close mock_close
#define socket mock_socket
#define ioctl mock_ioctl
#include MTK_NETIFD_AP_SOURCE

enum { DEV_EVENT_WIRELESS_POLICY = 1 };
static unsigned policy_events;
static void device_broadcast_event(struct device *dev, unsigned ev)
{ (void)dev; assert(ev == DEV_EVENT_WIRELESS_POLICY); policy_events++; }
#ifdef DEVICE_POLICY_SOURCE
#include DEVICE_POLICY_SOURCE
#endif

static struct device pool[34];
static void reset(unsigned children)
{
	for (unsigned i = 0; i < ndev; i++) ap_bridge_free(devices[i]);
	memset(pool, 0, sizeof(pool));
	ndev = children + 1;
	for (unsigned i = 0; i < ndev; i++) {
		devices[i] = &pool[i];
		snprintf(pool[i].ifname, IFNAMSIZ, "test%u", i);
		pool[i].index = i + 1;
		pool[i].present = true;
		pool[i].wireless_ap = pool[i].wireless_bridge_forwarding = true;
		if (i) {
			pool[i].wireless_parent = &pool[0];
			pool[i].wireless_vlan_proto = 0x8100;
			pool[i].wireless_vlan_id = i;
		}
	}
	ready = false; inaccessible = ambiguous_commit = ambiguous_prepare = reject_stage = false;
	stages = commits = prepares = gets = 0;
}
int main(void)
{
	reset(1);
	assert(ap_bridge_commit(&pool[0]) == -EACCES);
	assert(!ap_bridge_prepare_new(&pool[0]));
	assert(!ready && prepares == 1);
	assert(!ap_bridge_prepare(&pool[1]) && prepares == 1); /* claimant reuses owner */
	assert(!ap_bridge_commit(&pool[0]) && ready && stages == 2);
	assert(!ap_bridge_prepare_new(&pool[0]) && !ready);
	ambiguous_commit = true;
	assert(ap_bridge_commit(&pool[0]) == -EFAULT);
	assert(!ready && prepares == 3); /* successful new blocked ACK after ambiguity */
	reset(0);
	ambiguous_prepare = true;
	assert(!ap_bridge_prepare_new(&pool[0]) && !ready && prepares == 2);
	reset(0);
	assert(!ap_bridge_prepare_new(&pool[0]));
	reject_stage = true;
	assert(ap_bridge_commit(&pool[0]) == -EINVAL && !ready && !commits);
	reset(0);
	ready = true; inaccessible = true;
	assert(ap_bridge_prepare_new(&pool[0]) == -EIO && ready);
	assert(!pool[0].ap_bridge->prepared); /* syscall failure is NOT blocked proof */
	reset(32);
	assert(!ap_bridge_prepare_new(&pool[0]));
	assert(ap_bridge_commit(&pool[0]) == -E2BIG && !commits && !ready);
	/* The actual config batch closes a previously committed owner only once;
	 * child/member reconciliation cannot publish before the whole batch ends. */
	reset(1);
	assert(!ap_bridge_prepare_new(&pool[0]) && !ap_bridge_commit(&pool[0]));
	unsigned old_commits = commits;
	uint64_t old_lease = lease;
	ap_bridge_config_begin();
	assert(!ap_bridge_prepare(&pool[1]) && !ready);
	ap_bridge_reconcile(&pool[0]);
	assert(!pool[0].ap_bridge->retry.pending && commits == old_commits);
	assert(!ap_bridge_config_end() && pool[0].ap_bridge->retry.pending);
	pool[0].ap_bridge->retry.cb(&pool[0].ap_bridge->retry);
	assert(ready && commits == old_commits + 1 && lease > old_lease && stages == 2);
	/* A later fault blocks the whole touched batch, including a queued retry.
	 * An unrelated event must not clear this failure and restore old policy. */
	ap_bridge_config_begin();
	assert(!ap_bridge_prepare(&pool[0]));
	ap_bridge_fault(&pool[1], -EIO);
	assert(ap_bridge_config_end() == -EIO && !ready);
	old_commits = commits;
	ap_bridge_reconcile(&pool[1]);
	ap_retry(&pool[0].ap_bridge->retry);
	assert(!ready && commits == old_commits && pool[0].ap_bridge->error == -EIO);
	ap_bridge_config_begin();
	assert(!ap_bridge_prepare(&pool[1]));
	assert(!ap_bridge_config_end());
	ap_retry(&pool[0].ap_bridge->retry);
	assert(ready && commits == old_commits + 1);
	/* Other products without the profile file retain legacy reload results. */
	missing_mode = true;
	unsigned unchanged_prepares = prepares;
	ap_bridge_config_begin();
	assert(!ap_bridge_prepare(&pool[0]) && prepares == unchanged_prepares);
	ap_bridge_config_error(-EIO);
	assert(!ap_bridge_config_end());
	assert(!ap_reload_active && !ap_reload_error);
	missing_mode = false;
	/* Controlled wireless activation is never borrowed by a network reload. */
	assert(!ap_bridge_prepare_new(&pool[0]));
	ap_bridge_config_begin();
	assert(!ap_bridge_prepare(&pool[1]) && !ap_bridge_config_end());
	ap_retry(&pool[0].ap_bridge->retry);
	assert(!ready && commits == old_commits + 1 && pool[0].ap_bridge->controlled);
	assert(!ap_bridge_commit(&pool[0]));

	reset(0);
	pool[0].wireless_isolate = true;
	pool[0].wireless_proxyarp = true;
	uint64_t f = ap_bridge_port_flags(&pool[0], &pool[0]);
	assert(!(f & 1) && (f & (1ULL << 10)));
	pool[0].wireless_isolate = pool[0].wireless_proxyarp = false;
	f = ap_bridge_port_flags(&pool[0], &pool[0]);
	assert((f & 1) && !(f & ((1ULL << 10) | (1ULL << 16))));

#ifdef DEVICE_POLICY_SOURCE
	reset(1);
	pool[0].wireless_local = (struct device_wireless_policy){
		.wireless = true, .ap = true, .forwarding = true,
		.isolate = true, .proxyarp = true, .bridge_isolate = true,
	};
	device_wireless_refresh(&pool[0], NULL);
	device_wireless_refresh(&pool[1], &pool[0]);
	assert(pool[1].wireless_bridge_forwarding && pool[1].wireless_isolate && pool[1].bpdu_filter);
	memset(&pool[0].wireless_local, 0, sizeof(pool[0].wireless_local));
	device_wireless_refresh(&pool[0], NULL);
	device_wireless_refresh(&pool[1], &pool[0]);
	assert(!pool[1].wireless_bridge_forwarding && !pool[1].wireless_isolate &&
	       !pool[1].wireless_proxyarp && !pool[1].wireless_bridge_isolate && !pool[1].bpdu_filter);
	pool[1].wireless_local.isolate = true;
	device_wireless_refresh(&pool[1], &pool[0]);
	assert(pool[1].wireless_isolate); /* local explicit setting survives parent false */
	device_wireless_refresh(&pool[1], NULL);
	assert(!pool[1].wireless_parent && pool[1].wireless_isolate && policy_events);
#endif
	reset(0);
	ap_bridge_free(&pool[0]);
	puts("activation: real helper scenarios passed");
}
