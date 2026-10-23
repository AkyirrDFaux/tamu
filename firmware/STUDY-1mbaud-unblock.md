# What unblocks RSBus at 1 MBaud: UART DMA vs bigger buffers

Read-only study (no firmware edits, no flash, no device writes). Branch
`exp/rsbus-1mbaud-ring` @ 59de043, tree clean. Every number below is either quoted from the
build output, from the IDF source in `~/.platformio/packages/framework-espidf` (IDF 6.0.1,
target esp32c3), or measured on the attached rig through the app interface (read-only).

**Second pass, 2026-10-23 15:19–15:22: every number re-derived from an independent build of the
same commit.** A detached worktree at `exp/rsbus-1mbaud-ring` @ 59de043 was built from scratch
(`git worktree add --detach .wt-1mbaud exp/rsbus-1mbaud-ring`, `managed_components` copied in
because it is untracked) and reproduces the figures below exactly:

    pio run -e Tamu_v2_0A -e DAS_v0_1
      Tamu_v2_0A  RAM:   25.2% (used 82572 bytes from 327680)
                  Flash: 24.3% (used 668072 bytes from 2752512)     [SUCCESS 39.03 s]
      DAS_v0_1    RAM:  100.0% (used 2048 bytes from 2048)
                  Flash: 95.7% (used 13716 bytes from 14336)        [SUCCESS 1.17 s]
    riscv32-esp-elf-size .pio/build/DAS_v0_1/firmware.elf
      14084  116  1968  16168  3f28                                (text/data/bss/dec/hex)
    ls -l .pio/build/DAS_v0_1/firmware.bin          13720 bytes
    grep "_etext = " .pio/build/DAS_v0_1/firmware.map   PROVIDE (_etext = .)  0x3d24
    riscv32-esp-elf-nm -S .pio/build/Tamu_v2_0A/firmware.elf
      3fc9608c 00000400 b _ZL5RxDma        3fc9508c 00001000 b _ZL6RxRing
    riscv32-esp-elf-size on the peripheral objects
      esp_driver_uart/src/uhci.c.o    text 7693

**The shared checkout is no longer on this branch**, and that matters for reproducing the sizes: a
concurrent merge worker moved `/home/akyirr/Project/Fluff/Tamu` from `exp/rsbus-1mbaud-ring` to
`master` at 15:17 and cherry-picked the DAS compare-wrap as `6c38310` (`9c0f5d7` is master's head
with version stamps). Building that tree gives **RAM 77,372 / Flash 662,888** — 5,200 B of RAM
less than the branch, of which 5,120 B is `RxRing` (4,096) + `RxDma` (1,024), an independent
confirmation that the UHCI ring is what the branch adds. Use a worktree (recipe in §6) or
`git checkout exp/rsbus-1mbaud-ring` to get the numbers above.

**Third pass, 2026-10-23 15:29–15:31: every size re-derived again from a clean detached worktree, plus a
fresh read-only rig reading.** Shared checkout on `master` @ `9c0f5d7` (the only change in the tree is
this untracked study file); the experiment is `exp/rsbus-1mbaud-ring` @ `59de043`. All output below is
from this pass, not copied from the earlier two.

    cd firmware && pio run -e Tamu_v2_0A -e DAS_v0_1            # master (no UHCI ring)
      DAS_v0_1    RAM: 100.0% (used 2048 from 2048)     Flash: 95.6% (used 13708 from 14336)
      Tamu_v2_0A  RAM:  23.6% (used 77372 from 327680)   Flash: 24.1% (used 662888 from 2752512)
      2 succeeded in 00:00:04.323

    git worktree add --detach .wt-1mbaud exp/rsbus-1mbaud-ring   # 59de043
    cp -r firmware/managed_components .wt-1mbaud/firmware/       # untracked, needed offline
    cd .wt-1mbaud/firmware && pio run -e Tamu_v2_0A -e DAS_v0_1  # branch
      DAS_v0_1    RAM: 100.0% (used 2048 from 2048)     Flash: 95.7% (used 13716 from 14336)  [1.21 s]
      Tamu_v2_0A  RAM:  25.2% (used 82572 from 327680)   Flash: 24.3% (used 668072 from 2752512) [39.10 s]

    riscv32-esp-elf-size .pio/build/DAS_v0_1/firmware.elf   14084  116  1968  16168  3f28
    ls -l .pio/build/DAS_v0_1/firmware.bin                  13720
    grep "_etext = " .../DAS_v0_1/firmware.map              PROVIDE (_etext = .)  0x3d24
    riscv32-esp-elf-nm -S .pio/build/Tamu_v2_0A/firmware.elf
      3fc9608c 00000400 b _ZL5RxDma     3fc9508c 00001000 b _ZL6RxRing
    riscv32-esp-elf-size .../esp_driver_uart/src/uhci.c.o   7693

