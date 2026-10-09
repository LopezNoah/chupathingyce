#ifndef TOOLS_ALLOCATION_GUARD_H
#define TOOLS_ALLOCATION_GUARD_H

#include <stddef.h>

/* ADR 0089: runtime subsystems allocate nothing after initialization.
   Guarded test builds compile every source with the flags from
   tools/allocation_guard_build.py, which redirect malloc, calloc, realloc,
   and free to the functions below. A compile-time redirect works with both
   GNU ld and Apple ld (which has no --wrap). Every call from the compiled
   objects while the guard is armed is counted; allocations made inside libc
   itself are not, matching the old --wrap behavior. */

void *allocation_guard_malloc(size_t size);
void *allocation_guard_calloc(size_t count, size_t size);
void *allocation_guard_realloc(void *pointer, size_t size);
void allocation_guard_free(void *pointer);

void allocation_guard_arm(void);
/* Returns the violations seen while armed and disarms the guard. */
unsigned long allocation_guard_disarm(void);

#endif
