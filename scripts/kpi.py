#!/usr/bin/env python3
"""Resolve the kernel KPI addresses the ARM11 shim calls into by symbol name.

The shim is plain C/assembly linked at a fixed VA; it calls a handful of
XNU/IOKit functions directly. Their addresses move with every iOS build, so
hard-coding 8C148's addresses silently breaks another firmware. This module
looks the symbols up in the staged kernelcache's LC_SYMTAB and writes a small
C header into build/ (never committed). The 4.2.1 defaults in kernel/shim/kpi.h
are the values this generates for the default firmware, so its shim.bin stays
byte-identical.
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from fwlib.macho import MachO  # noqa: E402

# macro -> (symbol, 4.2.1 fallback). C++ names are mangled in LC_SYMTAB.
SYMBOLS = {
    "BUF_COUNT": ("_buf_count", 0x800781c0),
    "BUF_SETRESID": ("_buf_setresid", 0x800781d4),
    "BUF_BLKNO": ("_buf_blkno", 0x800782a4),
    "BUF_SETERROR": ("_buf_seterror", 0x80078158),
    "BUF_BIODONE": ("_buf_biodone", 0x800793f4),
    "BUF_MAP": ("_buf_map", 0x800789a0),
    "BUF_UNMAP": ("_buf_unmap", 0x800789f2),
    "BUF_FLAGS": ("_buf_flags", 0x80078198),
    "LCK_GRP_ATTR_ALLOC_INIT": ("_lck_grp_attr_alloc_init", 0x8001ae22),
    "LCK_GRP_ALLOC_INIT": ("_lck_grp_alloc_init", 0x8001ae00),
    "LCK_ATTR_ALLOC_INIT": ("_lck_attr_alloc_init", 0x8001ade8),
    "LCK_MTX_ALLOC_INIT": ("_lck_mtx_alloc_init", 0x80062008),
    "LCK_MTX_LOCK": ("_lck_mtx_lock", 0x80063000),
    "LCK_MTX_UNLOCK": ("_lck_mtx_unlock", 0x800631a0),
    "ASSERT_WAIT_TIMEOUT": ("_assert_wait_timeout", 0x8001f02a),
    "THREAD_BLOCK": ("_thread_block", 0x8001edcc),
    "PANIC": ("_panic", 0x80015c7c),
    "SYSCTL_REGISTER_OID": ("_sysctl_register_oid", 0x80159c54),
    "SYSCTL_HANDLE_OPAQUE": ("_sysctl_handle_opaque", 0x80159242),
    "SYSCTL__CHILDREN": ("_sysctl__children", 0x8027f7a8),
    "COPYIN": ("_copyin", 0x80067c84),
    "COPYOUT": ("_copyout", 0x80067d04),
    "IOMALLOCCONTIGUOUS": ("_IOMallocContiguous", 0x801a70e0),
    "IOFREECONTIGUOUS": ("_IOFreeContiguous", 0x801a727c),
    "ML_IO_MAP": ("_ml_io_map", 0x80067780),
    "IODELAY": ("_IODelay", 0x801a6c2c),
    "IOSLEEP": ("_IOSleep", 0x801a6c40),
    "PRINTF": ("_printf", 0x8001c4c4),
    "PROC_SELFPID": ("_proc_selfpid", 0x8014f1be),
    "PROC_FIND": ("_proc_find", 0x8014f94c),
    "PROC_RELE": ("_proc_rele", 0x8014f862),
    "IOREGISTRYENTRY_FROMPATH":
        ("__ZN15IORegistryEntry8fromPathEPKcPK15IORegistryPlanePcPiPS_", 0x801aae60),
    "IOSERVICE_ADJUSTBUSY": ("__ZN9IOService10adjustBusyEl", 0x801aef24),
    "SYSCTL__KERN_CHILDREN": ("_sysctl__kern_children", 0x8027f7b8),
    "IOLOG": ("_IOLog", 0x801a6be8),
    "IO_WAITFORSERVICE": ("__ZN9IOService14waitForServiceEP12OSDictionaryP13mach_timespec", 0x801b18f0),
    "IO_GETPLATFORM": ("__ZN9IOService11getPlatformEv", 0x801ac744),
    "IO_GETRESOURCESERVICE": ("__ZN9IOService18getResourceServiceEv", 0x801ac75c),
    "IO_ATTACH": ("__ZN9IOService6attachEPS_", 0x801adc54),
    "IO_REGISTERSERVICE": ("__ZN9IOService15registerServiceEm", 0x801b0844),
    "PM_POWERSOURCE": ("__ZN15IOPMPowerSource11powerSourceEv", 0x801bb624),
    "PM_SETMAXCAPACITY": ("__ZN15IOPMPowerSource14setMaxCapacityEj", 0x801bb3d0),
    "PM_SETCURRENTCAPACITY": ("__ZN15IOPMPowerSource18setCurrentCapacityEj", 0x801bb3fc),
    "PM_SETISCHARGING": ("__ZN15IOPMPowerSource13setIsChargingEb", 0x801bb1f8),
    "PM_SETEXTERNALCONNECTED": ("__ZN15IOPMPowerSource20setExternalConnectedEb", 0x801bb288),
    "PM_SETEXTERNALCHARGE_CAPABLE":
        ("__ZN15IOPMPowerSource24setExternalChargeCapableEb", 0x801bb258),
    "PM_SETBATTERYINSTALLED": ("__ZN15IOPMPowerSource19setBatteryInstalledEb", 0x801bb228),
    "PM_SETVOLTAGE": ("__ZN15IOPMPowerSource10setVoltageEj", 0x801bb348),
    "PM_SETPSProperty": ("__ZN15IOPMPowerSource13setPSPropertyEPK8OSSymbolP8OSObject", 0x801bb0dc),
    "PM_UPDATESTATUS": ("__ZN15IOPMPowerSource12updateStatusEv", 0x801bb428),
    "OSSYMBOL_WITHCSTRINGNOCOPY": ("__ZN8OSSymbol17withCStringNoCopyEPKc", 0x8019a5d0),
    "OSNUMBER_WITHNUMBER": ("__ZN8OSNumber10withNumberEyj", 0x80197684),
    "KOSBOOLEAN_TRUE": ("_kOSBooleanTrue", 0x80232e4c),
    "KOSBOOLEAN_FALSE": ("_kOSBooleanFalse", 0x80232e50),
    "OSDATA_GETBYTESNOCOPY": ("__ZNK6OSData14getBytesNoCopyEv", 0x8018d534),
    "OSDATA_GETLENGTH": ("__ZNK6OSData9getLengthEv", 0x8018d510),
}

# Data symbols with no LC_SYMTAB entry: gIOPlatform is a private global of
# AppleARMPE. The address is the literal PEGetGMTTimeOfDay loads: 8C148
# 0x801c84f0 -> 0x8029d490, 8B117 -> 0x8029c49c, 7E18 -> 0xc025f53c,
# 5H11a -> 0xc02064a0.
LITERALS = {"GIO_PLATFORM": (0x8029d490, {"8B117": 0x8029c49c,
                                            "7E18": 0xc025f53c,
                                            "5H11a": 0xc02064a0})}


def layout_consts(staging):
    """Virtual base and the kernel IRQ continuation for this firmware.

    The shim adds its own L1 map to the kernel page tables using VIRT_BASE and
    returns from shim_irq to the instruction after the two patched ones. Both
    values are per-build; 4.x keeps the original constants.
    """
    import versions
    build = _build(staging)
    info = next((v for v in versions.VERSIONS.values() if v["build"] == build), {})
    return (info.get("virt_base", 0x80000000), info.get("irq_cont", 0x80066494))


def _build(staging):
    import json
    manifest = os.path.join(staging, "manifest.json")
    if os.path.exists(manifest):
        with open(manifest) as f:
            return json.load(f).get("source", {}).get("build")
    return None


def resolve(staging):
    """Return macro -> address for every symbol found in this kernelcache."""
    path = os.path.join(staging, "kernelcache.macho")
    blob = open(path, "rb").read()
    mo = MachO(blob)
    symoff, nsyms, stroff, _ = mo.symtab
    found = {}
    for i in range(nsyms):
        strx, typ, sect, desc, val = struct.unpack_from("<IBBhI", blob, symoff + 12 * i)
        if not val:
            continue
        name = blob[stroff + strx:blob.index(b"\0", stroff + strx)].decode(errors="replace")
        found.setdefault(name, val)
    out = {}
    for macro, (sym, fallback) in SYMBOLS.items():
        out[macro] = found.get(sym, fallback)
    build = _build(staging)
    for macro, (fallback, overrides) in LITERALS.items():
        out[macro] = overrides.get(build, fallback)
    out["VIRT_BASE"], out["KERNEL_IRQ_CONT"] = layout_consts(staging)
    import versions
    info = next((v for v in versions.VERSIONS.values() if v["build"] == build), {})
    if info.get("irq_spsr"):
        out["KERNEL_IRQ_SPSR"] = 1
    return out


def write_header(staging, outdir):
    path = os.path.join(outdir, "ios3ds-kpi.h")
    if not os.path.exists(os.path.join(staging, "kernelcache.macho")):
        # No Apple kernelcache (CI never has one): leave the header empty so
        # kernel/shim/kpi.h's defaults apply, which are 4.2.1's addresses.
        with open(path, "w") as f:
            f.write("/* Generated by scripts/kpi.py: no kernelcache in %s, kpi.h defaults "
                    "(iOS 4.2.1) apply. */\n" % staging)
        return path
    addrs = resolve(staging)
    with open(path, "w") as f:
        f.write("/* Generated by scripts/kpi.py from %s; do not commit. */\n" % staging)
        for macro, addr in sorted(addrs.items()):
            f.write("#define IOS3DS_KPI_%s 0x%08x\n" % (macro, addr))
    return path


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("staging", nargs="?", default=os.path.join(ROOT, "build", "staging"))
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "build"))
    a = ap.parse_args()
    print(write_header(a.staging, a.out))


if __name__ == "__main__":
    main()
