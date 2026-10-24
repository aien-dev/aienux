# Spark Wi-Fi: Aienux driver for the MediaTek MT7925 (L206)

Status: driver, 802.11 station, WPA handshake layer and chip model written.
Host tests run them against our own chip model and a stand-in authenticator
(receipts with commands and exit codes in section 6); that proves the code
matches our reading of the reference and refuses hostile input, not that the
chip behaves this way. Never run on the chip. Every register sequence below
is from reading the open-source Linux mt76 driver (no public MediaTek
programming manual exists: DOCS SILENT, recorded with `docs-read` on
2026-10-20) and is UNKNOWN on hardware until an attended boot.

Tags: [OBSERVED] read on the Spark, read-only, 2026-10-20. [SOURCE] read in
openwrt/mt76 @467ae2e (BSD-3-Clause-Clear) or hostap @fcccc8c (BSD), both
fetched read-only into ~/workspace/_ext, nothing copied. [STANDARD] IEEE Std
802.11-2020 clause named. [TESTED] a host test checks it against the chip
model (which encodes the same reading, so it proves consistency and refusal
of hostile behaviour, not chip behaviour). [UNKNOWN] needs the real chip.

## 1. Plain-English summary

- The Spark's Wi-Fi is a MediaTek MT7925 card. AienOS now has its own
  driver for it, written from scratch in our tree: it finds the card, maps
  its registers, takes control of its power, resets it, sets up the shared
  memory queues the chip uses to talk to the host, sends the chip its
  firmware, asks it to scan, joins a network and moves internet traffic
  between the radio and the AienOS network stack.
- Open (no password) networks work end to end in the simulator: scan, join,
  then ARP and ping to the gateway through native/net/aienos_net.c.
- Password networks (WPA2/WPA3): the whole handshake is written and host-tested
  (the four-message key exchange, the group-key refresh, the attack checks
  listed in section 5), and the keys go into the chip. A pairwise key refresh
  is not done yet: the station leaves cleanly and must join again (R24). It runs in the
  simulator with stand-in crypto. With our real crypto library it refuses,
  because two pieces of standard crypto do not exist in native/ yet: the
  AES-128 message check (CMAC) and AES key wrap; plain WPA2 also needs SHA-1.
  They are listed in section 5; per the brief we do not hand-roll them here.
- Nothing has touched the real chip. The kernel hook (`make full CK_MT7925=1`)
  is the first attended-boot step: it probes the chip, reads its revision,
  and releases it, with no firmware sent.
- The 802.11 and WPA layers have no MediaTek code in them, so the iMac,
  Raspberry Pi and MacBook Wi-Fi drivers can reuse them.

## 2. What the Spark has

[OBSERVED] `lspci -vvv -s 0009:01:00.0`, sysfs, `modinfo mt7925e`:

| Fact | Value |
| --- | --- |
| PCI function | 0009:01:00.0, 14c3:7925 rev 00, subsystem 1a3b:6374 (AzureWave) |
| Class | 0x028000 (network controller, other) |
| BAR 0 | 64-bit, non-prefetchable, 2 MiB at 0x73a00000 |
| BAR 2 | 32 KiB at 0x73c00000 (not used by the reference) |
| Link | PCIe Gen2 x1 (5 GT/s) |
| DMA | 32-bit mask (reference), 1 MSI vector (irq 428 under Linux) |
| IOMMU | group 17 on smmu3 @0x13800000 |
| Reset | FLR and bus reset available |
| Linux driver | mt7925e, `Dual BSD/GPL`, parameter disable_aspm |
| Interface | wlP9s9 |

Firmware: docs/MT7925_FIRMWARE_INVENTORY.md.

## 3. Code map