The branch adds 5,200 B of RAM over master (82,572 − 77,372) and 5,184 B of flash. 5,120 B of that RAM
is the two buffers exactly (`nm`: 0x1000 and 0x400, both in `.bss`); the remaining 80 B is other static
data on the UHCI path — so the second pass's "exactly the 5,120 B" is 80 B too neat, and the conclusion
(the ring is what the branch costs) is unaffected.

Rig, read-only (Device Ping + Register Read only; no write, no flash, no reset), images unchanged from
the passes above — core `0026-10-23:14`, DAS `0026-10-23:3`, core up 3,835 s, DAS up 3,854 s:

    firmware/.pio/probe-venv/bin/python firmware/.pio/probe/study2_probe.py 40 30
      core time      uptime=3835085 ms  loop=196768  max_loop=262144  (raw Q16.16 = 3.003 / 4.000 ms)
      DAS ping       n=40/40 median=7.27 ms p95=7.83 min=6.07 max=8.25 timeouts=0
      DAS read SN    n=40/40 median=7.49 ms p95=8.01 min=6.47 max=8.25 timeouts=0
      DAS read RAM   n=40/40 median=7.50 ms p95=7.96 min=6.38 max=8.04 timeouts=0
      core ping      n=40/40 median=2.99 ms p95=4.11 min=2.38 max=5.58 timeouts=0
      loss vs spacing  gap 0 ms: 0/30   gap 20 ms: 0/30   gap 200 ms: 0/30

Third pass, same verdict as the first two: no loss at gap 0, and the DAS round trip sits at the ~3 ms
loop quantisation rather than at the wire. Those 15:22 numbers were printed to a terminal and were
**not** captured to a file; the citable stored outputs are the two runs quoted in §2
(`study2_result.txt` at 15:07, `study2_result_verify.txt` at 15:19), which agree with them within
0.1 ms.

Code/IDF facts this pass verified directly (not inherited): `LedNum = 86`
(`Blocks/Vysi1Layout.h:111`), the bit-bang mask `portENTER_CRITICAL` + NOP sums 36+108+64 = 208 (mixed),
144+64 = 208 (both-1), 36+144 = 180 (both-0) with 1–2 `GPIO.out_w*` stores (`Devices/Tamu_v2.0A/LED.h:71-135`),
its single call site in the main loop (`Main.h:283`), `BUFFER_SIZE 260` (`Devices/DAS_v0.1/RSBus.h:4`),
`SleepMicro(1000)` before the DE drop (`Devices/DAS_v0.1/RSBus.h:148`, the DE drop itself `:150`),
`RSBUS_RX_TASK_STACK 2048` (`Tamu_v2.0A/RSBus.h:42`), `SOC_UART_FIFO_LEN (128)`/`SOC_UART_NUM (2)`
(soc_caps.h:379/377), `SOC_UHCI_SUPPORTED 1` (:30), zero hits for `SOC_UART_SUPPORTS_DMA` under
`framework-espidf/components/`, `uhci_new_controller`/`uhci_receive`/`uhci_transmit`/
`uhci_wait_all_tx_transaction_done` exported by `esp_driver_uart/include/driver/uhci.h`.
`framework-espidf/version.txt` = 6.0.1. The worktree was removed again afterwards
(`git worktree remove --force .wt-1mbaud`).

## Answer

**Getting the CPU out of the byte path — UART DMA, not bigger buffers.** The buffer that overruns
is 128 B of silicon and cannot be enlarged; the buffer that *can* be enlarged is only useful if a
peripheral fills it, i.e. if the CPU is not the one moving bytes.

| option | does it remove the 128-byte-FIFO overrun? | does it remove the 2.5–2.9 ms interrupt mask? | verdict |
|---|---|---|---|
| A. UART DMA (UHCI + GDMA RX/TX) | **yes** — the DMA fills a 1,024 B buffer while the CPU is masked: 10.24 ms at 1 MBaud, 3.5–4.1× the mask | no | **built on the branch `exp/rsbus-1mbaud-ring`; no loss in any run whose output is stored; not in `master`** |
| C. bigger buffers | **no** — the constrained buffer is the FIFO, and `rx_buffer_size` sits behind the mask, not in front of it | no | rejected, with numbers (§3) |
| neither | — | — | 1 MBaud on its own is worth ~4 % measured (§4.2); the mask is the real target and it is out of scope here |

