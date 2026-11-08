/* Compile the actual production parser/binding helper, including with ASan. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ap_bridge_parse.h"

static unsigned int tests;
static void expect(const unsigned char *p, unsigned int n,
		enum apbr_wire_format format, enum apbr_wire_kind expected)
{
	struct apbr_wire_view view;
	unsigned char *exact = malloc(n ? n : 1);
	if (n)
		memcpy(exact, p, n);
	assert(apbr_parse_wire(exact, n, format, &view) == expected);
	free(exact);
	tests++;
}

int main(void)
{
	unsigned char eth[128] = { 0 }, llc[128] = { 0 }, wifi[160] = { 0 };
	unsigned char da[6] = { 2, 1, 2, 3, 4, 5 }, other[6] = { 2, 1, 2, 3, 4, 6 };
	unsigned char group[6] = { 1, 0, 0, 0, 0, 1 };
	struct apbr_wire_view view;
	int a, b;
	unsigned int n;
	eth[12] = 0x88; eth[13] = 0x8e;
	eth[14] = 2; eth[15] = 1; /* EAPOL Start, zero body. */
	for (n = 0; n < 18; n++)
		expect(eth, n, APBR_ETHERNET, APBR_WIRE_INVALID);
	expect(eth, 18, APBR_ETHERNET, APBR_WIRE_EAPOL);
	/* EtherType is at +12; an EAPOL-looking payload is not the EtherType. */
	eth[12] = 8; eth[13] = 0;
	eth[14] = 0x88; eth[15] = 0x8e;
	expect(eth, 18, APBR_ETHERNET, APBR_WIRE_DATA);
	eth[12] = 0x88; eth[13] = 0x8e;
	eth[14] = 2; eth[15] = 1;
	eth[17] = 1;
	expect(eth, 18, APBR_ETHERNET, APBR_WIRE_INVALID);
	expect(eth, 19, APBR_ETHERNET, APBR_WIRE_INVALID);
	eth[17] = 0;
	eth[14] = 0;
	expect(eth, 18, APBR_ETHERNET, APBR_WIRE_INVALID);
	eth[14] = 2; eth[15] = 9;
	expect(eth, 18, APBR_ETHERNET, APBR_WIRE_INVALID);
	eth[15] = 0; eth[17] = 4;
	eth[18] = 3; eth[20] = 0; eth[21] = 4; /* EAP success. */
	expect(eth, 22, APBR_ETHERNET, APBR_WIRE_EAPOL);
	eth[18] = 1; /* Request without EAP type. */
	expect(eth, 22, APBR_ETHERNET, APBR_WIRE_INVALID);
	eth[17] = 5; eth[21] = 5; eth[22] = 1;
	expect(eth, 23, APBR_ETHERNET, APBR_WIRE_EAPOL);
	eth[21] = 4;
	expect(eth, 23, APBR_ETHERNET, APBR_WIRE_INVALID);
	/* LLC and bridge-tunnel SNAP use an explicit, bounded input format. */
	memcpy(llc, "\xaa\xaa\x03\x00\x00\x00", 6);
	llc[6] = 0x88; llc[7] = 0x8e; llc[8] = 2; llc[9] = 1;
	for (n = 0; n < 12; n++)
		expect(llc, n, APBR_LLC, APBR_WIRE_INVALID);
	expect(llc, 12, APBR_LLC, APBR_WIRE_EAPOL);
	llc[5] = 0xf8;
	expect(llc, 12, APBR_LLC, APBR_WIRE_EAPOL);
	llc[3] = 1;
	expect(llc, 12, APBR_LLC, APBR_WIRE_DATA);
	llc[3] = 0;
	/* QinQ + 802.1Q retains the exact first type offset. */
	memset(eth, 0, sizeof(eth));
	eth[12] = 0x88; eth[13] = 0xa8;
	eth[16] = 0x81; eth[17] = 0;
	eth[20] = 0x88; eth[21] = 0x8e; eth[22] = 2; eth[23] = 1;
	expect(eth, 26, APBR_ETHERNET, APBR_WIRE_EAPOL);
	assert(apbr_parse_wire(eth, 26, APBR_ETHERNET, &view) == APBR_WIRE_EAPOL);
	assert(view.tags == 2 && view.type_offset == 12 && view.eap_offset == 22);
	for (n = 14; n < 26; n++)
		expect(eth, n, APBR_ETHERNET, APBR_WIRE_INVALID);
	/* Non-QoS and QoS+HTC 802.11 headers, with no fixed +26 guess. */
	wifi[0] = 8; wifi[1] = 1;
	memcpy(wifi + 24, llc, 12);
	expect(wifi, 36, APBR_IEEE80211, APBR_WIRE_EAPOL);
	wifi[0] = 0x88; wifi[1] = 0x81;
	memset(wifi + 24, 0, 6);
	memcpy(wifi + 30, llc, 12);
	expect(wifi, 42, APBR_IEEE80211, APBR_WIRE_EAPOL);
	wifi[24] = 0x80; /* A-MSDU never borrows the first EAPOL exception. */
	expect(wifi, 42, APBR_IEEE80211, APBR_WIRE_DATA);
	/* Actual final-binding predicate, including same-WCID wrong-MAC reuse. */
#define BIND(dst, peer, pw, tw, rec, assoc, ordinary, secured, auth, radius) \
	apbr_tx_binding_matches(dst, peer, &a, pw, tw, 9, 10, rec, assoc, ordinary, \
		secured, 1, auth, radius)
	assert(BIND(da, da, &a, &a, 1, 1, 1, 1, 0, 1)); tests++;
	assert(!BIND(da, da, &b, &a, 1, 1, 1, 1, 0, 1)); tests++;
	assert(!BIND(da, da, &a, &b, 1, 1, 1, 1, 0, 1)); tests++;
	assert(!BIND(da, other, &a, &a, 1, 1, 1, 1, 0, 1)); tests++;
	assert(!BIND(da, da, &a, &a, 0, 1, 1, 1, 0, 1)); tests++;
	assert(!BIND(da, da, &a, &a, 1, 0, 1, 1, 0, 1)); tests++;
	assert(!BIND(da, da, &a, &a, 1, 1, 0, 1, 0, 1)); tests++;
	assert(!BIND(da, da, &a, &a, 1, 1, 1, 0, 0, 1)); tests++;
	assert(BIND(da, da, &a, &a, 1, 1, 1, 0, 1, 1)); tests++;
	assert(!BIND(da, da, &a, &a, 1, 1, 1, 1, 0, 0)); tests++;
	assert(!BIND(group, da, &a, &a, 1, 1, 1, 1, 0, 1)); tests++;
	assert(apbr_tx_binding_matches(group, da, &a, &a, &a,
		10, 10, 1, 0, 0, 0, 1, 0, 1)); tests++;
	/* Actual bridge-hook policy predicate, including ingress-only traffic. */
	assert(!apbr_isolation_denies(1, 0, 0, 1, 1)); tests++;
	assert(!apbr_isolation_denies(0, 1, 0, 1, 1)); tests++;
	assert(!apbr_isolation_denies(1, 1, 1, 0, 1)); tests++;
	assert(apbr_isolation_denies(1, 1, 1, 1, 0)); tests++;
	assert(apbr_isolation_denies(1, 1, 0, 0, 1)); tests++;
	assert(!apbr_isolation_denies(1, 1, 0, 1, 0)); tests++;
	assert(apbr_isolation_denies(-1, 0, 0, 0, 0)); tests++;
	assert(apbr_isolation_denies(0, -1, 0, 0, 0)); tests++;
	printf("%u driver wire/binding checks passed\n", tests);
	return 0;
}
