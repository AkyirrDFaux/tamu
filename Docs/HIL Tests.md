The hardware-in-the-loop (HIL) suite exercises real firmware on the devices present on the bus. A single entry point scans the network, classifies every device it finds, and runs each test whose prerequisites the rig satisfies; a test whose prerequisites are unmet is skipped, never failed. The suite covers generic device behaviour, the services, the attached modules and the bootloader, and it fits any rig from one device with an app interface upwards.
### Entry Point
The suite runs from one Dart entry, `test/hil/hil_runner.dart`, started by `test/run_hil_tests.sh`. The script resolves the core port from `/dev/serial/by-id` and passes it in `TAMU_HIL`; `TAMU_HIL=ble` selects the BLE path.
The entry point does four things in order:
- Connects to the network through the app interface.
- Scans with the SNDB and the Register enumeration, building a rig inventory.
- Declares the test groups and their prerequisites.
- Runs the independent tests per device, then the coupled tests one at a time.
### Rig Inventory
Every device the scan finds becomes one inventory entry. An entry answers the questions a prerequisite asks.

| Field | Source | Note |
| --- | --- | --- |
| Role | Device type and capability bits | Core ([[Devices\|Tamu]]) or node ([[Devices\|DAS]], [[Devices\|Valu]]) |
| Address | SNDB | `NetID.Device`, such as `0.1` |
| Type | Register System block, DeviceType | |
| Capabilities | Register System block, Capability bits | Drives the service prerequisites |
| Services | Capability bits | Script and subscription services are capability-gated; storage is universal |
| Modules | The blocks the device exposes | [[Modules and Blocks/LED Display\|LED display]], [[Modules and Blocks/Buttons & LEDs\|fan and buttons]], [[Modules and Blocks/Measurement\|measurement]] |
### Test Categories
A test belongs to one of five categories. The category fixes when the test runs and what it needs. A run leaves the bootloader category out; it is started on its own because it takes its device off the bus.

| Category | Scope | Run |
| --- | --- | --- |
| Generic | One device | Independent |
| Service | One device and one service | Independent |
| Module | One device and one module | Independent |
| Bootloader | One device in [[Services/Bootloader\|bootloader]] mode | Exclusive |
| Partner | Two or more devices | Coupled |
### Prerequisites
A test carries a prerequisite the rig satisfies or not. The runner reads the prerequisite against the rig inventory and skips the test when it does not hold.

| Prerequisite | Holds when |
| --- | --- |
| role | The device is the core, is a node, or is unconstrained |
| capability | The device advertises one capability bit |
| service | The device answers on one service |
| module | The device exposes one module's blocks |
| mode | The device is in bootloader mode |
| partner | Another device satisfies a prerequisite of its own |
| peers | The rig holds a minimum number of devices that satisfy a prerequisite |
### Independence and Ordering
The HIL suite is destructive: a test changes the device's blocks, files, subscriptions and addresses, and the device is not restored to the [[Example Setup|example setup]] afterwards. A test clears the temporaries it creates.
An independent test touches only its own device's state. Independent tests run in any order, and two devices run different tests at the same time.
A coupled test changes state the whole network shares, such as a [[Services/Subscriptions|subscription]] table, a broadcast or an address assignment. Coupled tests run one at a time, after the independent set. A coupled test on a rig of one device is a skip.
A device runs every test its prerequisites allow, so a rig of one core, one [[Devices|DAS]] and one [[Devices|Valu]] runs a different set per device in the same pass.
A bootloader test takes its device off the bus and runs alone; the device stays in bootloader mode and is reset back to its application by hand.
### Skips and Reporting
A skipped test names the prerequisite that failed to hold. The run summary counts pass, fail and skip per test, not per file.
### Logs
Every run writes a timestamped log into `test/hil/logs/`, a folder kept out of version control. The log carries the rig inventory, each test's result and the reason behind every skip.
### Adding a Test
A test is one file under its category, exporting a value that carries a name, a prerequisite and the test body. The category list registers it and the runner schedules it. A test keeps its state on its own device and clears the temporaries it creates.
### Selecting What Runs
Every run scans the bus first and records the rig inventory in the log. The scan is available on its own, and either filter narrows the run.
- `--scan` lists the scanned devices and runs no test.
- `--category <name>` runs one category: generic, service, module, bootloader or partner.
- `--bootloader` runs the bootloader category, which the default run leaves out.
- `--device <id>` runs only against that device; a device whose prerequisite is unmet still skips.
- The filters combine: `--category module --device 2` runs the module category against device `2` alone.
### Running the Suite
The suite runs from `app/` with the Flutter tool on the path.
- `bash test/run_hil_tests.sh` runs every category, with the core port resolved automatically.
- `TAMU_HIL=/dev/ttyACM1 bash test/run_hil_tests.sh` runs against an explicit port.
- `TAMU_HIL=ble bash test/run_hil_tests.sh` runs over BLE.
- `TAMU_HIL_BOOTLOADER=1 bash test/run_hil_tests.sh` declares that a device is in bootloader mode. Such a device is off the bus - the bootloader ignores standard packets - so the scan cannot see it and the declaration stands in for the scan. Its value is irrelevant: bootloader frames carry no address, so the reply comes from whatever is in update mode.
- `bash test/run_hil_tests.sh --scan` lists the rig and stops.
- `bash test/run_hil_tests.sh --category module` runs one category.
- `bash test/run_hil_tests.sh --bootloader` runs the bootloader category on its own.
- `bash test/run_hil_tests.sh --device 2` runs only against device `2`.