A is what makes 1 MBaud *survive*; it is not what makes it *pay*: today the wire rate hides inside
the LED window, so the rate itself buys ~3–4 %, not the 8–11 % a wire-only model predicts (§4.2).

Revisit material (out of scope by owner decision): Akyirr ruled RMT-driven WS2812 out on 2026-10-23,
so it is not a recommendation here. Recorded for a future card, not for this one: the C3 has 2 TX
channels × 48 symbols (`soc_caps.h:235`) and no RMT DMA (`SOC_RMT_SUPPORT_DMA` exists only for the
S3/P4), and `rmt_tx.c.o` 10,260 B + `rmt_common.c.o` 2,794 B are already in the build tree.
Revisiting it means a card of its own with a prototype.

## 1. The blocker, re-stated in numbers (from the tree, not re-derived)

`firmware/src/Devices/Tamu_v2.0A/LED.h` bit-bangs 86 LEDs × 24 bits inside
`portENTER_CRITICAL`. Per bit slot the code runs 180 NOPs (both-0), 208 (both-1) or 208 (mixed),
plus 1–2 `GPIO.out_w*` stores and the loop overhead — the first study disassembled it at
194–224 cycles/bit, i.e. 1.21–1.40 µs at 160 MHz:

    86 × 24 = 2064 bit slots × 1.21–1.40 µs = 2.50–2.89 ms  (+ 80 µs reset latch)

Hardware facts (IDF 6.0.1, `components/soc/esp32c3/include/soc/soc_caps.h`):

    #define SOC_UART_FIFO_LEN  (128)   /* The UART hardware FIFO length */
    #define SOC_UART_NUM       (2)

so a masked window costs, in FIFO occupancy:

| rate | byte time | 128 B FIFO covers | 129 B frame | 2.50 ms mask | 2.89 ms mask |
|---|---|---|---|---|---|
| 460800 | 21.7 µs | 2.78 ms | 2.80 ms | 115 B → 13 B slack | 133 B → **5 B over** |
| 1000000 | 10.0 µs | 1.28 ms | 1.29 ms | 250 B → **122 B over** | 289 B → **161 B over** |

Measured on the rig (System block field 3, Q16.16 ms, core 1; raw output
`firmware/.pio/probe/study2_result.txt`):

    core loop avg 3.007 ms, max 4.000 ms      (197093/65536, 262144/65536)

i.e. the main loop is ~3.0 ms and ~2.7 ms of it is the masked bit-bang — **~90 % of the core's
time is spent bit-banging LEDs with interrupts off**, which is also why every bus transaction is
quantised to ~3 ms.

## 2. Option A — UART DMA on the core

**API.** In IDF 6.0.1 there is no UART-driver DMA path at all: `SOC_UART_SUPPORTS_DMA` does not
exist anywhere under `components/` (it was an S3-only macro in IDF 4.x). The C3's route is the
UHCI peripheral driving an AHB GDMA channel in both directions:

    components/soc/esp32c3/include/soc/soc_caps.h:30:  #define SOC_UHCI_SUPPORTED  1
    components/esp_driver_uart/include/driver/uhci.h:  uhci_new_controller, uhci_receive,
                                                       uhci_transmit,
                                                       uhci_wait_all_tx_transaction_done

**This is already implemented** on `exp/rsbus-1mbaud-ring` (`firmware/src/Devices/Tamu_v2.0A/RSBus.h`,
commit ea57052) — **and only there: `master` still runs `uart_driver_install()` + `uart_read_bytes()`
at 460800**, so on `master` the 1 MBaud overrun of §1 is unmitigated. The branch adds: a
4 KB software ring fed by the RX-done ISR, a 1 KB DMA transaction buffer, a 2 KB re-arm task,
RX transaction ended by the UART idle EOF (`rx_eof_flags.idle_eof = 1`), TX via `uhci_transmit`
plus a wire-empty poll of `UART1.status.txfifo_cnt`/`fsm_status.st_utx_out` before dropping DE.
The byte-callback architecture survives unchanged: `ReceivePacketFrame()` in
`Core/Functions/Bus.h` still pulls one byte at a time, only now from `RS485RxReadByte()` over the
ring instead of `uart_read_bytes()`.

**Cost, measured from the build** (`riscv32-esp-elf-nm -S` on `firmware.elf`):

    _ZL6RxRing   0x1000 = 4096 B   (RSBUS_RX_RING, .bss)
    _ZL5RxDma    0x0400 = 1024 B   (RSBUS_RX_TRANS, .bss, 4-byte aligned)
    rs485_rx task stack  2048 B    (heap)
    --------------------------------
    ~7.2 KB of the C3's 400 KB, plus ~1 KB of UHCI driver objects (controller, 2 GDMA channels,
    two 2-node link lists, per-node bookkeeping arrays) allocated from internal heap at init.

