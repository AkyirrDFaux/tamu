# Hardware test tools

## testsuite.py - Tamu v2.0A + DAS v0.1 CLI battery

Auto-detects the Tamu console port by USB vendor ID (303a). The Tamu app must
be closed while it runs (it holds the serial port).

    python3 testsuite.py            # everything (~5 min)
    python3 testsuite.py --core     # Tamu-local groups only (~3 min)
    python3 testsuite.py --node     # DAS via RS-Bus only (~1 min)
    python3 testsuite.py --storage  # storage + memory persistence groups
    python3 testsuite.py --reboot   # include reboot-persistence (reflashes!)
    python3 testsuite.py --only device,tamu   # fine-grained selection

Group keys: device, tamu, das, dynamic, storage, memory, device_edge, parse,
topology, reboot.

Timing model: replies complete after the echo plus an idle gap (1 s local,
2.5 s flash-mutating verbs, 4 s RS-Bus round trips). Prompts cannot be used as
completion markers - the console prints them before async result lines.

## hwtest.py - ad-hoc console poke tool

    python3 hwtest.py boot                 # first 5 s of console output
    python3 hwtest.py read 10              # raw capture for N seconds
    python3 hwtest.py cmd 2.5 "dev 1 ping" # send + capture window per command

Port auto-detected the same way.
