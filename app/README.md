# Tamu App

Flutter companion app for the Tamu device network.

Firmware: [`../firmware/`](../firmware/README.md)

The app speaks the App Interface protocol to the core device it is connected to,
over USB or BLE, discovers the rest of the network through the core's SN
database, and drives every service the firmware exposes: registers and dynamic
memory, storage files, scripts, subscriptions, logs, backups and firmware
updates. It keeps the network database in RAM and updates it as data arrives.
The emphasis is human-readable interpretation — plain labels for people who are
not firmware engineers, the raw values one level down for those who are.

## Target platforms

| OS      | Connection types |
| ------- | ---------------- |
| Android | BLE              |
| Linux   | BLE, USB         |
| Windows | BLE, USB         |

Per `Docs/App/General Info.md`. Linux and Android are the builds in use; the
Windows runner is an unmodified Flutter scaffold.

## Running

From the repo root:

    ./app.sh

It regenerates the version stamp (`scripts/gen_app_version.py`), then builds and
launches the debug Linux bundle (`flutter build linux --debug`). Android is
host-side only — `adb` is not installed on the development machine.

## Tests

    cd app
    flutter test --exclude-tags hil     # the host suites
    flutter analyze                     # must stay clean

Both are what `./test.sh` runs at the repo root, after the firmware's native
tests; it invokes them through its resolved `$FLUTTER` binary. The `hil` tag
(`app/dart_test.yaml`) marks the suites that need the physical rig; they run
through the app's own clients:

    bash test/run_hil_tests.sh

`test/README.md` at the repo root covers the categories, the port discovery and
the logs.

## Layout

    lib/main.dart     App entry and shell (drawer on phones, rail on desktop)
    lib/core/         Protocol, transports, connection, device DB, service clients,
                      backup, scripts, settings
    lib/ui/           One file per documented page, plus shared pickers and widgets
    test/             Host suites; test/hil/ the rig suites
    tool/             Developer helpers (fixtures, contrast, rig link, bench,
                      setup upload)

`lib/core/` holds the protocol, the state and the service clients; `lib/ui/`
holds the widgets.

## Android notes

- BLE only, with runtime permissions requested on the first scan; settings live
  in the app support directory.
- Gradle 8.14 and the Kotlin tooling reject JDKs newer than 24 (Java 25/26 fail
  inside Kotlin's `JavaVersion.parse`), so build with a JDK 17–24.
- Beta builds carry the application id `tamu.app.beta`
  (`applicationIdSuffix = ".beta"` in `android/app/build.gradle.kts`) so they
  install alongside a release build; debug and release share `tamu.app`, so
  installing one replaces the other.
- Runtime BLE permissions come from `permission_handler` `^13.0.2`
  (`pubspec.yaml`), resolved to 13.0.2 in `pubspec.lock`.
