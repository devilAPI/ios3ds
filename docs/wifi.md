# Wi-Fi: networking for iOS on the Old 3DS

Status: **works end to end in QEMU, never run on hardware.** In the
`nintendo3ds` machine the unchanged ARM9 driver brings up an emulated AR6014
over SDIO, associates with an emulated WPA2 access point, and iOS gets a DHCP
lease, a default route and DNS, and fetches a page from the host (section 7,
`emu/wifi_test.py`). iOS's own Settings > Wi-Fi scans and joins through the
touch screen and the status bar shows the bars (section 10,
`emu/wifi_ui_test.py`). The model is written from ath6kl, dsiwifi, GBATEK and
IEEE 802.11i, not from our driver; what it cannot prove, and the hardware
assumptions that are still open, are in sections 6 and 7.2. Host tests:
`make -C tools/ios3ds-wifi test` (also run by `make test`).

```
 AR6014 (SDIO)        ARM9 monitor                     XNU shim              iOS
 -------------        ------------                     --------              ---
 TMIO ctrl 2   <--->  boot/arm9/wifi/ar6k.c   netmbox_t   kern.ios3ds_net    ios3ds-wifid
 0x10122000           BMI fw upload, HTC/WMI  @0x208A0000 (RW opaque sysctl,  <-> IOUserEthernet "en0"
 (polled)             WPA2 4-way (wpa2.c)     16+16 slots  tools/ios3ds-wifi/      <-> configd DHCP
                      802.3 <-> WMI framing   x 1536 B     shim/net.c)            (ipconfig set en0 DHCP)
```

## 1. Verified facts

| Fact | Source |
|---|---|
| Wi-Fi SDIO controller at `0x10122000` (TMIO "controller 2") is mapped for **both** ARM11 and ARM9 | 3dbrew "IO Registers" (`A11/A9`, "WIFI SDIO bus registers", used by NWM) |
| The ARM9 interrupt controller has no line for it (only `SDIO_1`, `SDIO_3`): the ARM9 must poll | 3dbrew "IRQ Registers"; libn3ds `tmio.h` (ARM9 sees controllers 1/3, ARM11 sees 2/3) |
| `CONFIG11` (0x10140000) and `GPIO` (0x10147000) are mapped for the ARM9 too | 3dbrew "IO Registers" |
| Power-up: `CFG11_WIFICNT` (0x10140180) = 1, GPIO 2_2 (`0x10147014` bit 0) = 0 selects the SDIO chip, GPIO 3_12 (`0x10147028` bit 0) = 1 releases Atheros nRESET, then ~2 ms | libn3ds `arm11/drivers/wifi/wifi.c`, `gpio.h` |
| AR6014: chip id `0D000001h` at `[0040ECh]`, BMI ROM version `2300006Fh`, host interest area `00520000h`, BMI_DONE entry `927000h` | GBATEK "DSi Atheros Wifi" chapters; dsiwifi `wifi_card.twl.c` |
| BMI register setup before upload (`hi_app_host_interest`=2, scratch0 \|= 8, `WLAN_SYSTEM_SLEEP` \|= 1, clock control = 5, `[4020h]`=0, `hi_mbox_io_block_sz`=80h, `hi+74h`=63h) | dsiwifi `wifi_card_wlan_init` |
| MBOX packet headers, data frame layout (`00 1C`/rssi + dst + src + BE length + LLC/SNAP), trailer/lookahead | GBATEK "MBOX Transfer Headers"; dsiwifi `wifi_card_mbox0_readpkt`, `data_send_*` |
| HTC service connect list (5 services) + SETUP_COMPLETE, `INT_STATUS_ENABLE` = `010300D1h` | dsiwifi `htc_handle_pkt` |
| WMI_CONNECT layout and DSi/3DS enum values (auth 5 = WPA2-PSK, crypto 1/3/4 = none/TKIP/AES); TKIP keys need the MIC halves swapped | GBATEK "WMI Connect Functions"; dsiwifi `wmi_connect_cmd`, `wmi_add_cipher_key` |
| Firmware lives in NWM's .code as six blocks; Type1 main of newer NWM may hang, Type4 works for normal internet with heartbeat timeout 0 and event 1025h ignored | GBATEK "3DS Files - Module NWM" |
| `IOUserEthernet.kext` is prelinked (0x80512000), publishes `IOUserEthernetResource`, requires entitlement `com.apple.networking.ethernet.user-access` | kernelcache: `scripts/kexts.py`, strings |
| IOKit.framework exports `IOEthernetControllerCreate/ReadPacket/WritePacket/SetLinkStatus/GetIONetworkInterfaceObject`, `kIOEthernetHardwareAddress`; `ReadPacket` = `recv(CFSocketGetNative(*(ref+0x28)), buf, len, 0)` | dyld cache (8C148), disassembly |
| `/usr/sbin/ipconfig`, `scutil`, configd with `IPConfiguration`/`IPMonitor` bundles and `mDNSResponder` are in the stock root fs and not disabled by `rootfs/recipe.json` | `build/tools/hfsplus ... ls` |

Not verified (needs a console): that ARM9 writes to `CFG11_WIFICNT`/GPIO
really take effect, SDIO timing at HCLK/4 from the ARM9, that WMI_CONNECT
with our scan result associates with every AP, and the IOUserEthernet path
end-to-end.

## 2. ARM9 side (`boot/arm9/wifi/`)

| File | Role |
|---|---|
| `sdio.c` | Polled TMIO host: CMD5/3/7 enumeration, 4-bit bus, block size 128, CMD52, CMD53 block mode and byte mode (FIFO32). Register usage and command encoding follow dsiwifi `wifi_sdio.twl.c`; every wait is bounded. |
| `ar6k.c` | State machine, one bounded step per monitor-loop pass: POWER -> BMI -> FWLOAD (one 256-byte chunk per pass) -> BOOT (`hi_board_data_initialized`, 3 s) -> HTC (HTC_READY, 5 s) -> WMI (WMI_READY, 5 s) -> SCAN (10 s) -> AUTH -> UP. Timeouts use the monitor's clock (timers 2+3). After "SDIO up" a FIFO self-test (CMD52 pattern into the function-1 scratch registers, read back with one CMD53) decides between CMD53 block transfers and a CMD52-per-byte fallback for the mailbox. The WMI and data endpoints are the ones the firmware returns in its CONNECT_SERVICE responses. HTC credits from HTC_READY are split per endpoint; each message takes `ceil(padded_len / cred_sz)`, reports in RX trailers return them, a failed SDIO write returns them immediately, data frames stay queued when an endpoint is empty, and only bring-up/WMI control waits for a report, for at most ~20 ms in one monitor step; packets read during that wait go into a 16-entry deferred ring (a full ring stops the drain, nothing is dropped); control messages that still get BUSY wait in an 8-entry FIFO, and EAPOL replies are data frames retried from a one-entry queue. |
| `wpa2.c` | SHA-1, HMAC, PBKDF2, 802.11i PRF, AES-128, RFC 3394 unwrap, EAPOL-Key 4-way and group handshake (CCMP or WPA v1 TKIP pairwise; CCMP or TKIP group). WPA v1 adds MD5/HMAC-MD5, RC4-drop-256 for Key Data and a 64-byte TKIP PTK. Written from the standards. An EAPOL frame too long for the WPA v1 MIC scratch buffer is rejected with `WPA2_ERR_LONG` instead of comparing uninitialised data. |
| `wifi_frame.c` | Ethernet II <-> WMI data frames (LLC/SNAP RFC 1042 and bridge-tunnel), and the HTC RX-trailer credit parser (`wifi_htc_credits`). |
| `wifi_ring.c` | ARM9 half of the mailbox. |
| `wifi_cfg.c` | `sd:/ios3ds/wifi.cfg` parser. |
| `nwmcode.c` | NWM `.code` -> firmware container at boot (section 3): in-place BLZ, GBATEK's literal pools, fixed block sizes. Same logic as `nwmfw.py`; the host tests compare both byte for byte. |
| `wifi.c` | `wifi_init()` (before the XNU jump: config, PBKDF2, mailbox init) and `wifi_poll()` (each monitor loop pass: driver step, up to 4 frames each way). Loopback backend. |
| `vnic.c` | Backend for a paravirtual QEMU NIC that was never built (section 7); `backend=qemu` fails cleanly. |

Connect sequence after WMI_READY: error-report bitmask 7Fh, heartbeat timeout
0, BSS filter "all", probed SSID (hidden networks), START_SCAN. BSSINFO events
for our SSID give channel, BSSID and the RSN group cipher; on SCAN_COMPLETE
(or after 10 s) we send power mode 2, disconnect timeout 5, keepalive 0 and
WMI_CONNECT. If the SSID was not seen, channel and BSSID stay 0 and the
firmware searches by itself (unverified on this firmware). After
WMI_CONNECT_EVENT the supplicant answers EAPOL-Key frames (sent as
`00 1C`/SNAP data frames, mailbox control word 0), installs PTK and GTK with
WMI_ADD_CIPHER_KEY and sends WMI_SYNCHRONIZE; then frames flow. A disconnect
event (logged with its WMI reason and 802.11 code) brings the link down and
reconnects after 5 s; the third failed handshake in a row logs "is psk= in
wifi.cfg right?".

The nonce source is the ARM9 PRNG register, a timer and SDIO status hashed
with SHA-1 (no TRNG on the ARM9).

