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

/* float4 is always by value in pg >= 13. */
#if PG_VERSION_NUM >= 130000
#ifndef USE_FLOAT4_BYVAL
#define USE_FLOAT4_BYVAL 1
#endif
#endif

/*
 * CommandTag in pg13+ is an enum, not a string. GetCommandTagName returns the
 * old name, but obviously didn't exist in previous versions.
 */
#if PG_VERSION_NUM < 130000
#define GetCommandTagName(t_) (t_)
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

/* cope with variable-length fcinfo in pg12 */
#if PG_VERSION_NUM < 120000
#define LOCAL_FCINFO(name_,nargs_) \
	FunctionCallInfoData name_##data; \
	FunctionCallInfo name_ = &name_##data

#define LFCI_ARG_VALUE(fci_,n_) ((fci_)->arg[n_])
#define LFCI_ARGISNULL(fci_,n_) ((fci_)->argnull[n_])
#else
#define LFCI_ARG_VALUE(fci_,n_) ((fci_)->args[n_].value)
#define LFCI_ARGISNULL(fci_,n_) ((fci_)->args[n_].isnull)
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

#ifndef __has_builtin
#define __has_builtin(x_) 0
#endif
#ifndef __has_attribute
#define __has_attribute(x_) 0
#endif

#if defined(pg_noinline)
#define pllua_noinline pg_noinline
#elif __has_attribute(noinline) || (defined(__GNUC__) && __GNUC__ > 2) || defined(__SUNPRO_C) || defined(__IBMC__)
#define pllua_noinline __attribute__((noinline))
/* msvc via declspec */
#elif defined(_MSC_VER)
#define pllua_noinline __declspec(noinline)
#else
#define pllua_noinline
#endif

/* and likewise for unlikely() */
#if !defined(unlikely)

#if !defined(__builtin_expect) && !defined(__GNUC__) && !__has_builtin(__builtin_expect)
#define __builtin_expect(x_,y_) (x_)
#endif
#define likely(x)	(__builtin_expect(!!(x), 1))
#define unlikely(x)	(__builtin_expect(!!(x), 0))

#endif /* unlikely */

#endif /* PLLUA_PGVER_H */
