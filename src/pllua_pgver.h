/*
 * pllua_pgver.h
 */

#ifndef PLLUA_PGVER_H
#define PLLUA_PGVER_H

/* PG version cruft */

/*
 * It sucks to depend on point release version, but some values we want to use
 * in #if directives are not compile-time constant in older point releases.
 * So for unfixed point releases, forcibly fix things here.
 *
 * Fortunately this matters only at compile time.
 */
#if (PG_VERSION_NUM >= 90500 && PG_VERSION_NUM < 90510) \
	|| (PG_VERSION_NUM >= 90600 && PG_VERSION_NUM < 90606) \
	|| (PG_VERSION_NUM >= 100000 && PG_VERSION_NUM < 100001)

#undef INT64CONST
#undef UINT64CONST
#if defined(HAVE_LONG_INT_64)
#define INT64CONST(x)  (x##L)
#define UINT64CONST(x) (x##UL)
#elif defined(HAVE_LONG_LONG_INT_64)
#define INT64CONST(x)  (x##LL)
#define UINT64CONST(x) (x##ULL)
#else
#error must have a working 64-bit integer datatype
#endif

#if PG_VERSION_NUM >= 90600
#undef PG_INT64_MIN
#undef PG_INT64_MAX
#define PG_INT64_MIN    (-INT64CONST(0x7FFFFFFFFFFFFFFF) - 1)
#define PG_INT64_MAX    INT64CONST(0x7FFFFFFFFFFFFFFF)
#endif

#endif /* CONST_MAX_HACK */

#if PG_VERSION_NUM < 100000
#define TupleDescAttr(tupdesc, i) ((tupdesc)->attrs[(i)])
#endif

#if PG_VERSION_NUM < 90600
#define PG_INT64_MIN	(-INT64CONST(0x7FFFFFFFFFFFFFFF) - 1)
#define PG_INT64_MAX	INT64CONST(0x7FFFFFFFFFFFFFFF)
#define ALLOCSET_DEFAULT_SIZES \
	ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE
#define ALLOCSET_SMALL_SIZES \
	ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE
#define ALLOCSET_START_SMALL_SIZES \
	ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE
#endif

#endif /* PLLUA_PGVER_H */
