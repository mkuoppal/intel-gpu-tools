#include "igt_rand.h"

static uint32_t state = 0x12345678;

uint32_t
hars_petruska_f54_1_random_seed(uint32_t new_state)
{
	uint32_t old_state = state;
	state = new_state;
	return old_state;
}

uint32_t
hars_petruska_f54_1_random_unsafe(void)
{
#define rol(x,k) ((x << k) | (x >> (32-k)))
	return state = (state ^ rol (state, 5) ^ rol (state, 24)) + 0x37798849;
#undef rol
}