| File | What |
| --- | --- |
| native/net/aienos_mt7925_fw.{h,c} | Firmware container parsers (ROM patch, RAM code, CLC region), no device access |
| native/net/aienos_mt7925.{h,c} | Chip driver: identity, register remap, power own, WFSYS reset, five WFDMA rings, MCU commands (legacy and UNI), firmware download, scan, data TX (cut-through TXD+TXP with token hold), TX_FREE, RX descriptor parse, stop |
| native/net/aienos_mt7925_sta.{h,c} | Join commands (NIC capability, DEV_INFO, BSS_INFO, ROC, STA_REC, KEY) and the adapter to the station |
| native/net/aienos_wifi.{h,c} | IEEE 802.11 frames, elements, RSN element, BSS table, 802.3 to 802.11 and back. Chip independent |
| native/net/aienos_wifi_sta.{h,c} | Station: scan table, open authentication, association, 4-way handshake, key install, Ethernet bridge. Chip independent |
| native/net/aienos_wpa.{h,c} | EAPOL-Key codec, 802.11 KDF-SHA256, supplicant 4-way and group handshakes over a crypto table. Chip independent |
| native/net/tests/mt7925_model.{h,c} | Chip model: registers, remap windows, rings in an SMMU-like window, MCU, radio, access points with an authenticator |
| native/net/tests/wpa_toy.h | TEST DOUBLE for the missing MIC and key wrap, never linked into the kernel |
| native/net/tests/{mt7925_fw,mt7925,wifi,wpa}_test.c | Host tests |
| native/kernel/dev/mt7925_bind.{h,c} | Kernel binding, CK_MT7925=1 image only |

## 4. Register and protocol sequences (all UNKNOWN on hardware)

Each row: what the driver does, where it comes from, how it is tested, and
the attended-boot experiment that turns it from UNKNOWN into OBSERVED.

