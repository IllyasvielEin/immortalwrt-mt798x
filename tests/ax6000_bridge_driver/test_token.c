/* Compile the production reservation function; model only Linux atomics/types. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define READ_ONCE(x) (x)
#define smp_wmb() atomic_thread_fence(memory_order_release)
typedef uint32_t UINT32;
typedef unsigned long ULONG;
typedef void *PVOID;
typedef void *PNDIS_PACKET;
typedef uintptr_t NDIS_PHYSICAL_ADDRESS;
typedef atomic_uint atomic_t;
static UINT32 atomic_read(atomic_t *a) { return atomic_load(a); }
static UINT32 atomic_cmpxchg(atomic_t *a, UINT32 old, UINT32 value)
{
	atomic_compare_exchange_strong(a, &old, value);
	return old;
}
struct token_entry {
	void *pkt_buf;
	struct { ULONG AllocSize; void *AllocVa; uintptr_t AllocPa; } dma_buf;
};
struct token_rx_pkt_queue {
	struct token_entry *pkt_token;
	UINT32 pkt_tkid_cnt;
	atomic_t cur_free_idx;
};
#include "token.inc"
#define CAPACITY 127
static struct token_entry entries[CAPACITY];
static struct token_rx_pkt_queue queue = { .pkt_token = entries, .pkt_tkid_cnt = CAPACITY };
static atomic_uint seen[CAPACITY], successes, rejected;
static void *reserve(void *argument)
{
	uintptr_t id = (uintptr_t)argument;
	for (unsigned int i = 0; i < 40; i++) {
		UINT32 token = token_rx_dmad_init(&queue, (void *)id, 4096, (void *)id, id);
		if (token == 0xffffffffU) {
			atomic_fetch_add(&rejected, 1);
			continue;
		}
		assert(token < CAPACITY);
		assert(atomic_fetch_add(&seen[token], 1) == 0);
		assert(entries[token].pkt_buf == (void *)id);
		assert(entries[token].dma_buf.AllocVa == (void *)id);
		assert(entries[token].dma_buf.AllocPa == id);
		assert(entries[token].dma_buf.AllocSize == 4096);
		atomic_fetch_add(&successes, 1);
	}
	return NULL;
}
int main(void)
{
	pthread_t threads[8];
	struct token_rx_pkt_queue empty = { 0 };
	assert(token_rx_dmad_init(NULL, NULL, 0, NULL, 0) == 0xffffffffU);
	assert(token_rx_dmad_init(&empty, NULL, 0, NULL, 0) == 0xffffffffU);
	empty.pkt_token = entries;
	assert(token_rx_dmad_init(&empty, NULL, 0, NULL, 0) == 0xffffffffU);
	for (uintptr_t i = 0; i < 8; i++)
		assert(!pthread_create(&threads[i], NULL, reserve, (void *)(i + 1)));
	for (unsigned int i = 0; i < 8; i++)
		assert(!pthread_join(threads[i], NULL));
	assert(atomic_load(&successes) == CAPACITY);
	assert(atomic_load(&rejected) == 320 - CAPACITY);
	assert(atomic_load(&queue.cur_free_idx) == CAPACITY);
	for (unsigned int i = 0; i < CAPACITY; i++)
		assert(atomic_load(&seen[i]) == 1);
	puts("production token reservation: 320 concurrent requests, 127 unique slots, bounds held");
}
