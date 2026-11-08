#include <assert.h>
#include <stdio.h>
#include "mtk_cpu_lifecycle.h"

int main(void)
{
	struct mtk_cpu_life_state s = {0};
	struct mtk_cpu_lifetime a = {0}, b = {0};
	unsigned long long fe, wifi, old;
	int owner_a, owner_b;

	_Static_assert(sizeof(struct mtk_cpu_lifetime) == 16, "permit ABI");
	assert(!mtk_cpu_life_active(&s));
	assert(mtk_cpu_life_begin_fe(&s, 11, &fe) == 0);
	assert(fe && mtk_cpu_life_active(&s));
	assert(mtk_cpu_life_destruct_enter(&s, 22, &a) == -EBUSY);
	assert(mtk_cpu_life_begin_wifi(&s, &owner_a, 22, fe + 1, &wifi) == -ESTALE);
	assert(mtk_cpu_life_begin_wifi(&s, &owner_a, 22, 0, &wifi) == -EBUSY);
	assert(mtk_cpu_life_begin_wifi(&s, &owner_a, 22, fe, &wifi) == 0);
	assert(wifi == fe);
	assert(mtk_cpu_life_cleanup(&s, 33, fe, true) == -ESTALE);
	assert(mtk_cpu_life_cleanup(&s, 22, wifi, true) == 0);
	assert(mtk_cpu_life_cleanup(&s, 22, wifi, true) == -EALREADY);
	assert(mtk_cpu_life_destruct_enter(&s, 22, &a) == 0);
	assert(a.kind == MTK_CPU_LIFE_SER_CLEANUP);
	assert(mtk_cpu_life_destruct_enter(&s, 33, &b) == -EBUSY);
	assert(mtk_cpu_life_destruct_exit(&s, 33, &a) == -EPERM);
	assert(mtk_cpu_life_destruct_exit(&s, 22, &a) == 0);
	assert(mtk_cpu_life_end_wifi(&s, &owner_a, 22, wifi) == -EBUSY);
	assert(mtk_cpu_life_end_fe(&s, 11, fe) == 0);
	assert(mtk_cpu_life_active(&s));
	assert(mtk_cpu_life_cleanup(&s, 22, wifi, false) == 0);
	assert(mtk_cpu_life_end_wifi(&s, &owner_b, 22, wifi) == -ESTALE);
	assert(mtk_cpu_life_end_wifi(&s, &owner_a, 22, wifi) == 0);
	assert(!mtk_cpu_life_active(&s));
	old = fe;

	assert(mtk_cpu_life_destruct_enter(&s, 33, &a) == 0);
	assert(mtk_cpu_life_destruct_enter(&s, 33, &b) == 0);
	assert(s.teardown_depth == 2);
	assert(mtk_cpu_life_begin_fe(&s, 11, &fe) == -EBUSY);
	assert(mtk_cpu_life_destruct_exit(&s, 44, &a) == -EPERM);
	assert(mtk_cpu_life_destruct_exit(&s, 33, &b) == 0);
	assert(mtk_cpu_life_active(&s));
	assert(mtk_cpu_life_destruct_exit(&s, 33, &a) == 0);
	assert(mtk_cpu_life_destruct_exit(&s, 33, &a) == -ESTALE);
	assert(!mtk_cpu_life_active(&s));

	assert(mtk_cpu_life_begin_wifi(&s, &owner_a, 22, 0, &wifi) == 0);
	assert(wifi > old);
	assert(mtk_cpu_life_cleanup(&s, 22, old, true) == -ESTALE);
	assert(mtk_cpu_life_begin_wifi(&s, &owner_b, 44, 0, &fe) == -EBUSY);
	assert(mtk_cpu_life_end_wifi(&s, &owner_a, 22, wifi) == 0);
	s.sequence = ~0ULL;
	assert(mtk_cpu_life_begin_fe(&s, 11, &fe) == -EOVERFLOW);
	assert(!mtk_cpu_life_active(&s));
	assert(mtk_cpu_life_destruct_enter(&s, 33, &a) == -EOVERFLOW);
	assert(!mtk_cpu_life_active(&s));
	puts("lifecycle: actual production transitions passed");
	return 0;
}
