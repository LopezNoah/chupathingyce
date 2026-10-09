/* The guard must reach the real allocator, so undo the build's redirects
   before any system header declares malloc and friends. */
#undef malloc
#undef calloc
#undef realloc
#undef free

#include "allocation_guard.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>

static _Atomic int guard_armed;
static _Atomic unsigned long guard_violations;

static void guard_check(void)
{
	if (atomic_load_explicit(&guard_armed, memory_order_relaxed))
		atomic_fetch_add_explicit(&guard_violations, 1, memory_order_relaxed);
}

void *allocation_guard_malloc(size_t size)
{
	guard_check();
	return malloc(size);
}

void *allocation_guard_calloc(size_t count, size_t size)
{
	guard_check();
	return calloc(count, size);
}

void *allocation_guard_realloc(void *pointer, size_t size)
{
	guard_check();
	return realloc(pointer, size);
}

void allocation_guard_free(void *pointer)
{
	if (pointer)
		guard_check();
	free(pointer);
}

void allocation_guard_arm(void)
{
	atomic_store(&guard_violations, 0);
	atomic_store(&guard_armed, 1);
}

unsigned long allocation_guard_disarm(void)
{
	atomic_store(&guard_armed, 0);
	return atomic_load(&guard_violations);
}