Flash: the whole image is 668,072 B of 2,752,512 B (24.3 %), RAM 82,572 of 327,680 (25.2 %) —
`pio run -e Tamu_v2_0A`. `uhci.c.o` is 7,693 B. Nothing here is a budget question.

**Does it actually hold at 1 MBaud?** Yes — at whatever rate the flashed images carry (§7): no run
whose raw output is stored records a loss, and the round trip does not rise. Two stored runs of
`study2_probe.py` — latency n=60 (`firmware/.pio/probe/study2_result.txt`, 15:07) and n=40
(`study2_result_verify.txt`, 15:19), loss cases n=40 and n=30 per spacing — read-only requests only:

    DAS ping             n=60/60 median=7.29 ms p95=7.82 timeouts=0    (verify: 40/40, 7.19 ms)
    DAS read SN(14B)     n=60/60 median=7.46 ms p95=8.23 timeouts=0    (verify: 40/40, 7.44 ms)
    DAS read RAM(8B)     n=60/60 median=7.49 ms p95=8.09 timeouts=0    (verify: 40/40, 7.47 ms)
    core ping (ref)      n=60/60 median=2.96 ms p95=3.54 timeouts=0    (verify: 40/40, 2.97 ms)
    loss vs spacing      gap 0: 0/40  gap 20: 0/40  gap 200: 0/40      (verify: 0/30 each)

Each figure is the median of that run's samples (`statistics.median`, `study2_probe.py`); the two
runs are the same code and the same images (core `0026-10-23:14`, DAS `0026-10-23:3`) and agree to
≤0.1 ms.

The re-run's raw block, for the record (n=40; `study2_result_verify.txt`), unmodified code,
read-only requests only:

    core version           0026-10-23:14      DAS version 0026-10-23:3
    core loop 199082/65536 = 3.038 ms avg, max_loop 524288/65536 = 8.000 ms
    DAS ping             n=40/40 median=7.19 ms p95=7.99 min=6.03 max=8.02 timeouts=0
    DAS read SN(14B)     n=40/40 median=7.44 ms p95=7.95 min=6.56 max=8.34 timeouts=0
    DAS read RAM(8B)     n=40/40 median=7.47 ms p95=8.01 min=6.41 max=10.18 timeouts=0
    core ping (ref)      n=40/40 median=2.97 ms p95=3.51 timeouts=0
    loss vs spacing      gap 0 ms: 0/30   gap 20 ms: 0/30   gap 200 ms: 0/30

Same conclusion: no losses at gap 0, the DAS round trip sits at the 3 ms loop quantisation, not at
the wire. The one figure that moved is `max_loop`: 4.000 ms at the first reading, 8.000 ms now.
`max_loop` is a running maximum over uptime (core up 3.24 Ms here), so it only ever grows — it
records that some loop iteration took two LED windows, not that the average changed (3.007 ms
then, 3.038 ms now).

Before this change the same rate overran by 122–161 B per frame (first study).

**Withdrawn, not quoted:** `rate_probe.py` — the only probe with a file-table case — was run in an
earlier pass and reported `n=120/120` ping and `n=117/120` `.TABLE` with 3 timeouts, but its output
was never stored and its printed "median" is `min()` of two round medians over `n` samples per round
(`rate_probe.py:53-60`), not a median of `2n` samples. Those figures, and the 3 `.TABLE` losses in
them, are dropped from this report rather than cited without a file; nothing below depends on them.

### 2.1 The RX lifecycle: a transaction re-armed per frame

A UHCI receive is a **transaction, not a ring**. When one ends, the driver calls the event callback
(`components/esp_driver_uart/src/uhci.c:154`), then stops and resets the GDMA channel (`:158`, `:159`),
and only afterwards returns its RX state machine to `UHCI_RX_FSM_ENABLE` (`:161`) with
`node_index = 0` (`:162`). `uhci_receive()` refuses to start unless that state is ENABLE — it is an
`atomic_compare_exchange_strong(..., "controller not in enable state")` (`uhci.c:312`) — so the callback
cannot re-arm itself. On this build the re-arm is a task notification: `OnUhciRx` sets `RxNeedArm` and
notifies (`Tamu_v2.0A/RSBus.h:78-83`), `RS485RxTask` blocks in `ulTaskNotifyTake` (`:118`) and calls
`RS485RxArm()` (`:92-108`), which retries `uhci_receive(BusUhci, RxDma, sizeof(RxDma))` (`:99`) with a
10 µs backoff, bounded at 2 ms. The task runs at priority 6, above the application loop at 5, and the
first arm is the one in `SetupRS485` (`:236`).

