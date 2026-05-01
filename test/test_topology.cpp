/*
 * test_topology.cpp
 *
 * Validates that the CPU topology values reported by xbyak (xbyak_util.h
 * CpuTopology) match those reported by hwloc for the same machine.
 *
 * Comparisons performed
 * ---------------------
 *   1. Logical CPU (PU) count
 *   2. Physical core count
 *   3. Cache-line size
 *   4. Core type counts: P-core / E-core / LP E-core
 *      LP E-cores are E-cores that have no L3 cache (e.g. Intel Meteor Lake).
 *   5. Cores sharing each cache level, per core type
 *      - P-core : typically 1 core shares L1, 1 shares L2, all non-LP-E share L3
 *      - E-core : typically 1 core shares L1, 4 share L2, all non-LP-E share L3
 *      - LP E-core : no L3; sharing counts for L1/L2 vary by micro-arch
 *   6. Per-logical-CPU cache sizes (L1d / L2 / L3)
 *
 * Supported platforms
 * -------------------
 *   Windows (x86-64) and Linux (x86-64).
 *   On unsupported architectures the xbyak topology block is skipped and the
 *   test exits with code 0 (not a failure).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

// -- xbyak --------------------------------------------------------------------
#include "xbyak/xbyak_util.h"

// -- hwloc --------------------------------------------------------------------
#include "hwloc.h"

// -----------------------------------------------------------------------------
// Minimal test harness
// -----------------------------------------------------------------------------

static int g_failures = 0;
static int g_passes   = 0;
static int g_warnings = 0;

static void check_eq(const char* label,
                     unsigned long long xbyak_val,
                     unsigned long long hwloc_val)
{
    if (xbyak_val == hwloc_val) {
        printf("  PASS  %-60s  xbyak=%-8llu  hwloc=%llu\n",
               label, xbyak_val, hwloc_val);
        ++g_passes;
    } else {
        printf("  FAIL  %-60s  xbyak=%-8llu  hwloc=%llu\n",
               label, xbyak_val, hwloc_val);
        ++g_failures;
    }
}

static void warn_neq(const char* label,
                     unsigned long long xbyak_val,
                     unsigned long long hwloc_val,
                     const char* reason)
{
    printf("  WARN  %-60s  xbyak=%-8llu  hwloc=%llu  (%s)\n",
           label, xbyak_val, hwloc_val, reason);
    ++g_warnings;
}

// -----------------------------------------------------------------------------
// Shared helpers
// -----------------------------------------------------------------------------

static const char* core_type_str(Xbyak::util::CoreType t)
{
    switch (t) {
    case Xbyak::util::Performance: return "P-core";
    case Xbyak::util::Efficient:   return "E-core";
    case Xbyak::util::Standard:    return "Standard";
    default:                       return "Unknown";
    }
}

/* -- xbyak helpers ----------------------------------------------------------- */

/*
 * Returns true when idx is the primary (lowest-index) logical CPU for its
 * physical core - the first entry in the L1i sibling mask.
 * Used to avoid double-counting HT siblings when tallying physical cores.
 */
static bool is_primary_logical_cpu(const Xbyak::util::CpuTopology& xTopo,
                                    uint32_t idx)
{
    const Xbyak::util::CpuMask& sib = xTopo.getLogicalCpu(idx).getSiblings();
    return sib.empty() || sib.get(0) == idx;
}

/*
 * Count the number of *physical* cores whose logical CPUs appear in the
 * sharedCpuIndices set of the given cache.  Returns 0 if the cache is absent.
 */
static size_t xbyak_physical_cores_sharing(const Xbyak::util::CpuTopology& xTopo,
                                            size_t cpu_idx,
                                            Xbyak::util::CacheType ct)
{
    const Xbyak::util::CpuCache& cache = xTopo.getCache(cpu_idx, ct);
    if (cache.size == 0) return 0;
    size_t cnt = 0;
    for (Xbyak::util::CpuMask::const_iterator it = cache.sharedCpuIndices.begin();
         it != cache.sharedCpuIndices.end(); ++it)
    {
        if (is_primary_logical_cpu(xTopo, *it)) ++cnt;
    }
    return cnt;
}