| # | Sequence | Source | Host test | Attended-boot experiment |
| --- | --- | --- | --- | --- |
| R1 | Probe: memory decode + bus master on only after the SMMU window is set; BAR 0 only; window below 4 GiB | SOURCE mt7925/pci.c probe (32-bit DMA mask) | TESTED identity refusals; kernel bind compiles | Boot CK_MT7925=1: expect `smmu_dma_window: mt7925 only` then `dma_gate: mt7925 granted` |
| R2 | Register remap: offsets below 0x200000 direct; fixed table; 0x18000000-0x18c00000, 0x70000000-0x78000000, 0x7c000000-0x7c400000 through L1 (0x155024 high half = addr>>16, window 0x130000 + low 16 bits); everything else L2 (L1 base 0x1850, register 0x120 = addr, window 0x130000) | SOURCE mt7925/pci.c fixed map, mt792x remap | TESTED every table row and both windows | Read CHIPID 0x70010200 through L1: expect 0x7925 in the low half |
| R3 | Firmware own then driver own: LPCTL 0x7c060010 write bit 0, poll OWN_SYNC (bit 2) set, 10 tries x 50 ms; write bit 1, poll OWN_SYNC clear | SOURCE mt792x_core.c fw_pmctrl/drv_pmctrl | TESTED incl. stuck-sync faults | Log tries used for each handshake |
| R4 | Chip revision = CHIPID 0x70010200 << 16 \| (REV 0x70010204 & 0xff) | SOURCE mt7925/pci.c | TESTED (model 0x79250000) | Printed by `mt7925: init ok chip_rev=` |
| R5 | EMI sleep protection: 0x18011100 set bit 1 | SOURCE mt7925/pci.c | TESTED trace | Read back after set |
| R6 | WFSYS reset: 0x7c000140 clear bit 0, hold 50 ms, set bit 0, poll bit 4 (init done) up to 500 ms | SOURCE mt792x_dma.c wfsys_reset | TESTED incl. never-done fault | Log time to init done |
| R7 | Interrupt mask: HOST_INT_ENA (0xd4204) = 0, PCIE MAC INT 0x10188 = 0xff; polled driver keeps host interrupts masked | SOURCE mt7925/pci.c | TESTED | Confirm no MSI fires during the probe |
| R8 | Rings: TX0 data 2048 B slots, TX15 MCU, TX16 firmware download, RX0 events, RX2 data; registers at 0xd4300 + q*0x10 (TX) and 0xd4500 + q*0x10 (RX): base, count, cpu index, dma index; 16-byte descriptors {buf0, ctrl, buf1, info} | SOURCE mt792x_dma.c, mt76 dma.h | TESTED incl. out-of-window, bad index, overlong RX faults | After init read back each ring base and count |
| R9 | WFDMA enable: prefetch EXT_CTRL per ring, reset DTX/DRX pointers, PRI_DLY 0, GLO_CFG (0xd4208) flags (TX_WB_DDONE b6, FIFO_LE b12, CLK_GAT_DIS b30, OMIT_TX_INFO b28, DMA_SIZE=3 b5:4, FIFO_DIS_CHECK b11, RX_WB_DDONE b13, DISP_BASE_PTR_CHAIN b15, OMIT_RX_INFO_PFET2 b21) then TX_DMA_EN\|RX_DMA_EN; 0x7c0242b4 bit 28; INT_RX_PRI \|= 0xf00, INT_TX_PRI \|= 0x7f00; DUMMY_CR 0x54000120 bit 1; MCU2HOST_SW_INT_ENA 0xd41f4 bit 0 | SOURCE mt792x_dma.c dma_enable | TESTED trace order | Read back GLO_CFG after enable |
| R10 | Stop: HOST_INT_ENA 0, clear GLO_CFG enables, poll busy bits 1 and 3 clear (100 ms), firmware own | SOURCE mt792x_dma.c dma_disable + fw_pmctrl | TESTED | `mt7925: chip stop ok` and time to idle |
| R11 | MCU command framing: 32-byte TXD (len \| PKT_FMT CMD << 23 \| Q_IDX 0x20 << 25; HDR_FORMAT CMD << 16) + legacy or UNI header; sequence 1..15; event header 44 bytes | SOURCE mt76_connac_mcu.c, mt7925/mcu.c | TESTED incl. stale-sequence and silent-MCU faults | First command after reset: NIC capability query answer |
| R12 | Firmware download (docs/MT7925_FIRMWARE_INVENTORY.md section 5) | SOURCE mt76_connac2 load_patch/load_ram | TESTED order, addresses, lengths, SHA-256 of every scattered piece, against the real files too (`mt7925-fw-real`) | OPERATOR STEP: download, expect N9 ready (CONN_ON_MISC bits 1:0 = 3) |
| R13 | NIC capability query, MAC from the reply (efuse) | SOURCE mt7925/mcu.c | TESTED parse and refusal | Compare MAC with Linux's wlP9s9 address |
| R14 | CLC (country limits) send | SOURCE mt7925/mcu.c set_clc | parser TESTED, command not sent yet | UNKNOWN: which country code to use; needs Drake's location setting |
| R15 | Scan: UNI SCAN_REQ with SSIDs and channel list; results as beacons/probe responses on RX2 | SOURCE mt7925/mcu.c hw_scan | TESTED | Passive scan: list nearby networks |
| R16 | Join: DEV_INFO, BSS_INFO (basic + RLM), ROC acquire and grant, STA_REC, auth/assoc frames on TX0, STA_REC state update, ROC abort | SOURCE mt7925/main.c, mcu.c | TESTED incl. silent ROC, STA failure, AP refusals | Join an open network, then the home network |
| R17 | Data TX: cut-through TXD (32 B) + TXP (32 B), frame by pointer, token held until TX_FREE returns it | SOURCE mt7925/mac.c, mt76_connac_mac.c | TESTED incl. bogus/short TX_FREE, tokens never freed | Ping the gateway; count TX_FREE per frame |
| R18 | RX parse: RXD groups 4/1/2/3/5, frame bounds, decrypted flag from SEC_MODE | SOURCE mt7925/mac.c | TESTED incl. 50000 random and mutated descriptors | Capture RXD words of 10 frames |
| R19 | Key install: STA_REC KEY_V3 (CCMP-128), pairwise id 0, group 1..3; command buffer and MCU slot zeroed after | SOURCE mt7925/mcu.c add_key | TESTED keys match the AP's in the model | Join a WPA2 network once the crypto exists |
| R20 | MSI and interrupts | SOURCE mt7925/pci.c | not used (polled) | UNKNOWN: MSI vector routing through the GIC ITS; experiment after polled path works |
| R21 | IORT stream id for 0009:01:00.0 on smmu3 | OBSERVED IOMMU group | via ck_dma_confine | `smmu_dma_window` line prints the stream id |
| R22 | ASPM: the reference offers disable_aspm | SOURCE mt7925/pci.c | not touched | UNKNOWN: whether L1 entry breaks MMIO on this card |
| R23 | GTK from message 3 / group message 1: exactly 16 bytes (the group cipher is always CCMP-128 here), key id 1..3, checked before any reply | STANDARD 12.7.2 Figure 12-35 | TESTED (wpa_test NOGTK, PAD, group rekey) | Join a WPA2 AP whose group cipher is CCMP; a TKIP-group AP is refused by the RSN layer |
| R24 | PTK rekey (a protected message 1 after the handshake) is not done: the station deauthenticates (reason 1) so the caller reconnects | STANDARD 12.7.6.2 | TESTED (mt7925_test station_wpa) | Set a short PTK rekey interval on a test AP; expect a clean leave and reconnect, then implement rekey in place |
| R25 | Key removal on leave: no chip key-delete command is wired; keys stay in the chip until the next join rewrites the station record | SOURCE mt7925/mcu.c add_key (cmd remove) | not tested | UNKNOWN: whether a stale WCID key survives STA_REC removal; read back with a sniffer after leave, then wire the remove command |
| R26 | Re-entrancy: data frames that arrive on RX0 while the driver waits for an MCU reply are held (4 deep) and delivered after the reply, before any newer ring frame; every callback runs after its ring slot is consumed, on a per-level copy, so a callback that polls again (TX ring full, key install) cannot see the slot twice; nesting stops at 4 levels, deeper frames dropped and counted (rx_nest_dropped) | SOURCE mt7925 event path; our design | TESTED (deferred-frame block in station test, test_rx_nested) | Count rx_deferred / rx_defer_dropped / rx_nest_dropped during a busy join |
| R27 | Key data padding: 0xdd then zeros to an 8-byte boundary, anything else refused | STANDARD 12.7.2 | TESTED (wpa_test PAD) | Interop: some APs may pad differently; record any refusal with the frame |
| R28 | RSNXE (WPA3 extensions) in message 3 is not compared with the beacon's | STANDARD 12.7.6.4 | not tested | Needed with SAE-H2E; the SAE lane adds the check |
| R29 | Leaving after a failed handshake: deauthentication with reason 15 (4-way timeout), or 1 for any other failure after association | STANDARD Table 9-49 | TESTED (wrong PSK, bad MIC, downgrade, silent AP); the key-install failure in RUN (group rekey) is not tested, the model has no group rekey | Watch the AP log for the reason code |
| R30 | EAPOL source must be the BSSID (the authenticator); once the pairwise key is in, cleartext EAPOL is dropped | STANDARD 12.7.6 | TESTED (forged-source and cleartext message 1) | none needed beyond R16 |
| R31 | CCMP replay check: the packet number comes from RXD group 1 bytes 0..5 (PN0..PN5); the station keeps the last one per TID (16 = non-QoS), pairwise and group, resets on key install, and drops a decrypted data frame whose number is missing or not above the last | SOURCE mt7925/mac.c (group 1 to status->iv); STANDARD 12.5.3.4.4 | TESTED (replayed ARP reply dropped; chip without group 1: protected data dropped) | UNKNOWN: whether the chip always sets group 1 on decrypted frames, and the byte order; log the first 20 PNs after a join. A group rekey can drop old-key frames briefly (one group counter set across key ids) |
| R32 | Broadcast deauthentication / disassociation from our BSSID (an AP restart) is honoured like a unicast one | STANDARD 11.3.4.5 | TESTED (accepted from the BSSID, ignored from another) | none |

