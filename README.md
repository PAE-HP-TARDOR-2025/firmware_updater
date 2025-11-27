# Firmware Updater Reference

This repository bundles a complete CANopen firmware update path that now runs entirely on two ESP-IDF applications:

- `demo/demoslave` – CANopen slave that validates metadata, streams chunks into the inactive OTA partition, sets the next boot partition, and automatically reboots once the image is verified.
- `demo/demomaster` – CANopen master that mounts a SPIFFS partition, serves firmware binaries from `/spiffs/*.bin`, and drives the CiA‑302 download sequence over TWAI using CANopenNode.

You can reuse the same state machines on other hardware (desktop, STM32, etc.), but the demos make it easy to observe the whole loop on a pair of ESP32 boards.

## Repository layout

```
firmware_updater/
├── main_firmware_update.c     ← generic CANopen slave reference implementation
├── master_firmware_uploader.c ← desktop/host master reference
├── demo/
│   ├── build_slave_bins.py    ← helper that builds multiple slave greetings
│   ├── artifacts/             ← `.bin` output staged for uploads
│   ├── demoslave/             ← ESP-IDF slave project (OTA, auto reboot)
│   └── demomaster/            ← ESP-IDF master project (SPIFFS + CANopen SDO)
└── README.md (this file)
```

Each ESP-IDF project also has its own README under `demo/demoslave` and `demo/demomaster` with board-specific wiring and configuration details.

## Quick start (two ESP32 boards)

1. **Generate slave firmware variants**
   ```pwsh
   cd demo
   python build_slave_bins.py --greeting hello:"Hello from slave" --greeting bye:"Firmware update ready"
   ```
   - Creates deterministic build folders (`demoslave/build-hello`, `build-bye`, …).
   - Copies the finished binaries into `demo/artifacts/hello.bin` and `demo/artifacts/bye.bin`.

2. **Flash the baseline image to the slave**
   ```pwsh
   idf.py -C demo/demoslave -B build-hello -p <SLAVE_PORT> flash monitor
   ```
   Keep the monitor open to watch `[SLAVE]` and `[fw_server]` logs.

3. **Stage the update for the master**
   ```pwsh
   cd demo/demomaster
   copy ..\artifacts\bye.bin storage\bye.bin
   idf.py storage
   idf.py storage-flash -p <MASTER_PORT>
   ```
   The SPIFFS partition named `storage` now contains `/spiffs/bye.bin`.

4. **Configure and flash the master**
   ```pwsh
   idf.py menuconfig   # adjust TWAI pins, bit rate, firmware path, target node ID
   idf.py build
   idf.py -p <MASTER_PORT> flash monitor
   ```
   The master automatically starts the uploader task and runs forever.

5. **Observe the OTA transfer**
   - The master prints `[FW-MASTER]` metadata, start, chunk, and finalize logs.
   - The slave prints `[fw_server]` chunk confirmations, CRC verification, and finally `Firmware image validated …`.
   - One second later the slave reboots itself and loads the `ota_0` partition that now says “Firmware update ready”.

## How the pieces work together

| Stage | Master (`demo/demomaster`) | Slave (`demo/demoslave`) |
| ----- | ------------------------- | ------------------------ |
| Metadata (0x1F57) | Loads file from `/spiffs/*.bin`, computes CRC16/CCITT, pushes size/CRC/bank/type | Validates limits, prepares internal state |
| Start (0x1F51) | Issues CiA‑302 start command | Calls `esp_ota_begin()` on the inactive OTA partition |
| Data (0x1F50) | Streams file in <= chunk size blocks (default 256 B) | Pipes data straight into `esp_ota_write()` while computing CRC |
| Status (0x1F5A) | Sends final CRC | Verifies CRC, calls `esp_ota_end()`, selects new partition, schedules auto reboot |

Key ESP-IDF features in use:

- Two OTA partitions on a 4 MB flash map (`partitions_two_ota.csv`).
- `esp_ota_get_next_update_partition()`/`esp_ota_write()`/`esp_ota_set_boot_partition()`.
- Auto reboot through an ESP timer that fires ~500 ms after validation so logs reach the console before reset.
- TWAI (CAN) driver + CANopenNode stack to speak SDO.
- SPIFFS image baked from `demo/demomaster/storage/` to distribute firmware files.

## Reusing the components

- **Slave reference (`main_firmware_update.c`)** – drop this file into any CANopenNode project to get the same metadata state machine and CRC validation. Replace the ESP-specific storage hooks with your platform’s flash drivers.
- **Master reference (`master_firmware_uploader.c`)** – compile it on a desktop to test new binaries without hardware. The ESP-IDF master app embeds the same logic but replaces the transport stubs with real `CO_SDOclient` calls.
- **Build helper (`build_slave_bins.py`)** – reproducibly generates multiple slave binaries by greeting name, target, optimization level, etc. Use it to keep artifacts in `demo/artifacts/` up to date for regression tests.

## Troubleshooting cheatsheet

- `Chunk rejected: expected offset …` – master and slave lost sync. Verify SDO clients aren’t retransmitting stale segments.
- `Image size exceeds OTA partition` – adjust flash size in `demo/demoslave/sdkconfig` or reduce application footprint.
- `esp_ota_set_boot_partition` errors – ensure both `ota_0` and `ota_1` partitions exist and that the binary fits inside them.
- Master stuck waiting for file – confirm `/spiffs/<name>.bin` exists and that you reflashed the `storage` partition after copying the new file.
- No reboot after finalize – the slave now schedules its own restart; if you disable auto reboot via Kconfig, manually reset the board after the `[fw_server] Firmware image validated` log.

## Next steps

1. Review `demo/demoslave/README.md` for configurable node IDs, TWAI pins, and OTA tuning knobs.
2. Review `demo/demomaster/README.md` for storage layout, wiring diagrams, and the SPIFFS workflow.
3. Port the reference code into your production projects, replacing the dummy greeting logic with real application payloads.

With these pieces running, you now have a proven CANopen OTA flow that starts from a freshly built `.bin`, stages it on the master, and ends with the slave rebooting into the new firmware automatically.