With `rx_eof_flags.idle_eof = 1` (`RSBus.h:228`) a transaction ends at **every inter-frame gap** on a
half-duplex bus, so the arm rate is one per exchange. Between the end of one transaction and the
successful arm of the next nothing but the 128-byte FIFO is listening — and because
`portENTER_CRITICAL` on this port masks *all* interrupts (`portmacro.h:523` → `vPortEnterCritical()` →
`port.c:606` `portSET_INTERRUPT_MASK_FROM_ISR`), neither the end-of-transaction callback nor the
re-arm task lands during the 2.5–2.9 ms LED window. That is precisely the window `RxDma` has to cover,
and why "0 losses" is a property of the armed state, not of the ring.

The alternative the driver also offers is a free-running mount: no `idle_eof`, one transaction of
`RSBUS_RX_TRANS` that only ends on `length_eof`/`max_packet_receive` (`driver/uhci.h:25-31`). That
removes the per-frame unarmed gap entirely, at the price of frame boundaries — the ring would be
filled in 1,024 B pieces and the ISR would see nothing until the buffer is full. The per-frame
`idle_eof` lifecycle as built is the cheaper trade at 1 MBaud: the unarmed gap is one task wake-up
plus the driver's retry loop against a 128 B FIFO = 1.28 ms of cover.

**What A does not fix:** the 2.5–2.9 ms mask stays. Bus latency stays quantised to the LED window —
that is why the stored DAS round trip is 7.19–7.49 ms (ping / SN / RAM, `study2_result.txt`) and why
the 1 MBaud win over 460800 (7.57 ms in the first study) comes out at ~4 % measured instead of the
8.3 % a wire-only model predicts (§4.2).

## 3. Option C — bigger buffers (rejected)

The buffer that overflows is not a software buffer: it is the **128-byte UART hardware FIFO**,
fixed in silicon (`SOC_UART_FIFO_LEN`). While interrupts are masked, bytes can only go there; a
larger `rx_buffer_size`/ring in PSRAM or SRAM never sees them. That is exactly what the first
study measured (122–161 B over at 1 MBaud) and what the UHCI change fixed by giving the *DMA* a
buffer instead of the driver.

On the node side the budget is closed as well. `pio run -e DAS_v0_1 -t size`:

       text    data     bss     dec     hex  filename
      14084     116    1968   16168    3f28  .pio/build/DAS_v0_1/firmware.elf

    firmware.bin 13,720 B of the 14,336 B uploadable region (95.7 %)
    _etext = 0x3D24, storage (.fixed_data) at 0x3E00  ->  220 B of code headroom
    RAM: .data 112 + .bss 936 = 1,048 B statics + 1,000 B stack = 2,048 B = 100 % of 2 KB
         (objdump -h: .data 0x70 @0x20000000, .bss 0x3a8 @0x20000070 — `size`'s 116/1968 counts
          other LTO/startup sections too, so the section headers are the honest split)

The DAS ring is `BUFFER_SIZE 260` (2.6 ms of wire at 1 MBaud, 5.6 ms at 460800) — 12.7 % of the
part's RAM. There is no RAM to grow it with and 220 B of flash for code that would manage it.
(The card's "95.1 %" is the `.bin`/region ratio; `text+data` including the 512 B `.fixed_data`
reservation is 14,200 B = 99.0 % of the region — same conclusion, harder number.)

What the core buffers, and where: `RxDma` (`RSBUS_RX_TRANS`, 1,024 B, `RSBus.h:40`, `:45`) is the
buffer the **DMA** writes, so it keeps filling while the CPU is masked — 1,024 B = **10.24 ms** of
wire at 1 MBaud (22.22 ms at 460800), 3.5–4.1× the 2.5–2.9 ms mask. `RxRing` (`RSBUS_RX_RING`,
4,096 B, `:37`, `:46`) is **post-hoc storage, not cover**: the CPU that `portENTER_CRITICAL` masks is
the same CPU that copies the burst out of `RxDma` in `OnUhciRx` (`:75`), so the ring contributes
**zero** bytes across the window. Its size is set by the consumer's worst stall (a long service
handler), not by the mask — 4,096 B = 41 ms at 1 MBaud. Adding the 128-byte UART FIFO, which does keep
receiving while no transaction is armed, gives 1,152 B ≈ 11.5 ms as an **upper bound** for the unarmed
case: `uhci_ll_attach_uart_port` (`esp_hal_uart/esp32c3/include/hal/uhci_ll.h:79`) sets a port-select
bit once and there is no per-transaction routing, so the FIFO is a backstop, not a per-frame term.
(The branch's own comment at `RSBus.h:35-36` calls the same 4 KB "4.1 ms"; 4,096 × 10 µs is 41 ms — a
factor-of-10 slip worth correcting when the branch lands, and it does not change the point that the
ring is not what covers the mask.) Buffer size stopped being the constraint the moment the DMA
existed: the ring is sized for the consumer, the DMA buffer for the mask.

