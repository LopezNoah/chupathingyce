"""Compiler flags for builds that link tools/allocation_guard.c (ADR 0089).

The redirect is object-like so struct members or function pointers named
`free` are renamed consistently in every translation unit. It is portable to
Apple ld, GNU ld, and lld, unlike `-Wl,--wrap`.
"""

ALLOCATION_GUARD_FLAGS = [
    "-Dmalloc=allocation_guard_malloc",
    "-Dcalloc=allocation_guard_calloc",
    "-Drealloc=allocation_guard_realloc",
    "-Dfree=allocation_guard_free",
]