/*
 * Logical CPU classification used for counting and grouping.
 * xbyak marks both E-core and LP E-core as Efficient; we distinguish them by
 * whether the logical CPU has an L3 cache (LP E-cores do not).
 */
enum XbyakCoreClass { XCC_PCORE, XCC_ECORE, XCC_LP_ECORE, XCC_STANDARD };

static XbyakCoreClass classify_xbyak_cpu(const Xbyak::util::LogicalCpu& lcpu)
{
    switch (lcpu.coreType) {
    case Xbyak::util::Performance:
        return XCC_PCORE;
    case Xbyak::util::Efficient:
        return (lcpu.cache[Xbyak::util::L3].size > 0) ? XCC_ECORE : XCC_LP_ECORE;
    default:
        return XCC_STANDARD;
    }
}

/*
 * Count physical P-cores, E-cores, LP E-cores, and Standard cores.
 * Only primary logical CPUs (first HT sibling) are counted per physical core.
 */
static void count_xbyak_core_types(const Xbyak::util::CpuTopology& xTopo,
                                    size_t& pCores,
                                    size_t& eCores,
                                    size_t& lpECores,
                                    size_t& stdCores)
{
    pCores = eCores = lpECores = stdCores = 0;
    const size_t n = xTopo.getLogicalCpuNum();
    for (size_t i = 0; i < n; ++i) {
        if (!is_primary_logical_cpu(xTopo, (uint32_t)i)) continue;
        switch (classify_xbyak_cpu(xTopo.getLogicalCpu(i))) {
        case XCC_PCORE:    ++pCores;   break;
        case XCC_ECORE:    ++eCores;   break;
        case XCC_LP_ECORE: ++lpECores; break;
        default:           ++stdCores; break;
        }
    }
}

/* -- hwloc helpers ----------------------------------------------------------- */

/* Returns true for any hwloc cache object type. */
static bool obj_is_cache(hwloc_obj_type_t t)
{
    return t == HWLOC_OBJ_L1CACHE || t == HWLOC_OBJ_L2CACHE
        || t == HWLOC_OBJ_L3CACHE || t == HWLOC_OBJ_L4CACHE
        || t == HWLOC_OBJ_L5CACHE || t == HWLOC_OBJ_L1ICACHE;
}

/*
 * Walk up from logical CPU pu_idx and return the nearest data/unified cache at
 * the given depth (1=L1d, 2=L2, 3=L3).  Returns NULL if not found.
 */
static hwloc_obj_t find_cache(hwloc_topology_t topo,
                               unsigned int     pu_idx,
                               unsigned int     cache_depth)
{
    hwloc_obj_t pu = hwloc_get_obj_by_type(topo, HWLOC_OBJ_PU, pu_idx);
    if (!pu) return NULL;
    for (hwloc_obj_t obj = pu->parent; obj; obj = obj->parent) {
        if (!obj_is_cache(obj->type)) continue;
        if ((unsigned)obj->attr->cache.depth == cache_depth
            && obj->attr->cache.type != HWLOC_OBJ_CACHE_INSTRUCTION)
            return obj;
    }
    return NULL;
}

/* Returns true if the hwloc CORE object has an L3 cache ancestor. */
static bool hwloc_core_has_l3(hwloc_obj_t core)
{
    for (hwloc_obj_t o = core->parent; o; o = o->parent)
        if (o->type == HWLOC_OBJ_L3CACHE) return true;
    return false;
}

/*
 * Count physical P-cores, E-cores, LP E-cores, and Standard cores via hwloc.
 *
 * Strategy:
 *   - hwloc cpukinds are ordered lowest-to-highest efficiency.
 *   - Highest efficiency kind = P-cores (or Standard on non-hybrid).
 *   - Lower efficiency kind + L3 reachable = E-cores.
 *   - Lower efficiency kind + no L3        = LP E-cores.
 *   - 0 or 1 cpukind ? all Standard (non-hybrid).
 */
