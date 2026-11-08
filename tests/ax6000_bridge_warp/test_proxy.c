/* Production policy functions are extracted by test_proxy.py. Only OS/MMIO
 * operations and the small external object registry are host adapters.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include "mtk_cpu_lifecycle.h"

typedef uint32_t u32;
typedef uint64_t u64;
typedef pthread_mutex_t spinlock_t;
typedef int wait_queue_head_t;
struct mutex { pthread_mutex_t lock; };
#define CONFIG_MTK_AP_BRIDGE 1
#define WED_TX_SUPPORT 1
#define WED_RX_SUPPORT 1
#define WED_RX_D_SUPPORT 1
#define BIT(n) (1U << (n))
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define EXPORT_SYMBOL(x)
#define WARN_ON_ONCE(x) (x)
#define wake_up_all(q) ((void)(q))
#define MTK_CPU_DMA_CONTROL_RACE 1

static spinlock_t cpu_profile_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mtk_cpu_life_state cpu_life;
typedef atomic_uint atomic_t;
static atomic_t cpu_profile_fatal;
static _Thread_local unsigned long current = 17;
static _Thread_local bool interrupt_context;
#define atomic_read(p) atomic_load(p)
static bool in_interrupt(void) { return interrupt_context; }
static bool fatal_after_gate_lock;
static spinlock_t *target_gate;
static void lock(spinlock_t *gate)
{
	assert(!pthread_mutex_lock(gate));
	if (gate == target_gate && fatal_after_gate_lock) {
		fatal_after_gate_lock = false;
		atomic_store(&cpu_profile_fatal, 1);
	}
}
#define spin_lock_irqsave(m, f) do { (f) = 0; lock(m); } while (0)
#define spin_unlock_irqrestore(m, f) do { (void)(f); assert(!pthread_mutex_unlock(m)); } while (0)
static void __attribute__((noreturn)) mtk_cpu_dma_fatal(int reason, int error)
{
	(void)reason;
	(void)error;
	abort(); /* An unexpected active-counter underflow is a test failure. */
}
#include "proxy-permission.inc"
#include "proxy-state.inc"
#include "regs/coda/wed_hw.h"

struct wed_entry { struct warp_checked_state checked; };
struct wifi_ring { u32 base, cnt, cidx, didx; };
struct wifi_hw {
	void *priv;
	u32 int_sta, int_mask;
	struct wifi_ring event, tx[2], rx[2];
};
struct wifi_entry { struct wifi_hw hw; };
struct warp_wifi_cr_map { u32 wifi_cr, warp_cr; };
struct warp_entry {
	struct wed_entry wed;
	struct wifi_entry wifi;
	struct warp_wifi_cr_map mtbl[32];
};
static struct warp_entry entry;
static int owner, stranger;
static struct warp_entry *warp_entry_get_by_privdata(void *value)
{
	return value == entry.wifi.hw.priv ? &entry : NULL;
}
#define warp_dbg(...) ((void)0)
#define WARP_DBG_ALL 0
static u32 registers[256];
static unsigned int reads, writes, checks;
static u32 last_address, last_value;
enum mode_flip { NO_FLIP, CLOSE_IRQ_LANE, OPEN_IRQ_LANE };
static enum mode_flip flip_on_read;
static void mmio_read(struct wed_entry *wed, u32 address, u32 *value)
{
	assert(wed == &entry.wed && wed->checked.active);
	assert(address / 4 < sizeof(registers) / sizeof(registers[0]));
	reads++;
	last_address = address;
	*value = registers[address / 4];
	/* A real admitted access may overlap a later control-mode transition. */
	if (flip_on_read == CLOSE_IRQ_LANE)
		entry.wed.checked.control_irq = false;
	else if (flip_on_read == OPEN_IRQ_LANE)
		entry.wed.checked.control_irq = true;
	flip_on_read = NO_FLIP;
}
static void mmio_write(struct wed_entry *wed, u32 address, u32 value)
{
	assert(wed == &entry.wed && wed->checked.active);
	assert(address / 4 < sizeof(registers) / sizeof(registers[0]));
	writes++;
	last_address = address;
	last_value = value;
	registers[address / 4] = value;
}
#define warp_io_read32(wed, addr, value) mmio_read(wed, addr, value)
#define warp_io_write32(wed, addr, value) mmio_write(wed, addr, value)
#include "proxy-gate.inc"
#include "proxy-mmio.inc"

