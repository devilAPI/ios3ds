# Per-version kernel patch sets

`patches.json` and `patches-*.json` in the parent directory are the 8C148
(4.2.1) recipes with fixed virtual addresses. They are the default and are
never edited by another version's build.

`4.1.json` is the 8B117 (iOS 4.1) set. It was derived from the 8C148 recipes by
checking every original byte sequence in the 8B117 kernelcache:

- `same-va`: the instruction is at the same address;
- `context-N`: 8–256 bytes of surrounding code from 8C148 occur exactly once
  in 8B117, which gives the new address;
- `anchor`: for short/generic instruction sequences, the address was derived
  from a nearby uniquely located function (UART TX data at 0x801dc4bc, CLCD
  backlight log at 0x80758408, memdev strategy's following code at 0x80072bf8)
  and the original bytes were checked at the result;
- `unique-orig`: the original bytes occur once in 8B117.

Every entry keeps the strict original-byte check, so a wrong kernelcache still
stops the build. The clock voltage lookup and RTC wait literal were relocated
in re-log 131; gyro usage/dispatch and CLCD backlight log-enable followed in
the NEXT re-log section. CLCD backlight retry-off is absent on purpose: 8B117
has none of the `+0xea0` pending-flag retry path that 8C148's
`safe_enable_backlight` has. See `docs/multi-version.md` for the history. No Apple code is stored
here beyond the same short verification patterns already used by 8C148's
recipes.

`3.1.3.json` (7E18) and `2.2.1.json` (5H11a) are the Darwin 10.0 / Darwin 9
sets. Even though those kernelcaches are linked at `0xC0000000` instead of
`0x80000000`, their patches are still keyed by original bytes: a `va` entry is
the relocated address in *that* cache, and `find` entries use unique
surrounding context. Where the instruction itself changed (three 7E18/5H11a
timer hooks, the UART init/ready functions, `_fleh_irq`, `mdevstrategy` and
`commpage_set_timestamp`), the recipe carries the verified target bytes and a
target-specific replacement. Patches that could not be located uniquely were
left out; the exact lists and reasons are in `docs/multi-version.md` §6.4.
