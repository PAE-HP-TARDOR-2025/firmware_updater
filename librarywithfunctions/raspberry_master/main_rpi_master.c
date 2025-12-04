/*
 * Raspberry Pi CANopen master demo main.
 *
 * This skeleton initializes SocketCAN, sets up a minimal CANopenNode instance,
 * and exercises the fw_master_update helpers to push a firmware image to
 * an ESP32 slave over the CAN bus.
 *
 * Build on the Pi:
 *   gcc -o rpi_uploader main_rpi_master.c fw_master_update.c rpi_can.c \
 *       OD.c <path-to-CANopenNode>/*.c -I<path-to-CANopenNode> -lpthread
 *
 * Before running, bring up the CAN interface:
 *   sudo ip link set can0 type can bitrate 250000
 *   sudo ip link set can0 up
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "rpi_can.h"
#include "fw_master_update.h"

/* Adjust these paths to point at your CANopenNode checkout */
// #include "CANopen.h"
// #include "OD.h"

#define MASTER_LOG(fmt, ...) printf("[RPI-MASTER] " fmt, ##__VA_ARGS__)
#define MASTER_ERR(fmt, ...) fprintf(stderr, "[RPI-ERR    ] " fmt, ##__VA_ARGS__)

static int g_can_sock = -1;
static volatile bool g_running = true;

/* Stub timing helper */
static void
delay_ms(uint32_t ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/*
 * When you integrate CANopenNode for Linux, replace these stubs with real
 * CO_SDOclientDownloadInitiate / CO_SDOclientDownload calls wired to g_can_sock.
 */

#if 0 /* Real CANopenNode wiring example (pseudo-code) */
static CO_t* g_CO = NULL;

static bool
sdo_download(uint8_t nodeId, uint16_t index, uint8_t subIndex, const uint8_t* data, size_t len) {
    CO_SDO_abortCode_t abortCode;
    size_t sizeTransferred;
    CO_SDO_return_t ret = CO_SDOclientDownloadInitiate(g_CO->SDOclient, index, subIndex, len, 1000, false);
    if (ret < 0) return false;
    CO_SDOclientDownloadBufWrite(g_CO->SDOclient, data, len);
    while ((ret = CO_SDOclientDownload(g_CO->SDOclient, 1000, false, false, &abortCode, &sizeTransferred, NULL)) > 0) {
        delay_ms(1);
    }
    return ret == CO_SDO_RT_ok_communicationEnd;
}
#endif

static bool
stub_sdo_download(uint8_t nodeId, uint16_t index, uint8_t subIndex, const uint8_t* data, size_t len) {
    MASTER_LOG("[STUB] SDO download to node %u idx 0x%04X sub %u len %zu\n", nodeId, index, subIndex, len);
    /* TODO: replace with real CANopenNode SDO client calls */
    delay_ms(1);
    return true;
}

/*
 * Custom implementations of the fw_master helpers that call into real SDO logic.
 * For now they wrap stub_sdo_download; replace with your CANopenNode integration.
 */

bool
fw_master_send_metadata(const fw_upload_plan_t* plan, const fw_payload_t* payload, uint16_t crc) {
    uint8_t meta[8];
    uint32_t size = (uint32_t)payload->size;
    meta[0] = (uint8_t)(size & 0xFF);
    meta[1] = (uint8_t)((size >> 8) & 0xFF);
    meta[2] = (uint8_t)((size >> 16) & 0xFF);
    meta[3] = (uint8_t)((size >> 24) & 0xFF);
    meta[4] = (uint8_t)(crc & 0xFF);
    meta[5] = (uint8_t)((crc >> 8) & 0xFF);
    meta[6] = (uint8_t)plan->type;
    meta[7] = plan->targetBank;
    return stub_sdo_download(plan->targetNodeId, 0x1F57, 1, meta, sizeof(meta));
}

bool
fw_master_send_start_command(const fw_upload_plan_t* plan) {
    uint8_t cmd[3] = { 0x01, 0x00, 0x00 }; /* start token */
    return stub_sdo_download(plan->targetNodeId, 0x1F51, 1, cmd, sizeof(cmd));
}

bool
fw_master_send_chunk(const fw_upload_plan_t* plan, const uint8_t* chunk, size_t len, size_t offset) {
    (void)offset;
    return stub_sdo_download(plan->targetNodeId, 0x1F50, 1, chunk, len);
}

bool
fw_master_send_finalize_request(const fw_upload_plan_t* plan, uint16_t crc) {
    uint8_t status[2] = { (uint8_t)(crc & 0xFF), (uint8_t)((crc >> 8) & 0xFF) };
    return stub_sdo_download(plan->targetNodeId, 0x1F5A, 1, status, sizeof(status));
}

int
main(int argc, char** argv) {
    const char* canIf = "can0";
    const char* fwPath = argc > 1 ? argv[1] : "bye.bin";
    uint8_t slaveNodeId = argc > 2 ? (uint8_t)atoi(argv[2]) : 0x0A;

    MASTER_LOG("Initializing SocketCAN on %s\n", canIf);
    g_can_sock = rpi_can_init(canIf, 250);
    if (g_can_sock < 0) {
        MASTER_ERR("Failed to open CAN interface\n");
        return 1;
    }

    fw_upload_plan_t plan = {
        .firmwarePath = fwPath,
        .type = FW_IMAGE_MAIN,
        .targetBank = 1,
        .targetNodeId = slaveNodeId,
        .maxChunkBytes = 256,
        .expectedCrc = 0
    };

    MASTER_LOG("Starting firmware upload: %s -> node %u\n", plan.firmwarePath, plan.targetNodeId);
    bool ok = fw_master_run_upload_session(&plan);

    rpi_can_close(g_can_sock);

    if (ok) {
        MASTER_LOG("Upload complete. Slave should reboot into new firmware.\n");
        return 0;
    } else {
        MASTER_ERR("Upload failed.\n");
        return 1;
    }
}
