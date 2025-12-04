# Raspberry Pi Master Uploader

This folder contains the CANopen firmware uploader ported to run on a Raspberry Pi using SocketCAN instead of the ESP32 TWAI driver.

## Contents

| File | Description |
|------|-------------|
| `OD.[ch]` | Object Dictionary copy from the ESP32 master (unchanged). |
| `fw_master_update.[ch]` | Portable helper library for CRC, payload loading, and upload sequencing. |
| `rpi_can.[ch]` | Lightweight SocketCAN wrapper (init/send/recv/close). |
| `main_rpi_master.c` | Demo main that opens SocketCAN, builds a plan, and drives the upload. |

## Hardware Setup

1. Attach a CAN transceiver (e.g., MCP2515 SPI module or Waveshare CAN HAT) to the Raspberry Pi.
2. Enable the overlay in `/boot/config.txt`:
   ```
   dtoverlay=mcp2515-can0,oscillator=8000000,interrupt=25
   ```
   Adjust oscillator and interrupt GPIO to match your board.
3. Bring the interface up before running:
   ```bash
   sudo ip link set can0 type can bitrate 250000
   sudo ip link set can0 up
   ```

## Building

```bash
# Assuming CANopenNode is checked out alongside this folder
gcc -o rpi_uploader \
    main_rpi_master.c fw_master_update.c rpi_can.c OD.c \
    -I. -lpthread
```

To integrate real CANopenNode SDO client calls instead of the stubs:

1. Clone `https://github.com/CANopenNode/CANopenNode` and the Linux driver.
2. Add the CANopenNode sources to the build and include `CANopen.h`.
3. Replace `stub_sdo_download` in `main_rpi_master.c` with `CO_SDOclientDownloadInitiate` / `CO_SDOclientDownload`.

## Running

```bash
./rpi_uploader bye.bin 10
```

- First argument: path to the firmware binary.
- Second argument: CANopen node ID of the ESP32 slave (default 10 / 0x0A).

The slave must already be running `hello_world_main.c` with `fw_server_init()` registered so the OTA objects are active.

## Notes

- The CAN bitrate is configured externally via `ip link`; the `bitrate_kbps` argument to `rpi_can_init` is informational only.
- For production, replace the stub SDO helpers with real CANopenNode client calls or use the Python `canopen` library with its SDO API.
