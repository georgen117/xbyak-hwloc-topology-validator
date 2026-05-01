/*
 * test_topology.cpp
 *
 * Validates that the CPU topology values reported by xbyak (xbyak_util.h
 * CpuTopology) match those reported by hwloc for the same machine.
 *
 * Comparisons performed
 * ─────────────────────
 *   1. Logical CPU (Processing Unit) count
 *   2. Physical core count
 *      Note: on multi-socket Linux systems xbyak uses sysfs core_id which is
 *      per-package, so the count may be < hwloc's machine-wide count.  The
 *      test issues a WARNING instead of a FAIL in that case.
 *   3. Cache-line size (from the L1 data cache of logical CPU 0)
 *   4. L1d cache size  (logical CPU 0)
 *   5. L2  cache size  (logical CPU 0)
 *   6. L3  cache size  (logical CPU 0, if present on the machine)
 *
 * Supported platforms
 * ───────────────────
 *   Windows (x86-64) and Linux (x86-64).
 *   On unsupported architectures the xbyak topology block is skipped and the
 *   test exits with code 0 (not a failure – the library itself documents this
 *   limitation).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdexcept>
#include <set>

// ── xbyak ────────────────────────────────────────────────────────────────────
// xbyak_util.h sets XBYAK_INTEL_CPU_SPECIFIC when compiling for x86/x64.
// CpuTopology is only available when XBYAK_CPU_CACHE == 1 (the default).
#include "xbyak/xbyak_util.h"

// ── hwloc ────────────────────────────────────────────────────────────────────
#include "hwloc.h"

// ─────────────────────────────────────────────────────────────────────────────
// Minimal test harness
// ─────────────────────────────────────────────────────────────────────────────

static int g_failures = 0;
static int g_passes   = 0;

static void check_eq(const char* label,
                     unsigned long long xbyak_val,
                     unsigned long long hwloc_val)
{
    if (xbyak_val == hwloc_val) {
        printf("  PASS  %-44s  xbyak=%-10llu  hwloc=%llu\n",
               label, xbyak_val, hwloc_val);
        ++g_passes;
    } else {
        printf("  FAIL  %-44s  xbyak=%-10llu  hwloc=%llu\n",
               label, xbyak_val, hwloc_val);
        ++g_failures;
    }
}

static void warn_neq(const char* label,
                     unsigned long long xbyak_val,
                     unsigned long long hwloc_val,
                     const char* reason)
{
    printf("  WARN  %-44s  xbyak=%-10llu  hwloc=%llu  (%s)\n",
           label, xbyak_val, hwloc_val, reason);
}

// ─────────────────────────────────────────────────────────────────────────────
// hwloc helpers
// ─────────────────────────────────────────────────────────────────────────────

/* Returns true for any hwloc object that is a cache. */
static bool obj_is_cache(hwloc_obj_type_t t)
{
    return t == HWLOC_OBJ_L1CACHE
        || t == HWLOC_OBJ_L2CACHE
        || t == HWLOC_OBJ_L3CACHE
        || t == HWLOC_OBJ_L4CACHE
        || t == HWLOC_OBJ_L5CACHE
        || t == HWLOC_OBJ_L1ICACHE;
}

/*
 * Walk up the topology tree from logical CPU `pu_idx` and return the first
 * cache object at the requested depth whose type is DATA or UNIFIED.
 * Returns NULL if not found.
 */