static void count_hwloc_core_types(hwloc_topology_t topo,
                                    size_t& pCores,
                                    size_t& eCores,
                                    size_t& lpECores,
                                    size_t& stdCores)
{
    pCores = eCores = lpECores = stdCores = 0;

    int nr_kinds = hwloc_cpukinds_get_nr(topo, 0);
    if (nr_kinds <= 1) {
        /* No meaningful kind distinction - all Standard. */
        stdCores = (size_t)hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_CORE);
        return;
    }

    /* Find the maximum efficiency value (P-cores have highest). */
    int max_eff = -1;
    for (int k = 0; k < nr_kinds; ++k) {
        int eff = -1;
        hwloc_cpukinds_get_info(topo, (unsigned)k, NULL, &eff, NULL, 0);
        if (eff > max_eff) max_eff = eff;
    }

    int nCores = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_CORE);
    for (int ci = 0; ci < nCores; ++ci) {
        hwloc_obj_t core = hwloc_get_obj_by_type(topo, HWLOC_OBJ_CORE, ci);
        if (!core) continue;

        int kind_idx = hwloc_cpukinds_get_by_cpuset(topo, core->cpuset, 0);
        int eff = -1;
        if (kind_idx >= 0)
            hwloc_cpukinds_get_info(topo, (unsigned)kind_idx, NULL, &eff, NULL, 0);

        if (eff == max_eff) {
            ++pCores;   /* highest efficiency = P-core */
        } else if (hwloc_core_has_l3(core)) {
            ++eCores;   /* lower efficiency + L3 = E-core */
        } else {
            ++lpECores; /* lower efficiency + no L3 = LP E-core */
        }
    }
}

/*
 * Count physical cores sharing the cache at cache_depth above logical CPU
 * pu_idx, according to hwloc.  Returns 0 if not present.
 */
static size_t hwloc_physical_cores_sharing(hwloc_topology_t topo,
                                            unsigned int     pu_idx,
                                            unsigned int     cache_depth)
{
    hwloc_obj_t hobj = find_cache(topo, pu_idx, cache_depth);
    if (!hobj) return 0;
    return (size_t)hwloc_get_nbobjs_inside_cpuset_by_type(
        topo, hobj->cpuset, HWLOC_OBJ_CORE);
}

// -----------------------------------------------------------------------------
// Section: core type counts
// -----------------------------------------------------------------------------

static void check_core_type_counts(const Xbyak::util::CpuTopology& xTopo,
                                    hwloc_topology_t                 hTopo)
{
    size_t xP, xE, xLP, xStd;
    size_t hP, hE, hLP, hStd;
    count_xbyak_core_types(xTopo, xP, xE, xLP, xStd);
    count_hwloc_core_types(hTopo, hP, hE, hLP, hStd);

    if (xTopo.isHybrid()) {
        check_eq("P-core count",
                 (unsigned long long)xP,  (unsigned long long)hP);
        check_eq("E-core count (includes LP E-cores in xbyak Efficient class)",
                 (unsigned long long)xE,  (unsigned long long)hE);
        check_eq("LP E-core count (Efficient + no L3)",
                 (unsigned long long)xLP, (unsigned long long)hLP);
    } else {
        check_eq("Standard core count",
                 (unsigned long long)xStd, (unsigned long long)hStd);
    }
}

// -----------------------------------------------------------------------------
// Section: cache sharing per core type
// -----------------------------------------------------------------------------

/*
 * For a representative physical core of each type, compare the number of
 * physical cores sharing each cache level between xbyak and hwloc.
 */
