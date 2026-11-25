/*
 * IOKit "quiet" at boot (docs/boot-perf.md, re-log 50).
 *
 * launchctl waits up to 5 s in IOKitWaitQuiet() before it loads the launch
 * daemons, until no IOService is busy. Two S5L drivers wait forever in their
 * start() for the iPod's PMU, which the 3DS doesn't have; their matching
 * threads keep the clkrstgen and wdt nubs busy, so every boot sat out the
 * whole 5 s timeout (ARM11 idle).
 *  - wdt: AppleARMWatchDogTimer waits for an "IOPMU" service only to read a
 *    debug property; kernel patch wdt-no-pmu-wait makes that wait return NULL
 *    (a case its code handles).
 *  - clkrstgen: AppleS5L8720XClockController waits for the PMU's
 *    "function-core_voltage_N" functions and must keep waiting: without them
 *    it panics ("Bad DPSM Tables"), and the platform already uses the
 *    half-started object. Kernel patch clkctl-voltage-quiet points the call
 *    at shim_quiet_clk_function: it takes the nub's matching off the busy
 *    count before the (endless) wait and puts it back if the wait ever ends.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "kpi.h"

#define KPI(addr, ret, ...) ((ret (*)(__VA_ARGS__))((addr) | 1))
/* IORegistryEntry::fromPath(path, plane, residualPath, residualLength, fromEntry) */
#define reg_from_path   KPI(IOS3DS_KPI_IOREGISTRYENTRY_FROMPATH, void *, const char *, const void *, char *, int *, void *)
/* IOService::adjustBusy(SInt32) */
#define svc_adjust_busy KPI(IOS3DS_KPI_IOSERVICE_ADJUSTBUSY, void, void *, long)
/* AppleARMPlatform: look up the platform function named by a device-tree
   property of the provider (waitForService on its phandle, no timeout) */
#define arm_function_lookup KPI(IOS3DS_KPI_ARM_FUNCTION_LOOKUP, void *, void *, const char *)

static void *clk_nub;

void *shim_quiet_clk_function(void *provider, const char *name)
{
    if (!clk_nub)
        clk_nub = reg_from_path("IODeviceTree:/arm-io/clkrstgen", 0, 0, 0, 0);   /* kept: lives forever */
    if (clk_nub) svc_adjust_busy(clk_nub, -1);
    void *fn = arm_function_lookup(provider, name);
    if (clk_nub) svc_adjust_busy(clk_nub, 1);
    return fn;
}