## 4. The card's other questions: DAS headroom, the economics, the transceiver

### 4.1 Item 3 — is there DAS-side headroom at 1 MBaud?

48 MHz / 1 MBaud = **48 clocks per bit, 480 clocks per byte** (10 bit times). `USART1_IRQHandler` is
30 instructions at `0x2d20-0x2d7a` in `firmware/.pio/build/DAS_v0_1/firmware.elf`, 28 of them executed
on the normal path, and its ring-full test is `li a4,259` + `bgeu` (`0x2d38`, `0x2d46`) — a compare, no
divide, which is what the compare-wrap commit bought. Those 28 instructions are **6–9 % of the
480-clock byte budget**. The figures were reviewed in round 2 and re-checked for this report with
`riscv32-esp-elf-objdump -d` on the DAS image in `firmware/.pio/build/` (mtime 15:17): same addresses,
same 30/28 count. That image is `master`'s, but the DAS source differs between `master` and the branch
only in the baud constant (§4.2), so the handler is the same code either way.

The ring is `BUFFER_SIZE 260` (`Devices/DAS_v0.1/RSBus.h:4`) = **2.60 ms** of wire at 1 MBaud (5.64 ms
at 460800), 12.7 % of the part's 2 KB. So the node copes on the normal path: the ISR costs a few per
cent of a byte time and the service handler runs inside the inter-frame gap. What does not scale with
the rate is the discovery retry — a jittered repeat over [375, 625] ms while the node has no short
address (`DAS_v0.1/Main.h:181-183`) — unchanged by any baud rate and unrelated to the wire.

Loss criterion stays **CRC/framing** (Akyirr, card comment 4). The node has no drop counter, so "loss"
in this report means a frame the peer rejected, not a counter that incremented.

### 4.2 Item 4 — is 1 MBaud the right next step?

The measured DAS register read is **7.57 ms**, of which **1.17 ms (15.5 %) is wire** at 460800. Even a
zero-length wire saves only 15.5 %; the other ~85 % is protocol — two round trips, the ~3 ms LED
quantisation, the 1 ms DE release. At 1 MBaud the wire term halves to 0.54 ms ⇒ **6.94 ms = 8.3 %
predicted**, against **4.2 % measured**. In the honest form: **1 MBaud is not worth it on its own,
even if the UHCI ring makes it work** — the time a transaction spends is not wire time. And the rate
is a spec change before it is a code change: `Docs/RSBus and Packets.md:1` fixes the bus at 460.8k
baud.

Where the rate lives, file by file:

- `Core/Functions/Bus.h:23` `#define RSBUS_BAUD 1000000` (a `-D`, so one tree builds either rate) and
  `:30` `RS485_BYTE_TIME_US (RSBUS_BAUD >= 1000000 ? 10 : 22)` — both ends derive from it **on the
  branch**.
- `master` hard-codes it: `Devices/Tamu_v2.0A/RSBus.h:14` `.baud_rate = 460800`, and
  `Devices/DAS_v0.1/RSBus.h:87` `USART_InitStr.USART_BaudRate = 460800`.
- The **DAS bootloader is still fixed at 460800** — `Devices/DAS_v0.1/Bootloader.cpp:52`
  `BL_BYTE_TIME_US 22u` and `:170` `USART1->BRR = (48000000 + 460800/2)/460800 = 104` — which is why an
  A/B is a two-end flash, not a one-line change.
- Per option: **A** touches `Devices/Tamu_v2.0A/RSBus.h` only (as built) plus the derived rate
  constants; **"bigger buffers"** touches the `uart_driver_install` size argument (`master`
  `RSBus.h:31` passes 1024) and changes nothing.

### 4.3 Item 5 — the transceiver