static void check_cache_sharing(const Xbyak::util::CpuTopology& xTopo,
                                 hwloc_topology_t                 hTopo)
{
    struct CacheLevel {
        const char*            name;
        Xbyak::util::CacheType xbyak_type;
        unsigned int           hwloc_depth;
    };
    static const CacheLevel levels[] = {
        { "L1d", Xbyak::util::L1d, 1 },
        { "L2",  Xbyak::util::L2,  2 },
        { "L3",  Xbyak::util::L3,  3 },
    };
    static const size_t NUM_LEVELS = sizeof(levels) / sizeof(levels[0]);

    /* Find first primary logical CPU for each core class. */
    int firstCpu[4] = { -1, -1, -1, -1 };
    const size_t n = xTopo.getLogicalCpuNum();
    for (size_t i = 0; i < n; ++i) {
        if (!is_primary_logical_cpu(xTopo, (uint32_t)i)) continue;
        int cls = (int)classify_xbyak_cpu(xTopo.getLogicalCpu(i));
        if (firstCpu[cls] < 0) firstCpu[cls] = (int)i;
    }

    struct Group { const char* label; XbyakCoreClass cls; };
    static const Group groups[] = {
        { "P-core",    XCC_PCORE    },
        { "E-core",    XCC_ECORE    },
        { "LP E-core", XCC_LP_ECORE },
        { "Standard",  XCC_STANDARD },
    };

    for (size_t g = 0; g < sizeof(groups)/sizeof(groups[0]); ++g) {
        int repCpu = firstCpu[(int)groups[g].cls];
        if (repCpu < 0) continue; /* core type not present */

        printf("\n  [ %s - representative CPU[%d] ]\n",
               groups[g].label, repCpu);

        for (size_t d = 0; d < NUM_LEVELS; ++d) {
            char label[80];
            snprintf(label, sizeof(label), "CPU[%d] %s %s - physical cores sharing",
                     repCpu, groups[g].label, levels[d].name);

            size_t xSh = xbyak_physical_cores_sharing(
                xTopo, (size_t)repCpu, levels[d].xbyak_type);
            size_t hSh = hwloc_physical_cores_sharing(
                hTopo, (unsigned)repCpu, levels[d].hwloc_depth);

            if (xSh == 0 && hSh == 0) {
                printf("  N/A   %-60s  (cache level not present)\n", label);
                continue;
            }
            check_eq(label, (unsigned long long)xSh, (unsigned long long)hSh);
        }
    }
}

// -----------------------------------------------------------------------------
// Section: per-CPU cache sizes
// -----------------------------------------------------------------------------