/* MT7986 RBUS input offsets; both map construction and lookup are production
 * functions, so future map changes cannot silently intercept native N9 ACK.
 */
#define STATUS 0x24200U
#define MASK 0x24204U
#define TX_BASE 0x24420U
#define TX_COUNT 0x24424U
#define TX_INDEX 0x24428U
#define N9_ACK 0x241f0U
#define N9_ENABLE 0x241f4U
#define HOST2N9 0x54000108U
#define CHECK(expression) do { assert(expression); checks++; } while (0)

static void fresh(void)
{
	static bool initialized;
	if (initialized)
		assert(!pthread_mutex_destroy(&entry.wed.checked.gate));
	memset(&entry, 0, sizeof(entry));
	memset(registers, 0, sizeof(registers));
	memset(&cpu_life, 0, sizeof(cpu_life));
	assert(!pthread_mutex_init(&entry.wed.checked.gate, NULL));
	initialized = true;
	target_gate = &entry.wed.checked.gate;
	entry.wed.checked.initialized = true;
	entry.wifi.hw.priv = &owner;
	entry.wifi.hw.int_sta = STATUS;
	entry.wifi.hw.int_mask = MASK;
	entry.wifi.hw.tx[0] = (struct wifi_ring){ TX_BASE, TX_COUNT, TX_INDEX, 0x2442c };
	entry.wifi.hw.tx[1] = (struct wifi_ring){ 0x24430, 0x24434, 0x24438, 0x2443c };
	entry.wifi.hw.event = (struct wifi_ring){ 0x24520, 0x24524, 0x24528, 0x2452c };
	entry.wifi.hw.rx[0] = (struct wifi_ring){ 0x24500, 0x24504, 0x24508, 0x2450c };
	entry.wifi.hw.rx[1] = (struct wifi_ring){ 0x24510, 0x24514, 0x24518, 0x2451c };
	warp_mtable_build_hw(&entry);
	atomic_store(&cpu_profile_fatal, 0);
	current = 17;
	interrupt_context = false;
	fatal_after_gate_lock = false;
	flip_on_read = NO_FLIP;
	reads = writes = 0;
	last_address = last_value = 0;
}

static void denied_ring(void)
{
	u32 value = ~0U;
	unsigned int before_read = reads, before_write = writes;
	CHECK(warp_proxy_read(entry.wifi.hw.priv, TX_BASE, &value) == 0);
	CHECK(value == 0 && reads == before_read);
	CHECK(warp_proxy_write(entry.wifi.hw.priv, TX_BASE, ~0U) == 0);
	CHECK(writes == before_write && entry.wed.checked.active == 0);
	CHECK(warp_proxy_write(entry.wifi.hw.priv, TX_COUNT, 1024) == 0);
	CHECK(warp_proxy_write(entry.wifi.hw.priv, TX_INDEX, 128) == 0);
	CHECK(writes == before_write);
}

static u64 begin_wifi_permit(bool cleanup)
{
	unsigned long long cookie;
	CHECK(mtk_cpu_life_begin_wifi(&cpu_life, &owner, current, 0, &cookie) == 0);
	entry.wed.checked.cookie = cookie;
	if (cleanup)
		CHECK(mtk_cpu_life_cleanup(&cpu_life, current, cookie, true) == 0);
	return cookie;
}