The part is **not identifiable anywhere in the tree**: no schematic, no BOM, no marking —
`grep -riE "MAX3485|SP3485|MAX485|SN65HVD|THVD|ADM485"` over `firmware/src`, `app`, `Docs`, `scripts`,
`test` and the `datasheet/` archive returns nothing, and the transceiver exists only as comments
naming its pins: DI = `TXD_PIN` `GPIO_NUM_21`, RO = `RXD_PIN` `GPIO_NUM_20`, DE = `RS485_EN_PIN`
`GPIO_NUM_9` (`Tamu_v2.0A/RSBus.h:6-9`; on the node PD4 with `GPIOD`,
`DAS_v0.1/RSBus.h:5-6`).

What is known without it: 460.8 kbaud runs clean, so the fitted part is rated **≥0.5 Mbps** as a lower
bound. 1 MBaud needs a part rated for it — MAX3485/SP3485/MAX485 or SN65HVD72 are; SN65HVD3082E and
THVD1400 are **not**. The check is visual, not in the tree: the 8-pin SOIC/TSSOP between the RS-485
connector and the DE net, read the top marking. At 100 ns/bit the physical layer stops being free —
stub length and termination have to be looked at, where at 460.8 kbaud they were invisible.

## 5. Recommendation, in order

1. **Keep the UHCI receive ring — and note it is branch-only.** It is what makes 1 MBaud survivable
   (no loss in either stored run), nothing else in this list is a substitute for it, and `master` does
   not have it yet: merging ea57052 from `exp/rsbus-1mbaud-ring` is a decision for Akyirr, and it is
   the prerequisite for any 1 MBaud goal on this tree.
2. **Do not grow any buffer.** Silicon FIFO on the core, 0 B of RAM on the DAS.
3. **Treat 1 MBaud as a step, not a goal.** On its own it is ~4 % measured (§4.2) and it changes a
   rate the spec fixes at 460.8k baud; it is only worth it together with the thing that hides it — the
   ~2.7 ms LED mask — and that is out of scope here (revisit material, above).
4. **Cheap interim measure, if the mask has to wait:** skip `LED.SendParallel()` when the rendered
   frame is byte-identical to the previous one (the render already produces the buffer). It removes the
   mask in the common idle case at ~0 cost, but it does not bound the worst case — an animated display
   still masks for 2.5–2.9 ms.
5. **DAS-side, independent of all of the above:** `Devices/DAS_v0.1/RSBus.h:148` holds DE high for a
   fixed `SleepMicro(1000)` after every transmission — 1 ms of dead time per packet at 1 MBaud
   (~13.5 % of a 7.4 ms transaction). The core already replaced the equivalent with a wire-empty poll
   (`RS485TxWaitWireEmpty()`); the DAS could poll `USART_FLAG_TC`/`TXE` the same way.

## 6. Verification recipe (re-runnable)

The sizes below are the `exp/rsbus-1mbaud-ring` figures, and the shared checkout can be on another
branch, so build them in a detached worktree (the main tree stays untouched):

    git worktree add --detach .wt-1mbaud exp/rsbus-1mbaud-ring
    cp -r firmware/managed_components .wt-1mbaud/firmware/    # untracked, 1.4 MB, needed offline
    cd .wt-1mbaud/firmware
    export PLATFORMIO_CORE_DIR=/home/akyirr/.platformio
    pio run -e Tamu_v2_0A -e DAS_v0_1        # Tamu: RAM 82,572/327,680 (25.2 %),
                                             #       Flash 668,072/2,752,512 (24.3 %)
                                             # DAS:  RAM 2,048/2,048 (100 %),
                                             #       Flash 13,716/14,336 (95.7 %)
    TC=/home/akyirr/.platformio/packages/toolchain-riscv32-esp/bin
    $TC/riscv32-esp-elf-size .pio/build/DAS_v0_1/firmware.elf   # 14084/116/1968
    grep -n "_etext = " .pio/build/DAS_v0_1/firmware.map        # 0x3d24 vs storage 0x3e00
    ls -l .pio/build/DAS_v0_1/firmware.bin                      # 13,720 B
    $TC/riscv32-esp-elf-nm -S .pio/build/Tamu_v2_0A/firmware.elf | grep -E "RxRing|RxDma"
                                                                # 0x1000 + 0x0400 in .bss
    $TC/riscv32-esp-elf-size .pio/build/Tamu_v2_0A/esp_driver_uart/src/uhci.c.o   # 7693
    $TC/riscv32-esp-elf-objdump -d .pio/build/DAS_v0_1/firmware.elf > das.dis     # §4.1
    grep -A 32 "<USART1_IRQHandler>:" das.dis      # 0x2d20-0x2d7a, 30 insns, li a4,259 + bgeu
    cd ../.. && git worktree remove --force .wt-1mbaud

    # rig, read-only (Device Ping + Register Read only; no writes, no flash, no reset).
    # Store the output, or the figure cannot be cited:
    firmware/.pio/probe-venv/bin/python firmware/.pio/probe/study2_probe.py 40 30 \
        > firmware/.pio/probe/study2_result_<date>.txt
    firmware/.pio/probe-venv/bin/python firmware/.pio/probe/rate_probe.py 60 \
        > firmware/.pio/probe/rate_probe.txt    # optional: the file-table case lives only here

    # IDF facts
    grep -n "SOC_UART_FIFO_LEN\|SOC_UHCI_SUPPORTED" \
        /home/akyirr/.platformio/packages/framework-espidf/components/soc/esp32c3/include/soc/soc_caps.h
    grep -n "rx_fsm = UHCI_RX_FSM_ENABLE\|controller not in enable state\|on_rx_trans_event" \
        /home/akyirr/.platformio/packages/framework-espidf/components/esp_driver_uart/src/uhci.c
    grep -n "portENTER_CRITICAL(mux)" \
        /home/akyirr/.platformio/packages/framework-espidf/components/freertos/FreeRTOS-Kernel/portable/riscv/include/freertos/portmacro.h