static void check_all_cpu_caches(const Xbyak::util::CpuTopology& xTopo,
                                  hwloc_topology_t                 hTopo)
{
    struct CacheLevel {
        const char*            name;
        Xbyak::util::CacheType xbyak_type;
        unsigned int           hwloc_depth;
    };
    static const CacheLevel levels[] = {
        { "L1d", Xbyak::util::L1d, 1 },
        { "L2",  Xbyak::util::L2,  2 },
        { "L3",  Xbyak::util::L3,  3 },
    };
    static const size_t NUM_LEVELS = sizeof(levels) / sizeof(levels[0]);

    const size_t numCpus = xTopo.getLogicalCpuNum();
    Xbyak::util::CoreType lastType = Xbyak::util::Unknown;

    for (size_t i = 0; i < numCpus; ++i) {
        const Xbyak::util::LogicalCpu& lcpu = xTopo.getLogicalCpu(i);

        if (lcpu.coreType != lastType) {
            printf("\n  [ %s cores ]\n", core_type_str(lcpu.coreType));
            lastType = lcpu.coreType;
        }

        for (size_t d = 0; d < NUM_LEVELS; ++d) {
            uint32_t    xSize = lcpu.cache[levels[d].xbyak_type].size;
            hwloc_obj_t hobj  = find_cache(hTopo, (unsigned)i, levels[d].hwloc_depth);

            char label[80];
            snprintf(label, sizeof(label), "CPU[%u] %s %s size (bytes)",
                     (unsigned)i, core_type_str(lcpu.coreType), levels[d].name);

            if (xSize == 0 && hobj == NULL) {
                printf("  N/A   %-60s  (not present)\n", label);
                continue;
            }
            if (xSize == 0) {
                printf("  SKIP  %-60s  (xbyak=0, hwloc=%u)\n",
                       label, (uint32_t)hobj->attr->cache.size);
                continue;
            }
            if (hobj == NULL) {
                printf("  SKIP  %-60s  (hwloc: not found, xbyak=%u)\n",
                       label, xSize);
                continue;
            }
            check_eq(label, (unsigned long long)xSize,
                             (unsigned long long)hobj->attr->cache.size);
        }
    }
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------

int main(void)
{
#if !defined(XBYAK_INTEL_CPU_SPECIFIC)
    printf("SKIP  xbyak CpuTopology is not supported on this architecture.\n");
    return 0;
#else

    printf("=== cpu-topology-test: xbyak vs hwloc ===\n\n");

    Xbyak::util::Cpu cpu;
    Xbyak::util::CpuTopology xTopo(cpu);

    hwloc_topology_t hTopo;
    if (hwloc_topology_init(&hTopo) < 0) {
        fprintf(stderr, "ERROR  hwloc_topology_init() failed\n");
        return 2;
    }
    if (hwloc_topology_load(hTopo) < 0) {
        fprintf(stderr, "ERROR  hwloc_topology_load() failed\n");
        hwloc_topology_destroy(hTopo);
        return 2;
    }

    const bool isHybrid = xTopo.isHybrid();
    printf("  Hybrid system : %s\n\n", isHybrid ? "yes" : "no");

    // -- 1. Logical CPU count --------------------------------------------------
    {
        size_t x = xTopo.getLogicalCpuNum();
        int    h = hwloc_get_nbobjs_by_type(hTopo, HWLOC_OBJ_PU);
        check_eq("Logical CPU (PU) count",
                 (unsigned long long)x, (unsigned long long)h);
    }

    // -- 2. Physical core count ------------------------------------------------
    {
        size_t x     = xTopo.getPhysicalCoreNum();
        int    h     = hwloc_get_nbobjs_by_type(hTopo, HWLOC_OBJ_CORE);
        int    hPkgs = hwloc_get_nbobjs_by_type(hTopo, HWLOC_OBJ_PACKAGE);
        if ((int)x != h && hPkgs > 1)
            warn_neq("Physical core count",
                     (unsigned long long)x, (unsigned long long)h,
                     "multi-socket: xbyak reports per-socket on Linux");
        else
            check_eq("Physical core count",
                     (unsigned long long)x, (unsigned long long)h);
    }

    // -- 3. Cache-line size ----------------------------------------------------
    {
        uint32_t    xLine = xTopo.getLineSize();
        hwloc_obj_t hl1   = find_cache(hTopo, 0, 1);
        if (hl1 && xLine > 0)
            check_eq("Cache-line size (bytes)",
                     (unsigned long long)xLine,
                     (unsigned long long)hl1->attr->cache.linesize);
        else
            printf("  SKIP  Cache-line size  (xbyak=%u, hwloc l1d %sfound)\n",
                   xLine, hl1 ? "" : "not ");
    }

    // -- 4. Core type counts ---------------------------------------------------
    printf("\n--- Core type counts ---\n");
    check_core_type_counts(xTopo, hTopo);

    // -- 5. Cache sharing per core type ----------------------------------------
    printf("\n--- Cache sharing per core type ---\n");
    check_cache_sharing(xTopo, hTopo);

    // -- 6. Per-CPU cache sizes ------------------------------------------------
    printf("\n--- Per-CPU cache sizes");
    if (isHybrid) printf(" (hybrid: each core type shown separately)");
    printf(" ---\n");
    check_all_cpu_caches(xTopo, hTopo);

    hwloc_topology_destroy(hTopo);

    printf("\n%d passed, %d failed, %d warnings\n",
           g_passes, g_failures, g_warnings);
    return (g_failures > 0) ? 1 : 0;

#endif
}