int main(void)
{
	u32 value;
	u64 cookie;
	unsigned int before;
	struct mtk_cpu_lifetime teardown;
	fresh();
	registers[0x300 / 4] = 0xa55aff11;
	CHECK(warp_proxy_read(&owner, TX_BASE, &value) == 0);
	CHECK(value == 0xa55aff11 && reads == 1 && !entry.wed.checked.active);
	CHECK(warp_proxy_write(&owner, TX_COUNT, 0x99) == 0);
	CHECK(last_address == 0x304 && last_value == 0x99 && writes == 1);
	CHECK(warp_checked_enter(&entry.wed));
	CHECK(entry.wed.checked.active == 1);
	CHECK(warp_proxy_read(&owner, TX_BASE, &value) == 0);
	CHECK(entry.wed.checked.active == 1);
	warp_checked_exit(&entry.wed);
	CHECK(!entry.wed.checked.active);

	fresh(); entry.wed.checked.initialized = false;
	denied_ring();
	fresh(); entry.wed.checked.stopping = true;
	CHECK(!warp_checked_enter(&entry.wed));
	denied_ring();
	value = ~0U;
	CHECK(warp_proxy_read(&owner, STATUS, &value) == 0 && value == 0 && reads == 0);
	CHECK(warp_proxy_write(&owner, MASK, ~0U) == 0 && writes == 0);

	/* IRQ status/ack/rearm works while ordinary entry and ring access stop. */
	entry.wed.checked.control_irq = true;
	interrupt_context = true;
	registers[0x200 / 4] = registers[0x204 / 4] = ~0U;
	CHECK(warp_proxy_read(&owner, STATUS, &value) == 0 && value == WARP_CONTROL_IRQ);
	CHECK(warp_proxy_read(&owner, MASK, &value) == 0 && value == WARP_CONTROL_IRQ);
	CHECK(warp_proxy_write(&owner, MASK, ~0U) == 0 && last_value == WARP_CONTROL_IRQ);
	CHECK(warp_proxy_write(&owner, STATUS, ~0U) == 0 && last_value == WARP_CONTROL_IRQ);
	CHECK(!entry.wed.checked.active);
	denied_ring();
	CHECK(warp_proxy_write(&owner, MASK, 0) == 0 && last_value == 0);
	CHECK(warp_proxy_write(&owner, MASK, 1) == 0 && last_value == 0);
	CHECK(warp_proxy_write(&owner, MASK, WARP_CONTROL_IRQ | 1) == 0 && last_value == WARP_CONTROL_IRQ);
	registers[0x200 / 4] = 1;
	CHECK(warp_proxy_read(&owner, STATUS, &value) == 0 && value == 0);

	/* Regression: capture the read permission under the gate, not after IO. */
	registers[0x200 / 4] = ~0U;
	flip_on_read = CLOSE_IRQ_LANE;
	CHECK(warp_proxy_read(&owner, STATUS, &value) == 0 && value == WARP_CONTROL_IRQ);
	CHECK(!entry.wed.checked.control_irq && entry.wed.checked.stopping);
	value = ~0U; before = reads;
	CHECK(warp_proxy_read(&owner, STATUS, &value) == 0 && value == 0 && reads == before);

	/* Actual kernel permission rejects the IRQ current task even when it
	 * happens to equal the explicitly authorized SER actor.
	 */
	fresh(); entry.wed.checked.stopping = true;
	cookie = begin_wifi_permit(false);
	CHECK(!mtk_cpu_control_permitted(&owner, cookie));
	denied_ring();
	CHECK(mtk_cpu_life_cleanup(&cpu_life, current, cookie, true) == 0);
	CHECK(mtk_cpu_control_permitted(&owner, cookie));
	CHECK(warp_proxy_write(&owner, TX_BASE, 0xa000) == 0 && last_value == 0xa000 && writes == 1);
	registers[0x300 / 4] = 0xf001;
	CHECK(warp_proxy_read(&owner, TX_BASE, &value) == 0 && value == 0xf001);
	interrupt_context = true;
	CHECK(!mtk_cpu_control_permitted(&owner, cookie));
	denied_ring();
	interrupt_context = false;
	current++;
	CHECK(!mtk_cpu_control_permitted(&owner, cookie));
	denied_ring();
	current--;
	entry.wifi.hw.priv = &stranger;
	CHECK(!mtk_cpu_control_permitted(&stranger, cookie));
	denied_ring();
	entry.wifi.hw.priv = &owner;
	entry.wed.checked.cookie++;
	CHECK(!mtk_cpu_control_permitted(&owner, entry.wed.checked.cookie));
	denied_ring();
	entry.wed.checked.cookie = 0;
	CHECK(!mtk_cpu_control_permitted(&owner, 0));
	denied_ring();
	entry.wed.checked.cookie = cookie;
	CHECK(mtk_cpu_life_cleanup(&cpu_life, current, cookie, false) == 0);
	denied_ring();

	/* Read permission cannot be tightened or broadened by a later mode
	 * change: an admitted actor/normal access retains its captured mask.
	 */
	CHECK(mtk_cpu_life_cleanup(&cpu_life, current, cookie, true) == 0);
	registers[0x204 / 4] = ~0U;
	flip_on_read = OPEN_IRQ_LANE;
	CHECK(warp_proxy_read(&owner, MASK, &value) == 0 && value == ~0U);
	CHECK(entry.wed.checked.control_irq);
	CHECK(warp_proxy_read(&owner, MASK, &value) == 0 && value == WARP_CONTROL_IRQ);

	/* Explicit global teardown is a separate real lifecycle authorization. */
	fresh(); entry.wed.checked.stopping = true;
	CHECK(mtk_cpu_life_destruct_enter(&cpu_life, current, &teardown) == 0);
	CHECK(mtk_cpu_control_permitted(NULL, 0));
	CHECK(warp_proxy_write(&owner, TX_BASE, 0xb000) == 0 && writes == 1);
	current++;
	denied_ring();
	current--;
	CHECK(mtk_cpu_life_destruct_exit(&cpu_life, current, &teardown) == 0);
	denied_ring();

	/* Sticky error stops normal access; fatal stops even control/actor IO. */
	fresh(); entry.wed.checked.error = -EIO;
	denied_ring();
	entry.wed.checked.control_irq = true;
	registers[0x200 / 4] = ~0U;
	CHECK(warp_proxy_read(&owner, STATUS, &value) == 0 && value == WARP_CONTROL_IRQ);
	begin_wifi_permit(true);
	atomic_store(&cpu_profile_fatal, 1);
	CHECK(!mtk_cpu_control_permitted(&owner, entry.wed.checked.cookie));
	denied_ring();
	before = reads; value = ~0U;
	CHECK(warp_proxy_read(&owner, STATUS, &value) == 0 && value == 0 && reads == before);
	before = writes;
	CHECK(warp_proxy_write(&owner, MASK, ~0U) == 0 && writes == before);
	fresh();
	fatal_after_gate_lock = true;
	denied_ring();
	CHECK(atomic_load(&cpu_profile_fatal));

	/* N9 source status/ENA and host->N9 notifications are native MMIO,
	 * deliberately absent from the WED map. Returning negative preserves
	 * D's native ACK fallback; mapped denial above always returns zero.
	 */
	for (unsigned int mode = 0; mode < 3; mode++) {
		fresh();
		entry.wed.checked.stopping = mode != 0;
		entry.wed.checked.control_irq = mode != 0;
		interrupt_context = mode != 0;
		if (mode == 2)
			atomic_store(&cpu_profile_fatal, 1);
		value = 0x55;
		CHECK(warp_proxy_read(&owner, N9_ACK, &value) < 0 && value == 0x55);
		CHECK(warp_proxy_write(&owner, N9_ACK, 0x3c) < 0);
		CHECK(warp_proxy_write(&owner, N9_ENABLE, 0x3c) < 0);
		CHECK(warp_proxy_write(&owner, HOST2N9, 1) < 0);
		CHECK(!reads && !writes && !entry.wed.checked.active);
	}
	fresh();
	CHECK(warp_proxy_write(&stranger, TX_BASE, 1) < 0);
	CHECK(!reads && !writes);
	printf("%u production WARP proxy/IO/permission checks passed\n", checks);
}