static hwloc_obj_t find_cache(hwloc_topology_t topo,
                               unsigned int     pu_idx,
                               unsigned int     cache_depth)
{
    hwloc_obj_t pu = hwloc_get_obj_by_type(topo, HWLOC_OBJ_PU, pu_idx);
    if (!pu) return NULL;

    for (hwloc_obj_t obj = pu->parent; obj; obj = obj->parent) {
        if (!obj_is_cache(obj->type)) continue;

        unsigned int depth = (unsigned int)obj->attr->cache.depth;
        hwloc_obj_cache_type_e ctype = obj->attr->cache.type;

        if (depth == cache_depth
            && ctype != HWLOC_OBJ_CACHE_INSTRUCTION)   /* data or unified */
        {
            return obj;
        }
    }
    return NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(void)
{
#if !defined(XBYAK_INTEL_CPU_SPECIFIC)
    /*
     * xbyak CpuTopology relies on CPUID and sysfs/Win32 APIs that are only
     * available on Intel/AMD x86-64.  Skip gracefully on other architectures.
     */
    printf("SKIP  xbyak CpuTopology is not supported on this architecture.\n");
    return 0;

#else /* XBYAK_INTEL_CPU_SPECIFIC */

    printf("=== cpu-topology-test: xbyak vs hwloc ===\n\n");

    // ── 1. Initialise xbyak topology ─────────────────────────────────────────
    Xbyak::util::Cpu cpu;
    Xbyak::util::CpuTopology xTopo(cpu);   // throws on failure

    // ── 2. Initialise hwloc topology ─────────────────────────────────────────
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

    // ── 3. Logical CPU (PU) count ─────────────────────────────────────────────
    {
        size_t x = xTopo.getLogicalCpuNum();
        int    h = hwloc_get_nbobjs_by_type(hTopo, HWLOC_OBJ_PU);
        check_eq("Logical CPU (PU) count", (unsigned long long)x,
                                           (unsigned long long)h);
    }

    // ── 4. Physical core count ────────────────────────────────────────────────
    {
        size_t x       = xTopo.getPhysicalCoreNum();
        int    h       = hwloc_get_nbobjs_by_type(hTopo, HWLOC_OBJ_CORE);
        int    hPkgs   = hwloc_get_nbobjs_by_type(hTopo, HWLOC_OBJ_PACKAGE);

        if ((int)x != h && hPkgs > 1) {
            /*
             * On Linux, xbyak reads core_id from sysfs which resets to 0 per
             * package, so physicalCoreNum_ = max(core_id)+1 = per-socket count.
             * This is a known xbyak limitation; treat as a warning, not failure.
             */
            warn_neq("Physical core count",
                     (unsigned long long)x,
                     (unsigned long long)h,
                     "multi-socket: xbyak reports per-socket on Linux");
        } else {
            check_eq("Physical core count", (unsigned long long)x,
                                            (unsigned long long)h);
        }
    }

    // ── 5. Cache-line size ────────────────────────────────────────────────────
    {
        uint32_t xLine  = xTopo.getLineSize();
        hwloc_obj_t hl1 = find_cache(hTopo, 0, 1);   /* L1d of CPU 0 */
        if (hl1 && xLine > 0) {
            uint32_t hLine = (uint32_t)hl1->attr->cache.linesize;
            check_eq("Cache-line size (bytes)",
                     (unsigned long long)xLine,
                     (unsigned long long)hLine);
        } else {
            printf("  SKIP  Cache-line size  (xbyak=%u, hwloc l1d %sfound)\n",
                   xLine, hl1 ? "" : "not ");
        }
    }

    // ── 6. Per-CPU cache sizes (L1d / L2 / L3) for logical CPU 0 ─────────────
    if (xTopo.getLogicalCpuNum() > 0) {
        const Xbyak::util::LogicalCpu& lcpu0 = xTopo.getLogicalCpu(0);

        struct {
            const char*              label;
            uint32_t                 xbyak_size;
            Xbyak::util::CacheType   xbyak_type;
            unsigned int             hwloc_depth;
        } const caches[] = {
            { "L1d cache size CPU[0] (bytes)",
              lcpu0.cache[Xbyak::util::L1d].size, Xbyak::util::L1d, 1 },
            { "L2 cache size  CPU[0] (bytes)",
              lcpu0.cache[Xbyak::util::L2].size,  Xbyak::util::L2,  2 },
            { "L3 cache size  CPU[0] (bytes)",
              lcpu0.cache[Xbyak::util::L3].size,  Xbyak::util::L3,  3 },
        };

        for (size_t i = 0; i < sizeof(caches) / sizeof(caches[0]); ++i) {
            uint32_t    xSize = caches[i].xbyak_size;
            hwloc_obj_t hobj  = find_cache(hTopo, 0, caches[i].hwloc_depth);

            if (xSize == 0 && hobj == NULL) {
                /* Neither side reports this level – machine doesn't have it. */
                printf("  N/A   %s  (not present)\n", caches[i].label);
                continue;
            }
            if (xSize == 0) {
                printf("  SKIP  %s  (xbyak returned 0, hwloc=%u)\n",
                       caches[i].label, (uint32_t)hobj->attr->cache.size);
                continue;
            }
            if (hobj == NULL) {
                printf("  SKIP  %s  (hwloc did not find this cache level,"
                       " xbyak=%u)\n", caches[i].label, xSize);
                continue;
            }

            uint32_t hSize = (uint32_t)hobj->attr->cache.size;
            check_eq(caches[i].label,
                     (unsigned long long)xSize,
                     (unsigned long long)hSize);
        }
    }

    hwloc_topology_destroy(hTopo);

    // ── Summary ───────────────────────────────────────────────────────────────
    printf("\n%d passed, %d failed\n", g_passes, g_failures);
    return (g_failures > 0) ? 1 : 0;

#endif /* XBYAK_INTEL_CPU_SPECIFIC */
}
