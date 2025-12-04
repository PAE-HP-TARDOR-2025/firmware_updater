# Firmware Update Library

This folder holds a self-contained snapshot of the core building blocks that power the CANopen firmware update flow without the rest of the demo scaffolding. Use these files as the starting point for integrating the OTA logic into other applications or for diffing against future changes in the demo projects.

The slave Object Dictionary copy already includes the firmware-transfer cluster (`0x1F50` download FIFO, `0x1F51` control, `0x1F57` metadata, `0x1F5A` status, `0x1F5B` running CRC), so you can verify the entries before wiring them into your own project.

## Layout

```
librarywithfunctions/
├── master/
│   ├── OD.[ch]                  ← controller Object Dictionary
│   └── fw_master_update.[ch]    ← CRC-16, binary loading, CiA 302 helpers
├── slave/
│   ├── OD.[ch]                  ← OTA-capable slave Object Dictionary
│   ├── fw_slave_update.[ch]     ← platform-neutral state machine
│   ├── fw_update_server.[ch]    ← ESP-IDF OTA handler with running CRC getter
│   ├── slave_main.c             ← full CANopen slave template
│   ├── build_slave_bins.py      ← builds hello.bin / bye.bin variants
│   ├── artifacts/               ← generated binaries (hello.bin, bye.bin)
│   └── demoslave/               ← standalone ESP-IDF project
│       ├── CMakeLists.txt
│       ├── sdkconfig
│       └── main/
│           ├── CMakeLists.txt
│           └── Kconfig.projbuild
├── partitions/
│   ├── master_storage_spiffs.csv
│   └── slave_two_ota.csv
└── README.md (this file)
```

## Key Components

### Master

- **`master/OD.[ch]`** – Direct copy of the controller Object Dictionary. Keep in sync with your CANopen stack so SDO indices stay aligned.
- **`master/fw_master_update.[ch]`** – Reusable helpers for loading binaries, computing CRC-16, and exercising the CiA 302 firmware objects (`0x1F50–0x1F5B`). Transport calls are stubbed; swap with your `CO_SDOclient*` interactions.

### Slave

- **`slave/OD.[ch]`** – OTA-capable slave Object Dictionary with firmware objects in place.
- **`slave/fw_slave_update.[ch]`** – Platform-neutral state machine that validates metadata, checks chunk ordering, tracks CRC, and reports readiness to boot. Use for host-side tests or alternative RTOS ports.
- **`slave/fw_update_server.[ch]`** – ESP-IDF SDO hook implementation that binds firmware objects to OTA partitions (`esp_ota_*`, `esp_partition_*`). Exposes `fw_server_get_running_crc()` to retrieve the CRC of the currently running firmware via CANopen object `0x1F5B`.
- **`slave/slave_main.c`** – Full CANopen slave application template with firmware update server integration. Periodically prints the greeting and running firmware CRC for debugging. Use as a starting point for new slave firmware.
- **`slave/build_slave_bins.py`** – Builds greeting-specific binaries by compiling the demoslave project with different `SLAVE_GREETING_OVERRIDE` values.
- **`slave/demoslave/`** – Standalone ESP-IDF project that can be built independently.

## Building Slave Binaries

The `build_slave_bins.py` script generates multiple firmware variants:

```pwsh
cd librarywithfunctions/slave
python build_slave_bins.py
```

This produces:
- `artifacts/hello.bin` – prints "Hello from slave" (initial firmware)
- `artifacts/bye.bin` – prints "Bye from slave" (firmware after OTA update)

The greetings are configured via the `SLAVE_GREETING_OVERRIDE` environment variable during build.

### Custom Greetings

Edit `build_slave_bins.py` to change the default greeting pairs:

```python
DEFAULT_GREETING_PAIRS = (
    ("hello", "Hello from slave"),
    ("bye", "Bye from slave"),
)
```

Or pass them on the command line:

```pwsh
python build_slave_bins.py --greeting custom:"My custom greeting"
```

## Running CRC API

The slave exposes its running firmware CRC through CANopen object `0x1F5B:01`. This allows the master to query the slave's current firmware and skip OTA if it already matches.

```c
// In slave firmware
uint16_t crc = fw_server_get_running_crc();
printf("Running firmware CRC: 0x%04X\n", crc);
```

The CRC is computed at boot from the active OTA partition and remains constant until the next firmware update.

## How to Use the Master Helpers

1. Drop `master/fw_master_update.[ch]` into your host project and include `fw_master_update.h`.
2. Provide a `fw_upload_plan_t` describing the target:

   ```c
   fw_upload_plan_t plan = {
       .firmwarePath = "artifacts/bye.bin",
       .type = FW_IMAGE_MAIN,
       .targetBank = 1,
       .targetNodeId = 10,
       .maxChunkBytes = 256,
       .expectedCrc = 0
   };
   fw_master_run_upload_session(&plan);
   ```

3. Replace stubbed helpers with real SDO client calls:
   - Metadata → download into `0x1F57` subindices
   - Control tokens → write to `0x1F51`
   - Firmware data → block download into `0x1F50`
   - Status polling → read back `0x1F5A`
   - Running CRC query → upload from `0x1F5B:01`

## How to Use the Slave Helpers

1. Add `slave/fw_slave_update.[ch]` to your ESP-IDF component.
2. Initialize a `fw_update_context_t` and call:
   - `fw_slave_store_metadata` when master writes `0x1F57`
   - `fw_slave_prepare_storage` when `0x1F51` issues start token
   - `fw_slave_receive_chunk` for SDO block segments on `0x1F50`
   - `fw_slave_finalize` when master announces completion
3. Once finalize succeeds, switch boot partitions and reboot.

### Using the ESP-IDF Template

For a complete ESP-IDF application, start with `slave/slave_main.c`:

```c
// slave_main.c provides:
// - CANopen stack initialization
// - Firmware update server registration
// - Greeting task with CRC reporting
// - FreeRTOS task structure
```

Copy the demoslave project to your own location and customize as needed.

## ESP-IDF Configuration

Key `sdkconfig` settings for the slave:

- Enable "Custom partition table" → `partitions/slave_two_ota.csv`
- Flash size: 4 MB or larger (OTA slots need ~1 MB each)
- TWAI (CAN) driver enabled with correct GPIO pins
- CANopenNode component included

## Partition Tables

- **`partitions/master_storage_spiffs.csv`** – Master partition map with `/storage` SPIFFS for firmware artifacts.
- **`partitions/slave_two_ota.csv`** – Dual OTA layout mirroring ESP-IDF template.

Set `CONFIG_PARTITION_TABLE_CUSTOM=y` and point to the appropriate CSV.

## Keeping the Library in Sync

- When demo Object Dictionaries change, copy updated `OD.[ch]` into this folder.
- Diff helper files against demo originals to understand upstream edits.
- Use stubbed logging for headless CI tests before integrating with real CAN hardware.

With these pieces you can spin up the full OTA pipeline in any CANopen-compatible product without dragging the entire demo tree into your repository.