## 5. Security: what exists and what is missing

Chip side: CCMP-128 encrypt and decrypt run inside the chip once a key is
installed (R19). Host side, the 4-way handshake needs [STANDARD 12.7]:

| Primitive | Needed for | In native/? |
| --- | --- | --- |
| Random (SNonce) | all | the kernel has it (ck_entropy_fill over the Arm RNDR instruction, refusing when absent; whether GB10 has RNDR is UNKNOWN until the boot prints `cpu_features ... rndr=`). The station takes it through `wifi_radio.random`; no entropy, no RSN join. The MT7925 kernel binding does not attach the station yet, so it is not wired there |
| KDF-SHA256 | PTK for AKM 6 (PSK-SHA256) and AKM 8 (SAE) | yes: wpa_kdf_sha256 over the in-house HMAC-SHA256, checked against an OpenSSL-computed reference |
| AES-128 block cipher | CMAC and key wrap | **missing** (native/crypto has AES-256 only) |
| AES-128-CMAC | EAPOL-Key MIC for AKM 6 and 8 | **missing** |
| AES Key Wrap (RFC 3394) | GTK delivery in message 3 and group message 1 | **missing** |
| SHA-1, HMAC-SHA1-128, PRF-SHA1 | AKM 2 (plain WPA2-PSK, the most common home setting) | **missing** |
| PBKDF2-HMAC-SHA1 | passphrase to PMK (any PSK network) | **missing**; a raw 32-byte PSK works without it |
| P-256 arithmetic, SAE commit/confirm | WPA3-SAE | **missing** |
| BIP-CMAC-128 | protected management frames | **missing** (chip may do it; UNKNOWN) |

