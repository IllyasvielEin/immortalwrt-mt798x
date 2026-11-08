/* Host type adapters only: authorization is extracted from production source. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "ap_bridge_parse.h"
#define A4_CONN 1
#define WDEV_TYPE_AP 1
#define SST_ASSOC 1
#define FC_TYPE_DATA 2
#define WPA_802_1X_PORT_SECURED 1
#define Ndis802_11PrivFilter8021xWEP 2
#define READ_ONCE(x) (x)
#define fRX_AMSDU 1
#define fRX_ARALINK 2
#define RX_BLK_TEST_FLAG(rx, flag) ((rx)->flags & (flag))
#define VALID_UCAST_ENTRY_WCID(ad, wcid) ((wcid) > 0 && (wcid) < 4)
#define IS_ENTRY_CLIENT(p) ((p)->client)
#define IS_ENTRY_A4(p) ((p)->a4)
#define RTPKT_TO_OSPKT(pkt) (pkt)
typedef unsigned char UCHAR;
struct wifi_dev { unsigned int wdev_type; };
typedef struct { unsigned int Type, ToDs, FrDs; } FRAME_CONTROL;
typedef struct { struct wifi_dev *wdev; unsigned int PortSecured; } STA_TR_ENTRY;
typedef struct { struct { STA_TR_ENTRY tr_entry[4]; } tr_ctl; } RTMP_ADAPTER;
typedef struct {
	unsigned int wcid, Sst, PrivacyFilter;
	struct wifi_dev *wdev;
	bool client, a4;
} MAC_TABLE_ENTRY;
struct mtk_ap_bridge_reader { bool gate; };
typedef struct {
	FRAME_CONTROL *FC;
	unsigned int flags;
	void *pRxPacket;
	struct mtk_ap_bridge_reader ap_bridge_reader;
} RX_BLK;
static unsigned int stamp_calls;
static bool stamp_result = true;
static bool mtk_ap_bridge_stamp(void *pkt, const struct mtk_ap_bridge_reader *r, bool rx)
{
	assert(pkt && rx);
	stamp_calls++;
	return r->gate && stamp_result;
}
#include "authorization.inc"

int main(void)
{
	RTMP_ADAPTER ad = { 0 };
	struct wifi_dev ap = { WDEV_TYPE_AP }, other = { WDEV_TYPE_AP };
	struct wifi_dev sta = { 2 };
	MAC_TABLE_ENTRY peer = { .wcid = 1, .Sst = SST_ASSOC, .wdev = &ap, .client = true };
	FRAME_CONTROL fc = { .Type = FC_TYPE_DATA, .ToDs = 1 };
	RX_BLK rx = { .FC = &fc, .pRxPacket = &ad, .ap_bridge_reader = { true } };
	UCHAR eth[18] = { [12] = 8 }, eap[18] = { [12] = 0x88, [13] = 0x8e, [14] = 2, [15] = 1 };
	UCHAR llc[12] = { 0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e, 2, 1, 0, 0 };
	unsigned int checks = 0, before;
#define CHECK(expr) do { assert(expr); checks++; } while (0)
#define AUTH(data, len, ethernet) mtk_ap_rx_authorize(&ad, &peer, &rx, &ap, data, len, ethernet)
	ad.tr_ctl.tr_entry[1].wdev = &ap;
	ad.tr_ctl.tr_entry[1].PortSecured = WPA_802_1X_PORT_SECURED;
	CHECK(AUTH(eth, sizeof(eth), true) == 0);
	CHECK(stamp_calls == 1);
	peer.wdev = &other; CHECK(AUTH(eth, sizeof(eth), true) == -1); peer.wdev = &ap;
	ad.tr_ctl.tr_entry[1].wdev = &other;
	CHECK(AUTH(eth, sizeof(eth), true) == -1); ad.tr_ctl.tr_entry[1].wdev = &ap;
	peer.wcid = 99; CHECK(AUTH(eth, sizeof(eth), true) == -1); peer.wcid = 1;
	peer.Sst = 0; CHECK(AUTH(eap, sizeof(eap), true) == -1); peer.Sst = SST_ASSOC;
	peer.client = false; CHECK(AUTH(eap, sizeof(eap), true) == -1); peer.client = true;
	peer.a4 = true; CHECK(AUTH(eap, sizeof(eap), true) == -1); peer.a4 = false;
	fc.FrDs = 1; CHECK(AUTH(eap, sizeof(eap), true) == -1); fc.FrDs = 0;
	fc.ToDs = 0; CHECK(AUTH(eap, sizeof(eap), true) == -1); fc.ToDs = 1;
	fc.Type = 0; CHECK(AUTH(eap, sizeof(eap), true) == -1); fc.Type = FC_TYPE_DATA;
	ad.tr_ctl.tr_entry[1].PortSecured = 0;
	before = stamp_calls;
	CHECK(AUTH(eth, sizeof(eth), true) == -1);
	CHECK(stamp_calls == before);
	CHECK(AUTH(eap, sizeof(eap), true) == 1);
	CHECK(AUTH(llc, sizeof(llc), false) == 1);
	/* A first EAP subframe cannot authorize the rest of either aggregate. */
	rx.flags = fRX_AMSDU; before = stamp_calls;
	CHECK(AUTH(eap, sizeof(eap), true) == -1);
	CHECK(stamp_calls == before);
	rx.flags = fRX_ARALINK;
	CHECK(AUTH(llc, sizeof(llc), false) == -1);
	CHECK(stamp_calls == before);
	ad.tr_ctl.tr_entry[1].PortSecured = WPA_802_1X_PORT_SECURED;
	CHECK(AUTH(llc, sizeof(llc), false) == 0);
	rx.flags = 0;
	peer.PrivacyFilter = Ndis802_11PrivFilter8021xWEP;
	CHECK(AUTH(eth, sizeof(eth), true) == -1);
	CHECK(AUTH(eap, sizeof(eap), true) == 1);
	peer.PrivacyFilter = 0;
	before = stamp_calls;
	CHECK(AUTH(eap, sizeof(eap) - 1, true) == -1);
	CHECK(stamp_calls == before);
	stamp_result = false;
	CHECK(AUTH(eth, sizeof(eth), true) == -1);
	CHECK(AUTH(eap, sizeof(eap), true) == -1);
	stamp_result = true;
	rx.ap_bridge_reader.gate = false;
	CHECK(AUTH(eth, sizeof(eth), true) == -1);
	rx.ap_bridge_reader.gate = true;
	/* Fragment buffering requires ordinary authorization even for EAPOL. */
	CHECK(mtk_ap_rx_fragment_prepare(&ad, &peer, &rx, &ap));
	ad.tr_ctl.tr_entry[1].PortSecured = 0;
	CHECK(!mtk_ap_rx_fragment_prepare(&ad, &peer, &rx, &ap));
	ad.tr_ctl.tr_entry[1].PortSecured = WPA_802_1X_PORT_SECURED;
	peer.a4 = true; CHECK(!mtk_ap_rx_fragment_prepare(&ad, &peer, &rx, &ap)); peer.a4 = false;
	peer.wdev = &other; CHECK(!mtk_ap_rx_fragment_prepare(&ad, &peer, &rx, &ap)); peer.wdev = &ap;
	CHECK(mtk_ap_rx_authorize(&ad, NULL, &rx, &ap, eap, sizeof(eap), true) == -1);
	/* Non-AP dispatch retains the existing implementation's decision. */
	CHECK(mtk_ap_rx_authorize(&ad, NULL, &rx, &sta, eth, 0, true) == 0);
	printf("%u production source-authorization checks passed\n", checks);
}
