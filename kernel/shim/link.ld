/* ios3ds kernel shim: linked at its kernel VA (PA 0x20840000 via physmap).
   Two parts (scripts/mkboot.py splits them into two files):
     main  0x80840000  everything else         -> shim.bin
     GPU   0x808B0000  objects named *pica*.o, *cam*.o (camera, docs/camera.md) and *kif*.o (iOS 2.x en0) -> gpu.bin (loaded with load=,
           bss included as zeros); see boot/common/gpu_mirror.h */
OUTPUT_ARCH(arm)
ENTRY(_shim_base)
SECTIONS
{
    . = 0x80840000;
    .text : { *(EXCLUDE_FILE(*pica*.o *cam*.o *kif*.o) .text EXCLUDE_FILE(*pica*.o *cam*.o *kif*.o) .text*) }
    .rodata : { *(EXCLUDE_FILE(*pica*.o *cam*.o *kif*.o) .rodata*) }
    /* .bss lives inside .data so that objcopy -O binary writes it into shim.bin
       as zeros: the loader copies shim.bin and nothing else, and a separate
       NOBITS .bss was left with whatever RAM held. Zero on the Old 3DS and in
       QEMU by luck; on a New 2DS XL the md0 lock pointer read 0xa5ff7910 and
       lck_mtx_lock faulted (issue #1). */
    .data : { *(EXCLUDE_FILE(*pica*.o *cam*.o *kif*.o) .data*) *(EXCLUDE_FILE(*pica*.o *cam*.o *kif*.o) .bss* EXCLUDE_FILE(*pica*.o *cam*.o *kif*.o) COMMON) }
    __shim_end = .;
    ASSERT(__shim_end <= 0x80845F00, "shim code overlaps the probe snapshot (+0x5F00) / block mailbox (+0x6000)")

    . = 0x808B0000;
    .gpu : {
        KEEP(*pica*.o(.gpuhdr))
        *pica*.o(.text .text* .rodata* .data*)
        *cam*.o(.text .text* .rodata* .data*)
        *kif*.o(.text .text* .rodata* .data*)
        *pica*.o(.bss* COMMON)
        *cam*.o(.bss* COMMON)
        *kif*.o(.bss* COMMON)
        . = ALIGN(16);
    }
    __gpu_end = .;
    ASSERT(__gpu_end <= 0x808B8000, "shim GPU part overlaps the gpumirror blob (0x808B8000)")
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