Until these land (crypto lane, with their own test vectors and review), a
password network is refused before any frame goes out, and
`wifi_sta_missing_crypto` / `wpa_crypto_missing` name exactly what is
absent. The supplicant checks, all tested: key-info shape per message,
descriptor version per AKM, replay counter strictly increasing and only moved
by MIC-verified messages, ANonce equality, constant-time MIC check before any
key data is used, unwrap integrity, RSNE in message 3 equal to the beacon's
(downgrade), pairwise key length, GTK exactly 16 bytes with key id 1..3, no
reinstallation of an installed key, EAPOL only from the BSSID and only
protected once the pairwise key is in, deauthentication (reason 15) when the
handshake fails, a PTK rekey answered by leaving (R24), the CCMP replay check (R31),
message 4 sent before the pairwise key is installed, secrets wiped.

Review fixes 2026-10-24 (L822, independent review before merge):
- The group replay counter starts at the AP's Key RSC from message 3 / group message 1
  (802.11-2020 12.7.6.4), not 0, so broadcast frames recorded before the join are replays.
- Protected management frames: no IGTK, BIP or SA Query exists, so plain WPA2-PSK (no MFPC) is chosen
  whenever a network offers it; a WPA2/WPA3 transition network is joined that way instead of being
  refused for SAE. On an AKM 6 link (MFPC sent) deauth/disassoc end the link only when unicast,
  decrypted under the PTK and with a fresh packet number; broadcast ones (BIP) are refused and counted
  in `mgmt_unprotected_dropped`. Still missing: our own deauth on such a link goes out unprotected, so
  the AP may keep a stale association (status 30 on the next join, UNKNOWN on the real AP).
