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

#ifdef PG_INT64_MIN
#undef PG_INT64_MIN
#endif
#ifdef PG_INT64_MAX
#undef PG_INT64_MAX
#endif

#endif /* CONST_MAX_HACK */

#ifndef PG_INT64_MIN
#define PG_INT64_MIN	(-INT64CONST(0x7FFFFFFFFFFFFFFF) - 1)
#endif
#ifndef PG_INT64_MAX
#define PG_INT64_MAX	INT64CONST(0x7FFFFFFFFFFFFFFF)
#endif

/* RIP, oids. */
#if PG_VERSION_NUM >= 120000
#define TupleDescHasOids(tupdesc) (false)
#define IsObjectIdAttributeNumber(a) (false)
/*
 * since hasoid is always supposed to be false thanks to the above, any
 * references to the below macros should be unreachable
 */
#define HeapTupleHeaderGetOid(h) (AssertMacro(false), InvalidOid)
#define HeapTupleHeaderSetOid(h,o) do { (void) (h); (void) (o); Assert(false); } while (0)
#define HeapTupleSetOid(h,o) do { (void) (h); (void) (o); Assert(false); } while (0)
#else
#define TupleDescHasOids(tupdesc) ((tupdesc)->tdhasoid)
#define IsObjectIdAttributeNumber(a) ((a) == ObjectIdAttributeNumber)
#endif

/* TupleDesc structure change */
#if PG_VERSION_NUM < 100000
#define TupleDescAttr(tupdesc, i) ((tupdesc)->attrs[(i)])
#endif

/* AllocSetContextCreate API changes */
#if PG_VERSION_NUM < 110000
#define AllocSetContextCreateInternal AllocSetContextCreate
#elif PG_VERSION_NUM < 120000
#define AllocSetContextCreateInternal AllocSetContextCreateExtended
#endif
/*
 * Protect against backpatching or lack thereof.
 */
#ifndef ALLOCSET_DEFAULT_SIZES
#define ALLOCSET_DEFAULT_SIZES \
	ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE
#endif
#ifndef ALLOCSET_SMALL_SIZES
#define ALLOCSET_SMALL_SIZES \
	ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE
#endif
#ifndef ALLOCSET_START_SMALL_SIZES
#define ALLOCSET_START_SMALL_SIZES \
	ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE
#endif

/* We want a way to do noinline, but old PGs don't have it. */

#if defined(pg_noinline)
#define pllua_noinline pg_noinline
#elif (defined(__GNUC__) && __GNUC__ > 2) || defined(__SUNPRO_C) || defined(__IBMC__)
#define pllua_noinline __attribute__((noinline))
/* msvc via declspec */
#elif defined(_MSC_VER)
#define pllua_noinline __declspec(noinline)
#else
#define pllua_noinline
#endif

#endif /* PLLUA_PGVER_H */
