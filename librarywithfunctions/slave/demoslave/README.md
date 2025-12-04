# demoslave – ESP-IDF project scaffold

This folder is a minimal ESP-IDF project that compiles `slave_main.c` (and the
associated firmware-update helpers) into a runnable binary. The slave prints
its greeting and running firmware CRC periodically, enabling visual verification
of OTA updates.

## Features

- Full CANopen slave with firmware update server
- Greetings: "Hello from slave" (initial) / "Bye from slave" (after OTA)
- Running CRC via `fw_server_get_running_crc()` exposed on CANopen object `0x1F5B:01`
- Configurable greeting via environment variable or Kconfig

## Quick start

1. Source the ESP-IDF environment:
   ```bash
   . $IDF_PATH/export.sh
   ```
2. Build:
   ```bash
   idf.py -C demoslave build
   ```
3. Flash & monitor:
   ```bash
   idf.py -C demoslave -p /dev/ttyUSB0 flash monitor
   ```

## Overriding the greeting at build time

Set `SLAVE_GREETING_OVERRIDE` in the environment before building:

```powershell
$env:SLAVE_GREETING_OVERRIDE = "Goodbye from slave"
idf.py -C demoslave build
```

Or use the helper script in the parent folder:

```bash
python build_slave_bins.py --greeting "bye:Goodbye from slave"
```

The resulting `.bin` files are copied to `artifacts/` (or the `--output-dir` you
specify).

## Files sourced from parent directory

The component pulls in sources from `..`:

| File                  | Purpose                                          |
|-----------------------|--------------------------------------------------|
| `slave_main.c`        | Template main with CANopen + greeting task       |
| `fw_update_server.c`  | ESP-IDF-specific SDO extensions for OTA          |
| `fw_update_server.h`  | Header with `fw_server_get_running_crc()` API    |
| `fw_slave_update.c`   | Portable OTA state machine                       |
| `OD.c` / `OD.h`       | Object Dictionary (slave copy)                   |

If you customise these files, the changes apply to every build produced from
this project.

## Build Script

The parent folder contains `build_slave_bins.py` which builds both greeting variants:

```pwsh
cd librarywithfunctions/slave
python build_slave_bins.py
```

This produces:
- `artifacts/hello.bin` – prints "Hello from slave" with running CRC
- `artifacts/bye.bin` – prints "Bye from slave" with running CRC

## Dependencies

- ESP-IDF v5.0.1 or later (tested with v5.0.1)
- CANopenNode component (referenced from `demo/demoslave/canopennode`)