- MCU command header: TXD1 HDR_FORMAT moved to bits 15:14 (connac3, as mt7925/mcu.c) from 17:16
  (connac2); HIF_CTRL and CHIP_CONFIG go without the ACK option bit, as the reference. The chip model
  now refuses both mistakes (the old driver fails 373 model checks).

## 6. Receipts

Run 2026-10-20 (~21:00Z) on the Spark host, on top of 595ed72 with the advisor-2 fixes
(replay check R31, broadcast deauth R32, epoch guard, ordered drain), through `quiet-build`
(script /tmp/l206-run7.sh, run from native/):

| Command | Exit | Result |
| --- | --- | --- |
| `make -C net test` | 0 | MT7925_FW_TESTS PASS (948 checks), MT7925_DRIVER_TESTS PASS (7245 checks), wifi_test PASS (11046 checks), AIENOS_WPA_TEST PASS (433 checks, 0 failures); the rest of native/net 0 failures (log /tmp/l206b-test.log) |
| `make -C net sanitize` (ASan + UBSan, halt on error) | 0 | the same four suites PASS with the same counts |
| `make -C net mt7925-fw-real` (read-only parse of /lib/firmware/mediatek/mt7925) | 0 | MT7925_FW_REAL PASS; RAM code 5 regions (4 downloaded, region 4 host-only CLC); MT7925_MODEL_REAL_FW PASS: 6 downloads, 1275392 scatter bytes, 11 MCU commands, start addr 0x0090d000 option 0x1 |
| `make -C kernel full CK_MT7925=1 OUT=../target/l206b-mt7925 -j8` | 0 | BOOTAA64.EFI built; `nm` shows the ck_mt7925_* binding and mt7925_init linked. Not booted |

QEMU gates, default images, `scripts/ck_gates.sh` on a clean checkout of be9e2f4 (QEMU only, physical NOT_RUN). The shared gate lock was held by other agents, so the run came in three parts, each exit 0, no gate FAILed:

| Run | Result | Receipt (evidence/ck_gates_<prefix>...json) |
| --- | --- | --- |
| all 27 | 11 PASS (M1 M3 SMMU NVME_SHUTDOWN P2_ARTIFACT M4_NVME M4_STORE M4_STORE_CRASH ARGUS1_REVOKE NET SMP), 16 NOT_RUN (lock held) | 4719dfa5 |
| `--only` the 16 | 15 PASS (M0_ROLLBACK OSC_UNIT OSC_LAUNCH OSC_RECEIPT OSH_CORE OSH_OP M4_CONTINUITY M4_RECOVERY M4_ALLEN KEYBOARD DISK_LAYOUT FPU INFER SCREEN CONSOLE), SLOT_LOADER NOT_RUN | 6cc7cda6 |
| `--only SLOT_LOADER` | PASS | 06a649aa |

Together: 27 of 27 PASS on be9e2f4; the default images do not contain the MT7925 driver (only `CK_MT7925=1` builds it), so these gates show the driver files do not disturb the default build. The receipts are in the gates worktree, not committed here.
What these do not show: anything about the real chip (every row in
section 4 stays UNKNOWN until the attended boot).

## 7. Attended boot plan (operator steps, in order)

1. `make -C native/kernel full CK_MT7925=1` (image: target/native-kernel/full-mt7925/BOOTAA64.EFI); boot it the attended way the
   Spark boot package lane (L197) documents. Expected console lines:
   `mt7925: found seg 0009 01:00.0`, `smmu_dma_window: mt7925 only ...`,
   `mt7925: init ok chip_rev=0x7925....`, `mt7925: chip stop ok`,
   `devices: mt7925=probed-released (rc=0)`. Any other line is a finding.
2. With the boot package carrying the two firmware files (section 6 of the
   inventory): firmware download (R12), NIC capability (R13).
3. Passive scan (R15), open-network join (R16), ping (R17, R18).
4. After the crypto lane: WPA2 join (R19).
