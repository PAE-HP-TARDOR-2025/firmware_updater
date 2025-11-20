| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-H21 | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 | Linux |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | --------- | -------- | -------- | -------- | -------- | ----- |

# Demo master uploader (ESP32)

This application reuses the host-side `master_firmware_uploader` logic and runs it directly on an Espressif ESP32 development board. All transfers now ride on the bundled CANopenNode stack, so the same metadata, start, chunk, and finalize calls issue real `CO_SDOclient` downloads over TWAI while still printing the verbose log.

## Project layout

```
├── CMakeLists.txt
├── main
│   ├── CMakeLists.txt                Component definition
│   ├── Kconfig.projbuild             Firmware path, node identifier, and chunk-size settings
│   ├── demo_master_app.c             ESP-IDF entry point that loads settings and mounts storage
│   ├── master_uploader_demo.h        Local copy of the host uploader headers (enum + structs)
│   └── master_uploader_demo.c        Local copy of the uploader implementation with stubs
├── canopennode                       Vendored CANopenNode stack (built as an ESP-IDF component)
└── README.md
```

The same logic still drives the desktop executable at the repository root. This directory now carries an embedded-friendly copy along with a local CANopenNode component so the ESP-IDF build stays self-contained while you continue to iterate on the transport and storage hooks.

## Preparing the firmware image

1. Build the slave binaries with `python build_slave_bins.py` from the `demo` folder.
2. Copy the resulting `.bin` file (for example `demo/artifacts/bye.bin`) into the storage medium that your ESP32 master can read. The default configuration expects the file at `/spiffs/bye.bin`, so you can use `idf.py spiffs` or any other flashing method to populate the SPI flash partition named `storage`.
3. If you prefer Secure Digital card storage or another mount point, open `idf.py menuconfig` and adjust **Demo master uploader settings** → **Firmware image path** along with the mounting options.

## Configuring the CAN interface

Open `idf.py menuconfig` and look under **Demo master uploader settings** to point the built-in CANopen master at your wiring:

- **Master node identifier** — Node-ID that this ESP32 claims on the bus. Default is 100.
- **TWAI bit rate (kbps)** — Supported values are 125, 250, 500 (default), and 1000 kbps.
- **TWAI TX/RX GPIO** — Pins that connect to the external transceiver. Defaults match GPIO5 (TX) and GPIO4 (RX).

The CANopen task starts automatically during boot, binds the uploader to the first `CO_SDOclient`, and begins servicing the SDO download loop on the background FreeRTOS tasks.

## Refreshing the SPIFFS image

The root CMake project now packages everything under `demo/demomaster/storage` into the `storage` SPIFFS partition. To update the payload:

1. Copy your firmware file (for example `demo/artifacts/bye.bin`) into `demo/demomaster/storage/bye.bin`.
2. Build the filesystem image: `idf.py storage`.
3. Flash the updated partition: `idf.py storage-flash -p <PORT>`.

On the next reset the master will load the binary from `/spiffs/bye.bin` (or whichever path you configured in menuconfig).

## Building and running

```pwsh
cd demo/demomaster
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

You should see the `[FW-MASTER]` logs that were previously limited to the desktop build. Once the chunk loop ends, the demo prints a reminder to reset the slave so it can boot the newly written image.

## Next steps

- Point the indexed objects (0x1F50/0x1F51/0x1F57/0x1F5A) at the real slave-side Object Dictionary entries. The current layout transmits size, CRC, type, and bank inside a packed structure, so adapt the slave handler if your format differs.
- Hook the ESP32 up to the target transceiver (for example, SN65HVD230) and confirm that the SDO sequence reaches the slave by watching the log lines and the remote responses.
- Extend the demo to poll the slave's status word (0x1F5A) or heartbeat before declaring success, and add retries/back-off logic if desired.
