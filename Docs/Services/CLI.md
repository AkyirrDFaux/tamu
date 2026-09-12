ESP32 Only
Use define USE_CLI.
Must not interfere with APP Interface!

Allows for interaction with specified user-oriented commands.

Works over the ESP32-C3's USB Serial/JTAG console (App has priority on USB though).

Should implement these commands:
- Ping
- Identify
- All SNDB commands
- All registry commands
- Read and clear logs
- All storage commands
- Get and set subscription commands