### wifi.cfg

```
ssid=My Network          # 1-32 bytes, taken verbatim
psk=correct horse        # 8-63 chars, or 64 hex digits (= PMK, skips PBKDF2)
security=wpa2            # or open (default: wpa2 if psk given)
backend=auto             # auto|ar6014|qemu|loop|off
fw=0:/ios3ds/wifi-fw.bin
verbose=0                # 1 = state changes, 2 = every frame
diag=1                   # [wifi-diag] bring-up log (section 6), default on
fwmain=4                 # main firmware cut from nwm.code: 4 (default) or 1
mac=02:..                # qemu/loop backends only
nettest=http://10.0.2.2:8080/ example.com   # optional iOS self-test (section 5)
```

No file: the chip still comes up when `wifi-fw.bin` or `nwm.code` is there and waits idle
for iOS (Settings > Wi-Fi, section 10); without the firmware too, networking
is off and the mailbox reports backend NONE.

## 3. Firmware (not shipped)

The AR6014 ROM cannot run Wi-Fi by itself; Nintendo's Xtensa firmware must be
uploaded after every power-up. ios3ds never contains it. It comes from
**your own console**, once, as `sd:/ios3ds/nwm.code` (NWM's ExeFS `.code`;
the loader below cuts the firmware out of it at boot).

### 3.1 Automatic: the launcher (`tools/ios3ds-launcher`)

Start `ios3ds` from the Homebrew Launcher. If neither `sd:/ios3ds/nwm.code`
nor `sd:/ios3ds/wifi-fw.bin` exists, the launcher reads NWM's `.code`
through Horizon's FS service and writes it, before the usual "A: launch"
screen:

    Wi-Fi firmware: reading NWM .code
    from this console (read-only)...
      <k> / <k> KiB via FSPXI
    Wrote /ios3ds/nwm.code
    (<n> bytes). Keep it private.

Once the file exists this step is skipped. Any failure
is one red line (`Wi-Fi fw: cannot open NWM .code (FS xxxxxxxx, FSPXI
xxxxxxxx): Wi-Fi off`, `unexpected size`, `read failed`, `SD write failed`,
...), nothing is left on the card and the launch continues as before (Wi-Fi
stays off; use 3.2).

How it reads (`fetch_nwm_code()` in `tools/ios3ds-launcher/main.c`):

- Archive `0x2345678A` (`ARCHIVE_SAVEDATA_AND_CONTENT`, "title access"),
  binary archive lowpath (16 bytes) `{0x00002D02, 0x00040130, MEDIATYPE_NAND
  (0), 0}`: title id low/high word, media type, padding. Same layout as
  libctru `romfsMountFromTitle()` (`source/romfs_dev.c`), FBI
  (`source/fbi/task/listtitles.c`, `action/extractsmdh.c`) and Checkpoint
  (`3ds/source/smdh.cpp`).
