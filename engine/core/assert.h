#ifndef ENGINE_CORE_ASSERT_H
#define ENGINE_CORE_ASSERT_H

/* Assertions detect programmer errors and stay enabled in every build (ADR 0089).
   Operating errors (I/O, malformed content, exhausted budgets) are returned as
   results instead; never assert on them. */

#include <signal.h>
#include <stdio.h>

#define ENGINE_ASSERT_UNLIKELY(condition) (!!(condition))

static inline void engine_assert_fail(char const *condition, char const *file, int line)
{
	fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, condition);
	fflush(stderr);
	raise(SIGABRT);
	raise(SIGKILL);
}

#define ENGINE_ASSERT(condition) \
	(ENGINE_ASSERT_UNLIKELY(!(condition)) ? \
		engine_assert_fail(#condition, __FILE__, __LINE__) : (void)0)

#define ENGINE_STATIC_ASSERT(condition, message) _Static_assert(condition, message)

#endif