State the rig was measured in, both times: attached (`/dev/ttyACM0` = ESP32-C3 USB serial/JTAG
303A:1001), DAS reachable as node 2, App Active = USB (the probe's own session is the only app
link; no Flutter app attached), and the flashed images unchanged across both readings — core
`0026-10-23:14`, DAS `0026-10-23:3`. First reading: core up 2.49 Ms (≈41 min), raw output in
`firmware/.pio/probe/study2_result.txt`. Re-run: core up 3.24 Ms, DAS up 3.26 Ms, raw output in
`firmware/.pio/probe/study2_result_verify.txt`. Nothing was flashed or reset for either reading.

## 7. What this study did not establish

* **The flashed baud rate is inferred, and the inference is weaker than the earlier passes claimed.**
  Nothing on the wire or in a register reports the rate. What the two trees actually say: `master`
  hard-codes 460800 on both ends (`.baud_rate = 460800`, `Devices/Tamu_v2.0A/RSBus.h:14`;
  `USART_BaudRate = 460800`, `Devices/DAS_v0.1/RSBus.h:87`), while `exp/rsbus-1mbaud-ring` derives both
  ends from `RSBUS_BAUD`, default 1,000,000 (`Core/Functions/Bus.h:23-25`). Both configurations are
  self-consistent, so "the node answers, therefore both ends agree" is *not* evidence for 1 MBaud — the
  earlier passes' "the tree default is 1 MBaud" only holds on the branch, and the shared checkout is on
  `master`. The version stamps carry no rate (iteration counter only, `scripts/version.py`), and the one
  discriminator I could reach — a code-size difference of 8 B between the master and branch DAS images —
  is invisible because the DAS's System field 5 reports storage, not code (64 / 512 B). Treat all
  latency numbers above as "measured at whatever rate is flashed". The mechanism (§1's arithmetic, §2's
  DMA lifecycle, §3's silicon FIFO) does not depend on it. Settling the rate needs one A/B
  **with both ends flashed from the same tree**: flash `master` (460800) and re-run `study2_probe.py`,
  then flash `exp/rsbus-1mbaud-ring` (1 MBaud) and re-run it — a wire-only prediction says the loss
  figures do not change and the latency drops by the wire delta (~0.3 ms on a 2 × 13 B transaction),
  which is what "the wire hides inside the LED window" predicts and is worth confirming.
* **One earlier pass of this study measured the same code as lossy, and the difference is
  unexplained.** Round 2 recorded 19/40 ping, 16/40 identify, 0/50 on the 30 B reply and 8/40 at 460800
  on the 8 B value; no commit touches the core RX path between that reading and this one (`git log
  --oneline master..exp/rsbus-1mbaud-ring -- firmware/src/Devices/Tamu_v2.0A/RSBus.h` returns `ea57052`
  only), and every run whose output is stored shows 0 losses. The lossy numbers have **no** stored
  output, so what differed cannot be established from the tree: the candidates are the image actually
  flashed (the stamps count iterations, they do not identify a build), the rate, the inter-request
  spacing, and the probe script. Treat "0 losses" as measured on the two stored runs at whatever rate
  those images carry, and the lossy reading as **unreproduced**.
* **No prototype of anything was built or measured** — no DMA-free variant, no mask-free variant. That
  would be a firmware change, out of scope for a read-only study; every option here is argued from the
  build output, the IDF source and the tree's own code, not from a running alternative.
* **No HIL A/B of 1 MBaud vs 460800 on the same build** — that needs two flashes.