- Binary file lowpath (20 bytes) `{0, 0, 1, ".cod", "e\0\0\0"}`: title
  access type 0 (high-level NCCH), TMD content index 0, NCCH access type 1
  (`exefs:/.code`), 8-byte ExeFS name. 3dbrew "Filesystem services" (Title
  Access Archive Path Data Format); FBI and Checkpoint use the same 20-byte
  form with type 2 for `exefs:/icon` (`{0, 0, 2, "icon", 0}`), libctru with
  type 0 for RomFS (all zero); Luma3DS's loader (`sysmodules/loader/source/
  loader.c`, `codeContentPath`) opens `.code` with the same `{type 1,
  ".code"}` tail in its program archive `0x2345678E`.
- **fs:USER is tried first, FSPXI second.** 3dbrew lists `exefs:/.code` as
  not allowed through fs:USER (only RomFS, System Menu Data and the seed
  checks), FSPXI allows all access types. So after `FSUSER_OpenFileDirectly`
  fails, the launcher borrows a client session to `PxiFS0` with Luma3DS's
  `svcControlService(SERVICEOP_STEAL_CLIENT_SESSION)` (SVC 0xB0; Luma
  `k11_extension/source/svc/ControlService.c`) exactly as Checkpoint does
  (`3ds/source/archive.cpp`), then `FSPXI_OpenArchive` + `FSPXI_OpenFile`
  with the same two lowpaths. FSPXI is only used when `svcGetSystemInfo
  (0x10000)` says Luma3DS is running. Luma's 3dsx loader grants a .3dsx full
  FS access (`hbldr.c`: `fs_access_info = 0xFFFFFFFF`).
- **Read-only:** both opens are `FS_OPEN_READ` and the title-content archive
  has no write path; the only write is `nwm.code.tmp` on the SD card,
  renamed to `nwm.code` when complete (a power loss leaves no half file
  under the real name).
- **Compressed or not:** FS returns the ExeFS file as stored (Luma's loader
  decompresses `.code` itself when the exheader flag `compress_exefs_code`
  is set), so `nwm.code` is normally BLZ-compressed; `nwmcode.c` accepts both.
- **Size check:** 64 KiB .. 4 MiB (4 MiB is the ARM9 loader's FCRAM scratch);
  the number of bytes read must equal the file size. The firmware itself is
  then checked by `nwmcode.c` at boot (pools, fixed block sizes).
- The read buffer is zeroed and freed afterwards; the PxiFS0 handle closed
  (FS keeps its own reference).

Not verified on hardware: the launcher builds with devkitARM/libctru (CI job
`firm`), but no console has run it yet. If it prints a red line, 3.2 does
the same job.

### 3.2 Fallback: GodMode9 (`tools/ios3ds-wifi/ios3ds-wifi-fw.gm9`)

GodMode9 is already on every boot9strap/Luma console set up with the usual
guide:

1. Copy the SD card tree as usual (`make sd` / the installer). It has
   `gm9/scripts/ios3ds-wifi-fw.gm9`; or copy
   `tools/ios3ds-wifi/ios3ds-wifi-fw.gm9` to `sd:/gm9/scripts/` by hand.
2. Start GodMode9 (Luma: hold START while powering on, pick GodMode9).
3. Press HOME -> `Scripts...` -> `ios3ds-wifi-fw`, confirm with A.
   It shows the NWM file it read and writes `sd:/ios3ds/nwm.code`
   ("ios3ds: wrote 0:/ios3ds/nwm.code").
4. Boot ios3ds. The log shows
   `[wifi] firmware from 0:/ios3ds/nwm.code: <n> bytes, main type4, <t> ms`.

What the script does: `find 1:/title/00040130/00002d02/content/*.app`
(NWM, the Wi-Fi sysmodule; same title on Old and New 3DS, GBATEK "3DS Files -
Module NWM"), then GodMode9's `extrcode` (decrypts the NCCH and decompresses
its ExeFS `.code`) into `0:/ios3ds/nwm.code`. **NAND is only read**: the
script uses no `allow` and GodMode9 never asks for the SysNAND write
permission; the only write is the file on the SD card. A missing NWM or a
cancel stops the script before anything is written. `nwm.code` is Nintendo's
code: keep it on your card, do not share it.

Without the script (same result): in GodMode9's file browser open
`[1:] SYSNAND CTRNAND/title/00040130/00002d02/content/<id>.app` -> A ->
"NCCH image options..." -> "Extract .code" (GodMode9 writes it to
`0:/gm9/out/`, named after the title id), then move/rename it to
`sd:/ios3ds/nwm.code`. Or on a PC: `tools/ios3ds-wifi/nwmfw.py <that .code>
-o wifi-fw.bin` and copy `wifi-fw.bin` to `sd:/ios3ds/` (takes precedence
over `nwm.code`).

### 3.3 The ARM9 loader

**The loader** (`ar6k_init`, `nwmcode.c`): `wifi-fw.bin` first; otherwise
`nwm.code` is read into the top 4 MiB of FCRAM (free before XNU starts, never
a `load=` target) and processed in place:

- BLZ-compressed (ExeFS form) or plain (`extrcode` form): both accepted; a
  BLZ footer is only tried when the plain search finds nothing.
- GBATEK's literal pools: constant `524C00h` (main / stub-data destination),
  then either the "newer large pool" (`3EDh` at +4, `53FE18h` at +28h,
  `527000h` at +34h) or the "older large pool" (`53FE18h` at +0Ch,
  `527000h` at +18h; type1/type4 from its tail at +38h..+44h with `3EDh` at
  +48h, or from the "older small pool").
- Text base `100000h` (the 3DS sysmodule base) or `10000h` (GBATEK's
  wording); the one where all addresses fall inside the file and the stub
  data / stub code / database are exactly 38h / 316h / 1E8h bytes wins.
- Then the 'I3WF' container in the 64 KiB upload buffer, the FCRAM copy is
  zeroed. Every failure is one red line naming the reason (`no NWM firmware
  pool`, `sizes differ from GBATEK`, `main firmware type not referenced`,
  `BLZ decompression failed`) and Wi-Fi stays off; iOS boots either way.
- `[wifi-diag]` lines then give the pool layout and offset, text base, the
  sizes of type1/4/5 (and whether type4 is one of GBATEK's A053h/A482h/
  A5EBh), whether the database ends where the stub code starts (GBATEK:
  "SAME"), and size + CRC-32 of each uploaded block, so a failing console's
  firmware can be identified without sharing it.

Checked without the real file: GBATEK's pool tables (re-read for this
section; offsets in `nwmcode.c`/`nwmfw.py` match field by field), synthetic
`.code` files of all three pool layouts, plain and BLZ, with type1/4/5
(`make -C tools/ios3ds-wifi test`: the ARM9 C code and `nwmfw.py` produce
byte-identical containers), and QEMU (`emu/wifi_test.py --case nwm`: a
BLZ-compressed synthetic `nwm.code` unpacked by the ARM9 at boot, then the
whole bring-up to an HTTP fetch from iOS). GodMode9's `extrcode`, `find` and
`ask` were checked against GodMode9's `scripting.c` and `HelloScript.gm9`.

**Why not read NAND from the ARM9 directly:** it needs an eMMC driver (ours
only drives the SD slot), CTRNAND's AES-CTR with the console-unique
keyslot whose state depends on the boot chain, NCCH decryption (keyslot 2Ch,
keyY from the NCCH signature), FAT on the decrypted partition and BLZ; none
of it can be exercised in QEMU (no NAND crypto model), and Horizon's FS
(3.1, under Luma before the reboot) or a one-time GodMode9 run (3.2) does all
of it already. Reading would be safe, but the code to
get there would ship untested.

Upload order in `ar6k.c` (GBATEK "DSi SD/MMC Firmware Wifi Firmware"): stub
data -> 524C00h, stub code -> 527000h, BMI_EXECUTE(527000h + 400000h,
527000h), main via BMI_LZ_STREAM_START/DATA -> 524C00h, database -> 53FE18h,
`hi_dset_list_head` = 53FE18h, BMI_DONE. Default main variant is Type4
(`fwmain=1` in wifi.cfg or `nwmfw.py --main type1` to try the other; GBATEK:
Type1 of newer NWM versions may hang, Type5 cannot find APs). (dsiwifi's
disabled upload code re-sends the stub data after the main firmware;
GBATEK's order does not, and neither do we. Try that first if the firmware
never reports `hi_board_data_initialized`.)

**Missing or broken firmware fails safe.** Without either file the ARM9 logs
once `[wifi] 0:/ios3ds/wifi-fw.bin: not found (-4), no 0:/ios3ds/nwm.code
either: Wi-Fi off`, never powers the chip, and iOS sees "no backend"
(ios3ds-wifid waits quietly). A `wifi-fw.bin` that is not an `I3WF`
container logs `bad container`. Firmware that uploads but does not start ends
in state `failed` with one line naming the step (`no HTC_READY`, ...), the
host interest area dumped and the chip held in reset. QEMU checks these
(`emu/wifi_test.py --case nofw`, `--case badnwm`). The test firmware is
random bytes in the container or `.code` layout; no Nintendo or Atheros code
is in the repository or the tests.

## 4. Mailbox and kernel side

`netmbox_t` (`boot/common/wifi_proto.h`) at PA `0x208A0000` / VA `0x808A0000`:
FCRAM between the XNU log ring (ends `0x20890000`) and `topOfKernelData`
(`0x20900000`), same 1 MiB section as the shim, so XNU maps it and never hands
it to the VM. 128-byte header with one writer per cache line (the
`blkmbox_t` rule: the ARM9 sees FCRAM uncached, the ARM11 cleans/invalidates
lines), then 16 RX and 16 TX slots of 1536 bytes. Indices are free-running;
producer writes the slot, then the index. A tail the ARM9 does not believe
(ARM9 restarted under iOS) makes it drop until the ARM11's next read resyncs.

`kern.ios3ds_net` (`tools/ios3ds-wifi/shim/net.c`, built into shim.bin,
registered next to `kern.ios3ds_input` on the first root-disk I/O): one 4 KiB
read/write opaque OID (`sysctl_register_oid` 0x80159c54,
`sysctl_handle_opaque` 0x80159242, no kernel patch, so there is no
`kernel/patches-wifi.json`). Read = status (link, MAC, backend, state, TX
free) + as many pending RX frames as fit, *not consumed*. Write
(`NETIO_CMD`) = absolute ack sequence number + TX frames. Because the handler
builds the outgoing batch before it sees the written data, the daemon writes
first (oldp = NULL) and then reads. Writes need root (the daemon runs as
root). The shared 4 KiB buffer is guarded by a busy flag (`EBUSY` for a
concurrent caller).

## 5. iOS side

`ios3ds-wifid` (`tools/ios3ds-wifi/daemon`, launchd job
`com.ios3ds.wifi`, signed with `com.apple.networking.ethernet.user-access`):
waits until the mailbox has a backend and MAC, creates an
`IOEthernetController` with `kIOEthernetHardwareAddress` = the AR6014's MAC,
makes its socket non-blocking and loops: drain iOS's TX queue into one
write, read the next RX batch, `WritePacket` each frame, mirror the link
state with `SetLinkStatus`, `poll()` the socket for at most 4 ms when idle
(20 ms after ~0.25 s without a frame either way: a frame iOS sends still
wakes it at once; re-log 75).
It finds the interface's BSD name by its MAC in the link-level address list
(`getifaddrs`, `AF_LINK`): IONetworkStack names the interface asynchronously,
so the registry's "BSD Name" is often not there yet. In QEMU it is `en0` (the
only Ethernet-class interface: AppleBCMWLAN never matches without Apple's SDIO
controller).

**Addressing:** configd configures a new Ethernet interface by itself: its
default network set has DHCP, so IPConfiguration leases an address,
IPMonitor installs the default route and hands the DHCP DNS servers to
mDNSResponder (seen in QEMU, section 7.3). Only if the interface has no IPv4
address 15 s after link-up does the daemon run the stock
`/usr/sbin/ipconfig set <if> DHCP`; `nodhcp` never does.

**Self-test:** `nettest=<url> [dns name]` in wifi.cfg reaches iOS through
the mailbox (`netmbox_t.nettest`, `netio_t.nettest`, protocol version 2).
Once the interface has a name the daemon starts `/usr/libexec/ios3ds-nettest
<if> <url> [name]` (`tools/ios3ds-wifi/daemon/nettest.c`), which logs one
`[nettest]` line per step: the DHCP address, the IPv4 default route (routing
table via `sysctl NET_RT_DUMP`), `getaddrinfo(name)` (mDNSResponder with the
DHCP DNS server) and an HTTP/1.0 GET of the URL over BSD sockets, then
`all steps passed` or `FAILED`. Without `nettest=` nothing extra runs. An `https://` URL is fetched through CFNetwork instead, with iOS's own TLS and certificate checks (docs/certificates.md section 5).

Build: `make -C tools/ios3ds-wifi/daemon` (needs `scripts/ios-cc`), then the
usual rootfs build picks up `build/rootfs-add/usr/libexec/ios3ds-wifid` and the
plist via `rootfs/recipe.json`.

## 6. Bring-up checklist on a console

Put `nwm.code` (section 3) on the card; `wifi.cfg` is optional (without it
the chip comes up idle and iOS's Settings > Wi-Fi scans, section 10).
`diag=1` (the default) adds `[wifi-diag] <ms since power-up> ms <stage>: ...`
lines to the console and `sd:/ios3ds/log.txt`, one per bring-up step, in the
spirit of the GPU diagnostics (docs/pica200.md section 11-12). One run shows
where it stops; the last lines before a failure are always

```
[wifi] <step> failed: <error> (sdio status <STAT1|STAT0<<16>)
[wifi-diag] ... FAILED in stage <state>: <step>, error <n>, sdio status ...
[wifi-diag] ... host interest +00..+7c: ...   (only after the upload: how far the firmware got)
[wifi-diag] ... summary: Wi-Fi stopped at stage <state>; chip held in reset; iOS and md0 are not affected
```

**Bounded everywhere.** Each SDIO command ends on a response, an error bit or
~0.1 s; CMD5 is retried at most 200 times 1 ms apart; function-1 ready is
polled at most 1000 times; the 100 ms after the warm reset no longer blocks
the monitor (state BMI waits on the clock); firmware start 3 s, HTC_READY and
WMI_READY 5 s each, a 30 s watchdog over the whole bring-up, the boot-time
scan 10 s, an iOS scan without SCAN_COMPLETE is published after 10 s. A
failure parks the chip in reset and only sets state `failed`; md0, the GPU
mirror and iOS keep running. `monitor: one driver step took <n> ms` reports
every new maximum of 20 ms or more, i.e. how long block I/O waited on Wi-Fi.

| Log line | Settles | If it is wrong |
|---|---|---|
| `fw: newer pool at .code+..., text base 100000, ...; type4 is one of GBATEK's ...` + `fw: ... part N ...: <size>, crc32 ...` | the console's NWM matches GBATEK's pool layout and block sizes | red `nwm.code (...)` line names the check that failed; the type1/4/5 sizes it found are logged |
| `power: CFG11_WIFICNT 00 -> 01, GPIO 2_2 .. -> .. (want bit 0 = 0), GPIO 3_12 .. -> .. (want bit 0 = 1)` | ARM9 writes to CFG11/GPIO take effect | a value that did not change: the register is not writable from the ARM9; power-up must move to the ARM11 stub |
| `sdio: CFG9_SDMMCCTL a -> b (want bit 2 = 0), SD_OPTION 40ee (controller 2 answers)` | TMIO controller 2 at 0x10122000 is reachable from the ARM9 | `NOT answering`: the controller is not mapped/clocked for the ARM9 |
| `sdio: CMD5 xN: first R4 ..., last ... (ready 1, 1 I/O functions, memory 0, OCR ...)` | the chip is powered, out of reset and answers SDIO; N = real CMD5 retries (model: 2) | `stopped at step "CMD5"` + `SDIO enumeration failed: -3`: no answer = power/reset/port-power problem (lines above say which register) |
| `sdio: RCA ..., CCCR rev .., SD rev .., card caps .., bus interface 82` | CMD3/CMD7, CCCR access, 4-bit switch | stopped at `CMD3`/`CMD7`/`CCCR`: compare with libn3ds `SDIO_init` |
| `sdio: function 1 ready after N polls`, `sdio: CIS manufacturer 0271 card ....` | function enable; the card is Atheros (the model's card id 0201 is a guess) | not 0271: a different chip revision; still try |
| `[wifi] SDIO up, chip id 0d000001 (AR6014)` | the diagnostic window at function 1 0x474..0x47C | `(unexpected)`: window addresses differ |
| `[wifi] TMIO2 32-bit FIFO works` | the FIFO at 0x1012210C from the ARM9 | `FIFO check failed`: CMD52 fallback (slow); try the +0x200000 mirror |
| `bmi: target version 2300006f (AR6014 ROM), scratch0 was ..., WLAN_SYSTEM_SLEEP was ...` | warm reset + 100 ms, BMI credits at COUNT_DEC+16 | `BMI_GET_TARGET_ID failed`: longer reset wait, check credit register |
| `fw: part 1/2/3/4 ... in <ms>`, `fw: BMI_EXECUTE 00927000 ... returned <x>` | BMI write/LZ stream; the stub runs and returns | which part fails; the stub's return value |
| `boot: hi_board_data_initialized = 1 after <ms>` | the stub/firmware set the flag (model assumption; the model needs 50 ms) | `FAILED in stage boot` + host interest dump: re-send stub data after main (dsiwifi order), section 3 |
| `htc: HTC_READY ..., <n> bytes: <hex>` | Type4 firmware boots and speaks HTC (model: 8 credits of 1664 bytes) | `FAILED in stage htc`: try `fwmain=1` |
| `htc: service 0100 -> status 0, endpoint 1, max message ...` (0100..0104) | service connect layout and endpoints (model: 1..5 in order) | a non-zero status: the service is not offered by this firmware. `tx waits for credits on endpoint 0 (0 < 1)` followed by `HTC service setup failed: -5` instead means the *driver* ran out of HTC control credits (issue #11); -5 is `SDIO_ERR_BUSY`, not a firmware error |
| `htc: endpoint 0 credit not returned: funding 1 for this control message` (at most 6 times: 5 CONNECT_SERVICE + SETUP_COMPLETE) | the firmware does not credit EP0 back between control messages, so the driver funds them locally as dsiwifi does | absent when the firmware/QEMU returns credits; harmless either way |
| `wmi: WMI_READY ..., <n> bytes: <hex> (MAC, phy capability)` | WMI_READY layout (model: 7 bytes) | longer event: more fields (sw/abi version) we ignore, harmless |
| `wmi: REG_DOMAIN <hex>` | regulatory domain reported after READY (model: 0) | absent: harmless |
| `wmi: channel list: N channels, 2.4 GHz mask ....` | the channels the firmware will scan (model: 1-11, mask 0ffe; JPN/EUR consoles should show 1-13 = 3ffe) | `WMI command 000e rejected`: this firmware has no GET_CHANNEL_LIST; harmless |
| `summary: bring-up OK in <ms>` | everything up to WMI | |
| `scan: request from iOS: ...` then `scan: "<ssid>" <bssid> <MHz> (ch n) rssi <r> snr <s> beacon/probe resp, RSN/WPA/WEP/open` | BSS filter 1 + START_SCAN over all channels returns BSSINFO for every AP (dsiwifi scans per channel); BSSINFO layout; RSSI units (model: -40..-81 dBm) | no lines, `SCAN_COMPLETE ... 0 BSSINFO`: scan per channel like dsiwifi (`SET_CHANNEL_PARAMS` + `START_SCAN`); garbled SSIDs: BSSINFO layout differs |
| `scan: SCAN_COMPLETE after <ms>, status ..., <n> BSSINFO events, <m> networks` | the event exists and how long a real scan takes (model: 30 ms per channel) | `no SCAN_COMPLETE 10 s after the scan request`: the list is still published |
| `wmi: event <id>, <n> bytes: <hex>` (at most 24) | events we do not handle (GBATEK: Type4 sends 101Dh..1025h; 1025h is to be ignored) | |
| `[wifi] iOS: scan` / `[a80211] scan done: N networks` | the networks reach Settings > Wi-Fi | |
| (with `wifi.cfg`) `connect ...`, `associated`, `EAPOL event 1/2`, `link up` | association and our RSN IE (capabilities 0) | `disconnected (WMI reason 4, 802.11 code 17)`: copy the firmware's IE from the CONNECT event |
| `[wifid] interface is en0`, `[nettest] ... all steps passed` | IOUserEthernet + DHCP + data path on hardware | rx/tx stall: data mailbox control word `2008h` (dsiwifi), section 9 |

### 6.1 QEMU model guesses and the line that settles each

| Model behaviour (emu/qemu/hw/arm/ctr_wifi.c) | Source | Confirmed / denied by |
|---|---|---|
| Card answers only after CFG11_WIFICNT=1, GPIO 2_2=0, GPIO 3_12=1 | libn3ds | `power:` readback + `sdio: CMD5 ... ready 1` |
| CFG9_SDMMCCTL bit 2 = 0 powers port 2 | ctr_sdhc.h | `sdio: CFG9_SDMMCCTL a -> b` |
| CMD5: OCR-0 inquiry "not ready", ready at the 2nd try | SDIO spec | `sdio: CMD5 xN` (N, both R4 values) |
| RCA 1; CCCR/CIS contents; MANFID 0271/card 0201 | SDIO spec, guess | `sdio: RCA`, `sdio: CIS manufacturer` |
| Chip id 0D000001 through the window at 0x474/0x478/0x47C | GBATEK, ath6kl | `[wifi] SDIO up, chip id` |
| 32-bit FIFO at +0x10C works from the ARM9 | TMIO1 behaviour | `TMIO2 32-bit FIFO works` / `check failed` |
| BMI ROM ready right after the warm reset (we wait 100 ms) | ath6kl | `bmi: target version` |
| Stub EXECUTE returns at once | guess | `fw: BMI_EXECUTE ... returned ... in <ms>` |
| Firmware runs 50 ms after BMI_DONE and the stub sets `hi_board_data_initialized` | GBATEK order, guess | `boot: hi_board_data_initialized = 1 after <ms>` or the host interest dump |
| HTC_READY: 8 credits x 1664 bytes | ath6kl | `htc: HTC_READY ... <hex>` |
| Endpoints 1..5 in connect order | ath6kl, dsiwifi | `htc: service 01xx -> ... endpoint` |
| WMI_READY = MAC + phy capability (7 bytes), REG_DOMAIN 0 right after | ath6kl, guess | `wmi: WMI_READY ... <n> bytes`, `wmi: REG_DOMAIN` |
| GET_CHANNEL_LIST supported, 11 channels | ath6kl, guess | `wmi: channel list` or `WMI command 000e rejected` |
| BSS filter 1 + all-channel START_SCAN reports every AP; 30 ms per channel | guess (dsiwifi scans per channel) | `scan: "<ssid>" ...` lines, `SCAN_COMPLETE after <ms>` |
| BSSINFO = u16 MHz, u8 frame type, u8 SNR, s16 RSSI (dBm), BSSID, ieMask, beacon body | ath6kl, dsiwifi | the parsed `scan:` line: readable SSID, 2412-2484 MHz, negative RSSI |
| No events beyond the ones modelled | - | `wmi: event ...` lines |
| Commands finish instantly, no SDIO busy | - | `monitor: one driver step took <n> ms`, the `<ms>` column |

Not modelled and still to try on a console if WMI commands go unanswered:
dsiwifi writes 3 to the word that `hi_app_host_interest` points at after
`WMI_REG_DOMAIN_EVENT` ("allows more commands to be sent"); we do not.

`backend=loop` exercises everything above the driver on hardware without a
network (frames from iOS come straight back).

## 7. QEMU: the Wi-Fi model (`emu/qemu/hw/arm/ctr_wifi.c`)

The `nintendo3ds` board has the Wi-Fi slot modelled at the bus level, so the
real driver (`sdio.c`, `ar6k.c`, `wpa2.c`) runs unchanged against it
(`backend=auto`, nothing QEMU-specific). Everything the model does is written
from the references below, not from our driver; where they disagree or say
nothing, the model's choice is listed under "assumptions".

```
 ARM9 driver --MMIO--> TMIO controller 2 @0x10122000 (polled, no IRQ)
   --CMD5/3/7/52/53--> SDIO card: CCCR, FBR, CIS (Atheros 0271/0201), function 1
     --mailbox 0, function-1 registers--> AR6014 "target"
         BMI ROM: command parser, RAM bookkeeping (firmware contents never run)
         firmware: HTC (services, credits), WMI (scan, connect, keys, events)
     --"air"--> access point: beacons/probe responses, association,
                WPA2-PSK authenticator (IEEE 802.11i), CCMP keys checked
     --Ethernet II--> QEMU NIC backend: -nic user,model=ctr-wifi (slirp:
                DHCP 10.0.2.15, router + host 10.0.2.2, DNS 10.0.2.3, NAT)
```

Run it: `python3 emu/wifi_test.py` (section 7.3). Manually: boot with
`-nic user,model=ctr-wifi` (plus `-global ctr-wifi.ssid=...`,
`-global ctr-wifi.psk=...`) and put a `wifi.cfg` with the same SSID/PSK and a
`wifi-fw.bin` on the SD image.

### 7.1 What is modelled, and from which source

| Layer | Model | Source |
|---|---|---|
| Power | card answers only with `CFG11_WIFICNT` bit 0 = 1, GPIO 3_12 (nRESET) = 1, GPIO 2_2 = 0; nRESET low = cold reset (RAM, SDIO state) | libn3ds `WIFI_init`, `gpio.h` |
| TMIO | command/response, STAT0/STAT1 write-0-to-clear, 32-bit FIFO (`0x10C`) with `DATA32CTL` RX-full / STAT1 TXRQ per block, BLKLEN32/BLKCNT, CMD timeout when nothing answers, data timeout when the controller's block count x length differs from the CMD53 byte count, CRC error when card and controller bus widths differ | the board's TMIO1 model, libn3ds `tmio.h`, dsiwifi `wifi_sdio.twl.c` |
| SDIO | CMD5 (R4: inquiry with OCR 0 is "not ready", ready after an OCR with a common voltage), CMD3 (RCA 1), CMD7 select, CMD52 R/W/RAW, CMD53 byte and block mode with incrementing or fixed address; function 1 only after `CCCR 0x02` bit 1; `0x07` bus width; block sizes at `0x10`/`0x110` | SDIO Simplified Spec 3.00 |
| Function 1 | `HOST_INT_STATUS` (bit 0 = mailbox 0 has data, bit 7 = error), `ERROR_INT_STATUS` (TX overflow / RX underflow, write-1-clear), `RX_LOOKAHEAD_VALID` (bit 0 = 4 bytes ready), `RX_LOOKAHEAD0`, `COUNT[4]`/`COUNT_DEC[4]` = BMI credit (reading the decrementing copy takes it, each finished command returns it), the diagnostic window (data `0x474`, write address `0x478`, read address `0x47C`; writing the address LSB starts the cycle) | ath6kl `target.h`, `sdio.c` (`ath6kl_sdio_bmi_credits`, `ath6kl_set_addrwin_reg`), dsiwifi |
| Mailbox 0 | one byte stream each way. Windows `0x000-0x0FF`, `0x800-0xFFF`, `0x1000-0x3FFF`; a host write that touches the window's last address ends a message (HTC mode). Target messages are padded to `hi_mbox_io_block_sz` | ath6kl `hif.h`, dsiwifi (`0x4000 - len`, byte register `0xFF`) |
| BMI | `NOP, DONE, READ/WRITE_MEMORY, EXECUTE, SET_APP_START, READ/WRITE_SOC_REGISTER, GET_TARGET_ID (2300006Fh), LZ_STREAM_START, LZ_DATA`; data chunks up to 200h bytes; RAM 0x500000-0x53FFFF incl. the host interest area at 0x520000; `0x40EC` = chip id 0D000001h; `RESET_CONTROL` bit 8 = warm reset back into the ROM (`RESET_CAUSE` 2). Wedges (log line, no reply) on unknown commands, oversized chunks, writes outside RAM, `EXECUTE` of an address nothing was uploaded to, `LZ_DATA` without `LZ_STREAM_START` | dsiwifi `ath/bmi.h`, `wifi_card.twl.c`; GBATEK; ath6kl `bmi.c` |
| Firmware start | `BMI_DONE` + 50 ms: runs only if main firmware was streamed (`LZ_DATA` >= 1 KiB), the stub was executed and `hi_dset_list_head` points at uploaded data; otherwise the target stays dead | GBATEK upload order |
| HTC | `HTC_READY` (8 credits of 1664 bytes), `CONNECT_SERVICE` for services 0100h-0104h (endpoints handed out in order: 1 = WMI, 2-5 = data; unknown/duplicate services refused), `SETUP_COMPLETE(_EX)`; the target keeps one credit for EP0 and one per service, the rest go to the first data endpoint. Every host message consumes `ceil(padded_len / 1664)` credits; without enough it logs `Mbox Full`, sets TX overflow and drops the message. Credits come back in RX trailers (`HTC_FLG_RX_TRAILER`, record 1), and a message with `NEED_CREDIT_UPDATE` gets an empty EP0 credit report if nothing else is sent. QOM: `htc-credits-used`, `htc-credit-reports`, `htc-credit-waits`, `htc-overflows` | ath6kl `htc.h`, `htc_mbox.c`; dsiwifi `htc.twl.c` |
| WMI | 2-byte command header (AR6002-era firmware). Commands with a wrong parameter size get `WMI_CMD_ERROR_EVENT` (INVALID_PARAM). Events: `READY` (MAC, phy cap), `REG_DOMAIN`, `BSSINFO` (beacon or probe response with SSID, rates, DS, RSN IEs; SNR 40), `SCAN_COMPLETE`, `CONNECT` (channel, BSSID, intervals, assoc request/response IEs), `DISCONNECT` (802.11 code, BSSID, WMI reason), `GET_CHANNEL_LIST` reply. Scan: 30 ms per channel, honours the channel list, BSS filter (none / all / probed-SSID) and a hidden AP (only found by a directed probe) | dsiwifi `ath/wmi.h`, `wmi.twl.c`; ath6kl `wmi.h`; GBATEK "WMI" |
| Connect | `WMI_CONNECT` (52 bytes) must name the AP's SSID; channel/BSSID must match if given; WPA2 AP needs auth 5, pairwise 4 (AES), group = the AP's group cipher; open AP needs 1/1. Mismatch = `DISCONNECT` with the 802.11 status a real AP would use (41/42/43) | GBATEK "WMI Connect", 802.11-2007 7.3.1.9 |
| Authenticator | PMK = PBKDF2(psk, ssid) (GLib HMAC-SHA1), message 1 (ANonce), message 2 checked (replay counter, MIC with the PTK from PRF-384/512, IE equal to the association request's, compared like hostapd), message 3 (AP RSN/WPA IE + GTK KDE, AES key wrap with QEMU's AES, or RC4-drop-256 for WPA v1), message 4 MIC; retransmit every 1 s, deauth after 4 tries (reason 15). Written from IEEE 802.11i, not from `wpa2.c` | IEEE 802.11i-2004 8.5, RFC 3394, hostapd behaviour |
| WPA v1 | `security=wpa1`: WPA vendor IE (0xDD 00:50:F2 01) in beacons, probe responses and the association request; WMI_CONNECT auth 3 / TKIP pairwise / TKIP group; EAPOL-Key descriptor 0xFE, version 0, HMAC-MD5 MIC, Key IV + KEK RC4-drop-256 Key Data, 64-byte PTK, 32-byte TKIP GTK. The firmware installs TKIP keys with `WMI_ADD_CIPHER_KEY` (MIC halves swapped), so no software TKIP/Michael is needed in the driver | 802.11i/D10, WPA spec, hostapd `wpa_auth.c`/`wpa_common.c`, GBATEK "WMI Connect Functions" |
| Keys | `WMI_ADD_CIPHER_KEY` (45 or 51 bytes): pairwise must equal the AP's TK (type AES), group must equal the GTK with the AP's key index (TKIP: MIC halves swapped, as dsiwifi). Unicast frames are delivered/accepted only with the right TK, group frames only with the right GTK, nothing but EAPOL before message 4 | ath6kl `wmi.h` (`key_usage`), dsiwifi `wmi_add_cipher_key` |
| Data | host TX on a data endpoint: 2-byte WMI data header (`info & 3` = 0), 802.3 header, LLC/SNAP; source must be the station MAC. RX: `rssi, info, 802.3, LLC/SNAP` on the BE endpoint. Up to 24 KiB queued towards the host, beyond that frames are dropped like a full firmware buffer | dsiwifi `data_handle_pkt`, GBATEK "MBOX Transfer Headers" |

Properties (`-global ctr-wifi.<name>=`): `ssid` (default `ios3ds-qemu`),
`psk` (`ios3ds-test-passphrase`), `security` (`wpa2`/`wpa1`/`open`, plus the
`wpa2-wpa3` and `wpa3` cases), `group` (`ccmp`/`tkip`), `channel` (1-13, default 6), `hidden`, `debug` (0-3; 1 logs
every milestone as `ctr-wifi: <virtual seconds> ...`), `mac` (the station MAC
the firmware reports; QEMU's NIC default 52:54:00:12:34:56).

### 7.2 Fidelity limits (things the model cannot prove)

- **No firmware is executed.** Upload contents are ignored; only sizes,
  addresses and order are checked. Whether Nintendo's Type4 main firmware
  behaves like the model's WMI layer is open (section 6).
- **No radio, no CCMP.** Encryption is modelled as "keys equal" checks. No
  rekeying (group message 1/2), no PMKSA caching, no power save, no roaming,
  no beacon loss.
- **No timing.** TMIO commands finish immediately, CMD5 is ready at the
  second try, the firmware boots in 50 ms. Real SDIO clocks, busy states and
  FIFO stalls are not modelled.
- **No interrupts.** Neither the TMIO2 IRQ (GIC 64/65 on the ARM11) nor the
  SDIO card interrupt; the ARM9 polls anyway.
- **Credits are returned immediately** when the model processes a host message
  (or one virtual ms after `NEED_CREDIT_UPDATE`), not when a real firmware's
  buffer is freed. The accounting and the `Mbox Full` refusal are real, so a
  host that ignores credits fails here; the exact timing of a return is not.
- **Assumptions** (no source; settle on hardware with the log lines in
  section 6): the board-data stub sets `hi_board_data_initialized` = 1
  (dsiwifi only shows it being 1 after boot); the firmware's association
  RSN IE has capabilities 0 (our message 2 sends 0; a different value would
  make a strict AP reject message 2); BSS filter 1 reports BSSINFO during a
  START_SCAN over all channels (dsiwifi scans one channel at a time with
  filter 1); `REG_DOMAIN` follows `READY`; the AP sends 802.1X version 2.

### 7.3 `emu/wifi_test.py`

Writes `wifi.cfg` (with `nettest=`, section 5) and a dummy `wifi-fw.bin`
(random bytes in the `nwmfw.py` container: the model never runs firmware) to
`build/sd.img`, starts an HTTP server on the host's `127.0.0.1:8080` (iOS
reaches it as `10.0.2.2:8080` through slirp), boots, checks every step in
the log and removes the two files again. Cases: `--case ok` (WPA2, default),
`credit` (`nettest=<url> <dns> bulk`: iOS fetches a 4 MiB body, checks every
byte and the log must show credits used/returned and no `Mbox Full`),
`open`, `nocfg` (Wi-Fi must stay off and the card unpowered), `nofw`
(firmware missing: one log line, Wi-Fi off), `wrongpsk` (message 2 MIC
rejected, AP deauths after 4 tries, no link), psk hint after the third try), `nofifo` (model property
`fifo32=off`: the driver's FIFO self-test fails and the mailbox runs over
CMD52), `hidden` (AP only answers a directed probe), `tkip` (WPA2 with a TKIP group
key, MIC halves swapped), `wpa1` (WPA v1 vendor IE, TKIP pairwise and group,
HMAC-MD5/RC4 key data), `nwm` (no `wifi-fw.bin`: a synthetic BLZ-compressed
`nwm.code` with GBATEK's newer pool, cut up by the ARM9 at boot; also checks
the `[wifi-diag]` lines of section 6), `badnwm` (an `nwm.code` that is not
NWM: one red line, card never powered). `--icount 2` gives hardware-like
timing. `emu/wifi_ui_test.py --nwm` runs the Settings test with `nwm.code`
and no `wifi.cfg`, i.e. the files a console has after section 3.

The test needs a QEMU with slirp (`-nic user`): `emu/build-qemu.sh`
configures it with `--enable-slirp`; a build without it fails every check
with `network backend 'user' is not compiled into this binary` in `run.txt`.

Results (2026-11-18, re-log 45): `ok` PASS in plain QEMU and with
`--icount 2` (SpringBoard on screen, `[nettest] all steps passed`, the host's
server logged `GET /ios3ds-nettest`); `open`, `nocfg` (full boot to
SpringBoard), `nofw`, `wrongpsk`, `nofifo`, `hidden` PASS. Later runs
(2026-11-22, re-log 91-93) with this branch's QEMU: `ok`, `tkip` and `wpa1`
PASS end to end, including the credit case's 4 MiB with 0 bad bytes,
1,216 credits consumed and returned and 0 overflows. LED QOM checks matched
`wifi_led` on/off. For a branch with its own QEMU build,
set `IOS3DS_QEMU` (both `emu/run.py` and `emu/soak.py` honour it). The
status bar showed "Searching..." and no Wi-Fi bars; section 10 added them.

The paravirtual "ctr-vnic" NIC that an earlier version of this section
proposed (`vnic.c`, `backend=qemu`, register spec in `boot/common/wifi_proto.h`)
bypasses the driver and was not built; `backend=qemu` fails cleanly on this
board ("backend qemu-vnic not available").

## 8. Shared-file hooks

- `boot/Makefile`: wifi sources added to `ARM9_SRC`.
- `boot/arm9/main.c`: `#include "wifi/wifi.h"`, `wifi_init()` before the jump,
  `wifi_poll()` in the monitor loop.
- `scripts/mkboot.py`: `tools/ios3ds-wifi/shim/net.c` added to the shim build.
- `kernel/shim/blk.c`: `shim_net_register()` next to `shim_input_register()`.
- `rootfs/recipe.json`: adds `ios3ds-wifid`, `ios3ds-nettest` and `com.ios3ds.wifi.plist`;
  `ios3ds-a80211.dylib` (configd) and `ios3ds-hwaes.dylib` (securityd) with
  `DYLD_INSERT_LIBRARIES` in their launchd plists; `SBNoWatchdogs` (section 10).
- `emu/qemu/hw/arm/nintendo3ds.c`: one `ctr_wifi_init()` call; `emu/build-qemu.sh`
  links `ctr_wifi.c` and configures QEMU with slirp.
- `Makefile`: `make test` also runs `make -C tools/ios3ds-wifi test`;
  `tools/ios3ds-wifi/a80211` and `tools/ios3ds-keychain` in `USERLAND`.
- `scripts/build-ios-toolchain.sh`: SystemConfiguration added to the stub SDK.

## 9. Open items

- Hardware bring-up (section 6). The MCU Wi-Fi LED now follows the link
  ([docs/mcu.md](mcu.md) §14), but it has only been checked in QEMU.
- Status-bar Wi-Fi icon and Settings > Wi-Fi: done (section 10; it was
  listed here as "not feasible without a kernel IO80211 controller").
- Data frames are sent with mailbox control word 0; dsiwifi uses `2008h` for
  IP traffic. Try that if unicast data does not flow after the handshake.
- WEP and enterprise (802.1X) networks are not supported. The firmware does
  have a WEP key type in `WMI_ADD_CIPHER_KEY`, but that alone is not enough:
  WEP needs an open/shared-key association and key management we do not do.
- Faster path later: wake the daemon from the shim instead of 4 ms polling,
  CMD53 byte mode for BMI instead of one CMD52 per byte.

## 10. iOS Settings > Wi-Fi and the status bar (Apple80211 bridge)

Sections 2-9 give iOS a working `en0`, but iOS itself does not know it is
Wi-Fi: Settings showed "No Wi-Fi", the status bar no bars, and the network
had to be preset in `wifi.cfg`. All of that belongs to configd's
`WiFiManager.bundle` (MobileWiFi's server), and WiFiManager only drives
Apple's own 802.11 driver. Since this section, iOS's own UI scans, joins
(WPA2-PSK and open networks, password typed on the on-screen keyboard),
remembers networks and shows the signal in the status bar; `wifi.cfg` is
optional.

```
 Settings (AirPortSettings)  SpringBoard (bars)      ARM9 (ar6k.c)
        | MobileWiFi IPC           |                      ^ scan / join(ssid, PMK) / leave
        v                          v                      | status, scan table (16 x 128 B)
 configd: WiFiManager.bundle -- Apple80211 ioctls --> ios3ds-a80211.dylib --sysctl kern.ios3ds_wlan--> wlanmbox_t
          IPConfiguration (DHCP)    802.11 kernel events <--  (DYLD_INSERT_LIBRARIES)                @NET_PA+0xC200
 securityd -- AppleKeyStore / IOAESAccelerator --> ios3ds-hwaes.dylib (keychain: the Wi-Fi password)
```

### 10.1 ARM9 and the control mailbox

- **No `wifi.cfg` + `wifi-fw.bin` present:** the driver brings the chip up as
  before and stops in state `idle` (`[wifi] idle: no network configured,
  waiting for iOS (Settings > Wi-Fi)`). **Neither file:** one line, the chip is
  never powered (`[wifi] 0:/ios3ds/wifi-fw.bin: not found ... Wi-Fi off`), iOS
  sees no Wi-Fi hardware. A `wifi.cfg` with `ssid=` works as before (joins at
  boot); iOS can still scan and switch networks. A bring-up that reaches
  `failed` instead (`HTC service setup failed`, ...) leaves the backend with
  no MAC, so `ios3ds-wifid` waits and Settings has no Wi-Fi control to turn
  on: that is a driver bring-up failure, not a missing `wifi.cfg` (issue #11).
- `wlanmbox_t` (`boot/common/wifi_proto.h`) at `NET_PA + 0xC200`, inside the
  network mailbox's section: a status block (state off/starting/idle/joining/up,
  SSID, BSSID, channel, RSSI/noise, security, last disconnect reason, scan and
  link sequence numbers), a command block (`scan`, `join` with SSID + 32-byte
  PMK or open, `leave`; sequence-numbered, acknowledged in the status) and a
  scan table of 16 networks (SSID, BSSID, channel, RSSI, noise, capability,
  beacon interval, the first 76 bytes of IEs). One writer per cache line, as
  `netmbox_t`.
- `kern.ios3ds_wlan` (`tools/ios3ds-wifi/shim/net.c`): read = status + scan
  table, write = one command (root only).
- iOS hands the ARM9 the **PMK** (WiFiManager computes PBKDF2 itself), so a
  join from Settings costs no PBKDF2 on the ARM9.

### 10.2 `ios3ds-a80211.dylib` in configd

`tools/ios3ds-wifi/a80211/a80211.c`, inserted with `DYLD_INSERT_LIBRARIES` in
configd's launchd plist (rootfs recipe), interposes (`__DATA,__interpose`)
exactly what WiFiManager uses, everything reverse engineered from
WiFiManager-185.6.12 (iOS 4.2.1):

| WiFiManager does | It gets |
|---|---|
| `IOServiceMatching("IO80211Interface")` + `IOServiceFirstMatch` notification; per match it reads "BSD Name" (31 tries, 10 ms apart) or gives up for good ("Could not obtain IO80211Interface bsd name") | a notification on a class that never matches; once `en0` exists (found by the AR6014's MAC in `getifaddrs`), a run-loop timer on the run loop the client's notification port is scheduled on (`CFRunLoopAddSource` is watched) hands its callback an iterator with `IOBSDNameMatching(en0)`. Two clients in configd register, in either order from boot to boot; each one is served. The real notification would come 20+ s too early: InterfaceNamer names the IOUserEthernet interface only after IOKit is quiet. |
| `IOObjectConformsTo(parent, "IO80211Controller")`, `IORegistryEntryCreateCFProperties(controller)` | true for `IOUserEthernetController`; plus `IO80211HardwareVersion`, `IO80211DriverVersion`, `IO80211CountryCode` (X0), `IO80211Locale` (ETSI) |
| `SIOCGIFMEDIA` on en0 (Apple80211GetIfListCopy keeps interfaces whose media type is `IFM_IEEE80211`) | `IFM_IEEE80211`, active. This also makes SystemConfiguration and IPConfiguration treat en0 as Wi-Fi (`kSCNetworkInterfaceTypeIEEE80211`). |
| `ioctl(SIOCGA80211 / SIOCSA80211)` (`0xC02069C9` / `0x802069C8`, `struct apple80211req` {ifname, type, value, len, data}) | answered from `kern.ios3ds_wlan`, table below |
| `socket(PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT)` + `SIOCSKEVFILT` {vendor 1, class 6 (802.11), subclass 1} | one end of a datagram socketpair; a watcher thread (250 ms) posts 40-byte `kern_event_msg`s with the interface name: 1 power, 2 SSID, 3 BSSID, 4 link, 9 association done, 10 scan done |
| notify state `com.apple.iokit.hid.displayStatus` (auto-join and the disassociation timer only run with the display on; "Device display is OFF" at every boot) | nobody posts it on ios3ds (the HID event system does on an iPod), so the watcher mirrors SpringBoard's `com.apple.springboard.hasBlankedScreen` into it (on unless blanked) and posts the change: `[a80211] display on (SpringBoard) -> ...` |
| `SCPreferencesGetValue` (logging keys only) | `IOS3DS_WIFI_LOG=1` in configd's environment sets WiFiManager's own `LoggingEnabled`/`LoggingFile` to `/dev/console` (all its NOTICE lines); `=2` also logs every Apple80211 request with its caller. Off by default. |

Apple80211 requests (iOS 4 numbering; "first GET/SET type N" is logged once
per type):

| Type | Request | Answer |
|---|---|---|
| 1 / 9 | SSID (raw bytes, length in `req_len`) / BSSID | current network; ENXIO when not associated |
| 2, 4, 8, 13, 14, 15, 5 | auth type, channel, rate, state (INIT/AUTH/RUN), PHY mode (11b/g), op mode (station), power save (off) | from the status block |
| 10 (SET) | scan request | `scan` command; scan-done event when the ARM9's scan sequence moves, **at the latest after 8 s** (WiFiManager's synchronous scan waits for the event without a timeout) |
| 11 | scan result | one 0x8C-byte record per call (channel, flags, noise, RSSI, beacon interval, capability, BSSID, 12 rates, SSID, IE pointer/length at +0x88/+0x86), EIO after the last |
| 12 | card capabilities | WPA2-PSK, TKIP, AES-CCM bits |
| 16 / 17 | RSSI / noise (dBm, control and aggregate) | from the AR6014's BSSINFO of the current network |
| 19 | power (get / set) | soft radio switch; off = `leave` |
| 20 (SET) | associate (0x180 bytes: SSID len +0x0C, SSID +0x10, key length +0x3C, key type +0x40, key +0x48) | `join`: key type 6 (PMK, 32 bytes) = WPA2-PSK, no key = open; WEP and others: EOPNOTSUPP |
| 21 | association result | 0 pending, 1 done, 3 / 9 failed (disconnect reason / 30 s timeout); judged only after the ARM9 acknowledged the join command (WiFiManager disassociates first, so the status still shows idle for a poll: the first version reported "failed" while the join went on) |
| 22 (SET) | disassociate | `leave` |
| 27, 32, 29, 43, 44, 201, 204 | supported channels (1-13), rate set, deauth reason, driver/hardware version strings, current network as a scan record, RSSI bounds (-95/-30, threshold -85) | constants / status |
| others (6, 7, 18, 57, ...) | | EOPNOTSUPP, like a driver without the feature |

### 10.3 The keychain (`ios3ds-hwaes.dylib` in securityd)

The first join failed with WiFiManager's `No password for ios3ds-qemu`:
Settings stores the password in the keychain (service `AirPort`, account =
SSID) and WiFiManager reads it back, and **every keychain write on ios3ds
failed** with `errSecInteractionNotAllowed` (-25308; a probe `SecItemAdd`
from configd showed it). iOS 4.2.1's securityd encrypts each item with a
random key and has AppleKeyStore wrap that key with the class key of the
item's protection class (IOConnectCallMethod selector 10 KeyWrap / 11
KeyUnwrap, 32 -> 40 bytes, RFC 3394 sizes). Those class keys come from the
system keybag that keybagd loads at boot; keybagd needs the iPod's AES engine
and effaceable storage and was disabled long ago (re-log 18), and there is no
IOAESAccelerator either ("unable to access hwaes key"). So mail account
passwords could not be saved either.

`tools/ios3ds-keychain/hwaes.c` is inserted into securityd the same way and
answers `IOServiceMatching("AppleKeyStore" / "IOAESAccelerator")` with
IOResources, `IOServiceOpen` with private handles, AppleKeyStore selector 0
(open), 10/11 (RFC 3394 AES key wrap with one software key for every class,
checked once on the host against the RFC's test vector, not part of
`make test`) and IOAESAccelerator selector 1
(AES-CBC with CommonCrypto; hardware keys UID/GID/0x835 replaced by the same
software key). Everything else returns `kIOReturnUnsupported`. After it, the
probe `SecItemAdd` returns 0, Settings stores the password and WiFiManager
finds it (`SecItemCopyMatching -> 0`).

**Security note:** the keychain is no longer bound to the hardware: anyone
who has the SD card (rootfs.hfs) and this source can decrypt it. The SD card
is not encrypted anyway. A console-bound key (the ARM9 can use the 3DS AES
engine's console-unique keyslots) is possible later.

### 10.4 Other changes

- **Launch watchdog off** (`SBNoWatchdogs` in com.apple.springboard.plist):
  with a Wi-Fi device present, Settings needs longer than SpringBoard's launch
  watchdog in QEMU and was killed (`0x8badf00d`, "Settings failed to launch
  in time", CPU 10 s). The 3DS is slower than the iPod; the watchdog would
  kill slow launches on hardware too.
- **QEMU:** the access point model also reports two beacon-only decoys in
  scans, NeighborNet (WPA2, channel 1, -81 dBm) and CoffeeShop (open,
  channel 11, -72 dBm), `-global ctr-wifi.decoys=off` drops them.
- **`emu/session.py`**: one QEMU in the background, one action per call
  (tap, drag, type on the iOS keyboard, key, shot, log). Keyboard rows were
  measured on a screenshot (291/347/400/457); the 123 page stays up after a
  symbol, and the page is remembered between calls.
- **`emu/wifi_ui_test.py`**: the whole flow through the touch screen (below).

### 10.5 Verified in QEMU

`python3 emu/wifi_ui_test.py` (no `wifi.cfg`, dummy firmware, ca_gpu=0 for
readable screenshots), 2026-11-18 on agent/wifi3:

| Check | Log line / evidence |
|---|---|
| chip idle without `wifi.cfg` | `[wifi] idle: no network configured, waiting for iOS (Settings > Wi-Fi)` |
| WiFiManager has the interface | `[a80211] handing en0 to an IO80211Interface client`; Settings: "Wi-Fi  Not Connected" |
| Settings > Wi-Fi scans | `[wifi] iOS: scan`, `[a80211] scan done: 3 networks`; the list shows CoffeeShop, ios3ds-qemu (lock), NeighborNet (lock) |
| join with the on-screen keyboard | 22 dots typed, Join: `[a80211] associate "ios3ds-qemu" (WPA2-PSK, PMK from WiFiManager, auth 1/8)`, `[wifi] iOS: join "ios3ds-qemu" (WPA2), in the scan list`, `[wifi] link up: ios3ds-qemu`, `[a80211] association done (result 1)` |
| DHCP | `[wifid] en0 configured by configd` (WiFiManager's log: `Joined: ios3ds-qemu`, `Address: 10.0.2.15`) |
| status bar | check mark in the list, Wi-Fi bars in the status bar, also on the home screen |
| reboot, nothing touched | the known network is joined by WiFiManager's auto-join (password from the keychain): `[a80211] display on (SpringBoard) -> com.apple.iokit.hid.displayStatus`, `[a80211] associate "ios3ds-qemu"`, `association done`, bars |

8/8 checks PASS in 358 s (the tap on the Settings icon right after the home
screen appears is sometimes ignored; the test presses Home and retries).
Screenshots: `build/emu-wifi-ui/0*.png`. With the change in place,
`emu/wifi_test.py --case ok` (preset network in `wifi.cfg`, HTTP fetch
through iOS) and `--case nocfg` (no `wifi.cfg`, no firmware: one line, chip
never powered, full boot) still PASS; in the `nocfg` case WiFiManager simply
never gets an interface and Settings shows "No Wi-Fi". `make test` and
`emu/soak.py --cycles 10` (configd and securityd with the inserted
libraries, 10 app cycles and a long lock) PASS.

### 10.6 Unverified on hardware

Besides section 6's list (SDIO, BMI, firmware, WMI, 4-way):

- The real Type4 firmware answers a scan **without** a probed SSID with
  BSSINFO for every network (the model does; with `wifi.cfg` the driver
  always probed for one SSID) and accepts WMI_CONNECT for an SSID chosen
  after an idle period.
- RSSI/noise from BSSINFO are in the units the status bar expects (the model
  sends RSSI -40..-81 dBm-like values; dsiwifi treats the byte as SNR).
- WiFiManager's PMK equals our PBKDF2 (same standard; verified only against
  the model's authenticator).
- Disconnect / roaming / AP loss events: the model never loses the AP, so the
  link-down path to WiFiManager (event 4, then auto-join) is untested.
- A slow console: the 8 s scan guard, WiFiManager's association timeout and
  the 250 ms event poller were tuned in QEMU only.
- The software keychain key works the same on hardware (no hardware involved),
  but securityd's other AppleKeyStore uses (backup keybags, selectors 4/6/12)
  are answered "unsupported" and were never exercised.

### 10.7 Open items

- WEP and enterprise networks: Settings lists them, joining fails
  (EOPNOTSUPP / association failed). WPA v1 (TKIP) works through `wifi.cfg`
  and is detected in Settings scans, but the on-screen join still needs a
  key type mapping in `a80211` beyond the PMK type used by WPA2.
- The status bar's carrier text says "Searching..." (no baseband); an iPod
  shows "iPod" there. Unrelated to Wi-Fi.
- Turning Wi-Fi off in Settings disassociates but leaves the chip powered.
- Scan table: 16 networks, 76 bytes of IEs each.

### 10.8 The two join paths: Settings and "Ask to Join Networks"

Both paths end in the same Apple80211 ASSOCIATE request. Once iOS has accepted
a network, our dylib logs its fields (the new diagnostic line keeps the PMK
out of the log):

```
[a80211] associate "ios3ds-qemu": req_len 384, ssid_len 11, key_len 32, cipher 6, auth 1/8, word44 0
[a80211] associate "ios3ds-qemu" (WPA2-PSK, PMK from WiFiManager, auth 1/8)
[wifi] iOS: join "ios3ds-qemu" (WPA2), in the scan list
[wifi] "ios3ds-qemu" is a WPA2 + WPA3 network: joining with WPA2-PSK
[wifi] link up: ios3ds-qemu, AP 0e:a9:11:c0:ff:ee
[wifid] en0 configured by configd
```

`key_len 32, cipher 6` is `APPLE80211_CIPHER_PMK` (WiFiManager computed
PBKDF2 itself), `auth 1/8` is the scan record's open-system/WPA2 pair. The
popup (SpringBoard's `Select a Wi-Fi Network` alert, raised when Mail or
Safari needs the network and no known one is in range) sends exactly the same
fields. Section 66's fix in the ARM9 driver (`wpa2_rsn_parse`,
`CONNECT_PROFILE_MATCH_DONE`) therefore applies to both paths.

**The difference is before the request:** WiFiManager classifies a network
from the scan record's IEs. A WPA2+WPA3 ("transition mode") RSN IE lists AKM
PSK (2) *and* SAE (8) and sets MFPC; iOS 4.2's parser has no SAE, so it refuses
the network before it issues ASSOCIATE. In QEMU (mixed AP, before this fix)
Settings showed `Attepting Apple80211AssociateAsync` / `Attempting to join
WPA network` / `Failed to associate with ios3ds-qemu: -100`, with no SET 20 at
all; the popup uses the same scan record to decide which networks it may
offer. With plain WPA2 or an open network nothing is rejected.

**Fix:** `wlan_ie_wpa2_view()` in `boot/common/wifi_proto.h` is applied to the
copy of each scan result iOS receives (`fill_result()` in
`tools/ios3ds-wifi/a80211/a80211.c`, GET 11 and GET 201). It keeps the PSK AKM,
drops SAE/PSK-SHA256 AKMs, clears MFPC/MFPR and shortens the RSN element,
including any IEs after it. The ARM9's own scan table is **not** changed: it
still sees the real mixed IE, logs the fallback and sets the CONNECT flags
that make the firmware associate. Pure-WPA3 (SAE-only / MFPR) IEs are left
alone, so they stay unsupported and the ARM9 prints its one-line refusal
(section 66). Plain WPA2 and open IEs are byte-for-byte unchanged.

Host tests (`make -C tools/ios3ds-wifi test`, also in `make test`) cover the
rewrite: mixed with PSK first or second, a following vendor IE, plain WPA2,
SAE-only, open IEs, and a truncated RSN IE. `emu/wifi_popup_test.py` joins the
mixed AP in QEMU through Settings, forgets it, and joins it again from the
SpringBoard alert; `--case wpa2` runs the same flow against a plain WPA2 AP.
Results: re-log 84.

## 11. iOS 2.2.1 (5H11a)

2.2.1 differs from 3.x/4.x in two ways that matter here (re-log, "Wi-Fi on
2.2.1"):

- **No IOUserEthernet** (and no tun/utun) in the 5H11a kernelcache, so
  `ios3ds-wifid` cannot create `en0` from userland. The shim registers it
  itself: `tools/ios3ds-wifi/shim/kif_eth.c`, a Darwin 9 `ifnet`
  (`ifnet_allocate`/`ifnet_attach` with the stock `ether_*` demux/framer,
  `bpfattach` for IPConfiguration's DHCP, `KEV_DL_LINK_ON/OFF` through
  `ifnet_event`, `SIOCGIFMEDIA` answers the link). Frames go through the
  opaque sysctl `kern.ios3ds_eth` (`shim/kif_proto.h`): reads return the
  head of the output queue with a sequence number, writes acknowledge it and
  carry received frames for `ifnet_input`. It is built only when
  `versions.py` sets `kernel_ifnet` (2.2.1), its KPI addresses come from
  `scripts/kpi.py` (`KIF_SYMBOLS`, Thumb bit from `n_desc`), and it lives in
  the shim's GPU part (`link.ld`: `*kif*.o`). The 4.2.1 shim is unchanged.
  `ios3ds-wifid` built with `-DWIFID_KIF` (2.x toolchain flags,
  `build/rootfs-add/ios2/`) uses this sysctl instead of
  `IOEthernetController*`.
- **en0 is not an IOKit object**, so configd would never create a network
  service for it: `rootfs/overlay-2.2.1.json` writes a `preferences.plist`
  with one Wi-Fi service on `en0` (IPv4 DHCP). There is no
  `/usr/sbin/ipconfig` on 2.x, so wifid only waits for the lease.
- **No WiFiManager.** configd's `Aeropuerto.bundle` matches
  `IO80211Controller` and reads the child's `BSD Name`; Settings'
  `AirPortSettings.bundle` and SpringBoard's status bar call the (dynamic)
  `Apple80211.framework` **in their own process**. Apple80211 2.x speaks the
  3.x ioctl protocol (GET 23 event device, 144-byte scan results), so the
  2.x build of `ios3ds-a80211.dylib` (`-DA80211_IOS2 -DDYLD2_DYLIB`) is
  inserted into configd **and SpringBoard** (whose apps inherit
  `DYLD_INSERT_LIBRARIES`). Differences in that build:
  - Aeropuerto gets `IOResources` as its controller; `IORegistryEntrySearchCFProperty(…,
    "BSD Name")` on it answers `en0`.
  - Every process that issues an Apple80211 ioctl starts its own status
    poller (events go to that process's event stream).
  - It talks to `debug.ios3ds_wlan`, a twin of `kern.ios3ds_wlan` that any
    user may write (Settings and SpringBoard run as `mobile`; xnu-1228's
    `__sysctl` wants root for every write below `CTL_KERN` regardless of
    `CTLFLAG_ANYBODY`). Registered by `kif_eth.c`, 2.x only.
  - One SCAN_DONE per scan request (Apple80211 2.x counts outstanding scans
    and completes only when all are answered), and the result list can be
    read again for each.
  - RSSI/noise in scan results are reported as dBm + 100: Apple80211 drops
    results whose RSSI is below the scan's threshold, which Settings leaves
    at 0.
- Verified in QEMU: `emu/wifi_test.py --case ok` (wifi.cfg join, DHCP, DNS,
  HTTP), `emu/wifi_ui_test.py --psk <letters> --settings-icon 275,226
  --wifi-row 160,96 --key-pause 2 --keep-ca` (8/8: Settings lists the three
  networks, WPA2 join with the keyboard, DHCP), Safari loading a page from
  the host by typed URL.
