#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fw_slave_update.h"
#include "OD.h"
#include "../../../demo/demoslave/canopennode/CANopen.h"

#define SLAVE_LOG(fmt, ...) printf("[SLAVE-DEMO] " fmt, ##__VA_ARGS__)
#define SLAVE_ERR(fmt, ...) printf("[SLAVE-ERR ] " fmt, ##__VA_ARGS__)

#define SLAVE_NMT_CONTROL                                                                                              \
    (CO_NMT_STARTUP_TO_OPERATIONAL | CO_NMT_ERR_ON_ERR_REG | CO_ERR_REG_GENERIC_ERR | CO_ERR_REG_COMMUNICATION)
#define SLAVE_FIRST_HB_TIME        500
#define SLAVE_SDO_SRV_TIMEOUT_TIME 1000
#define SLAVE_SDO_CLI_TIMEOUT_TIME 500
#define SLAVE_SDO_CLI_BLOCK        false

static CO_t* gSlaveCO = NULL;
static void* gCanInterface = NULL;
static fw_update_context_t gFwCtx;

static bool
slave_demo_canopen_init(uint8_t nodeId, uint16_t bitrate) {
    CO_ReturnError_t err;
    uint32_t heapUsed = 0U;
    CO_config_t* config = NULL;
#ifdef CO_MULTIPLE_OD
    CO_config_t co_config = {0};
    OD_INIT_CONFIG(co_config);
    config = &co_config;
#endif

    gSlaveCO = CO_new(config, &heapUsed);
    if (gSlaveCO == NULL) {
        SLAVE_ERR("CO_new failed\n");
        return false;
    }
    SLAVE_LOG("Reserved %lu bytes for CANopen stack\n", (unsigned long)heapUsed);

    CO_CANsetConfigurationMode(gCanInterface);
    err = CO_CANinit(gSlaveCO, gCanInterface, bitrate);
    if (err != CO_ERROR_NO) {
        SLAVE_ERR("CO_CANinit failed (%d)\n", err);
        return false;
    }

    uint32_t errInfo = 0U;
    err = CO_CANopenInit(gSlaveCO, NULL, NULL, OD, NULL, SLAVE_NMT_CONTROL, SLAVE_FIRST_HB_TIME,
                         SLAVE_SDO_SRV_TIMEOUT_TIME, SLAVE_SDO_CLI_TIMEOUT_TIME, SLAVE_SDO_CLI_BLOCK, nodeId, &errInfo);
    if (err != CO_ERROR_NO && err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) {
        SLAVE_ERR("CO_CANopenInit failed (%d) info=0x%lX\n", err, (unsigned long)errInfo);
        return false;
    }

    err = CO_CANopenInitPDO(gSlaveCO, gSlaveCO->em, OD, nodeId, &errInfo);
    if (err != CO_ERROR_NO && err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) {
        SLAVE_ERR("CO_CANopenInitPDO failed (%d) info=0x%lX\n", err, (unsigned long)errInfo);
        return false;
    }

    CO_CANsetNormalMode(gSlaveCO->CANmodule);
    SLAVE_LOG("CANopen stack started for node %u @ %u kbit/s\n", nodeId, bitrate);
    return true;
}

static void
slave_demo_canopen_service(uint32_t iterations) {
    CO_NMT_reset_cmd_t reset = CO_RESET_NOT;
    for (uint32_t i = 0; i < iterations && reset == CO_RESET_NOT; i++) {
        const uint32_t timeDifference_us = 1000U;
        reset = CO_process(gSlaveCO, false, timeDifference_us, NULL);
#if (CO_CONFIG_PDO) & CO_CONFIG_RPDO_ENABLE
        CO_process_RPDO(gSlaveCO, false, timeDifference_us, NULL);
#endif
#if (CO_CONFIG_PDO) & CO_CONFIG_TPDO_ENABLE
        CO_process_TPDO(gSlaveCO, false, timeDifference_us, NULL);
#endif
    }
}

static void
slave_demo_canopen_shutdown(void) {
    if (gSlaveCO == NULL) {
        return;
    }
    CO_CANsetConfigurationMode(gCanInterface);
    CO_delete(gSlaveCO);
    gSlaveCO = NULL;
}

static void
slave_demo_fill_pattern(uint8_t* buffer, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)(i & 0xFFU);
    }
}

static uint16_t
slave_demo_crc16(const uint8_t* data, uint32_t len) {
    uint16_t crc = 0xFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc = fw_slave_crc16_step(crc, data[i]);
    }
    return crc;
}

static bool
slave_demo_run_fw_session(void) {
    static uint8_t demoImage[FW_CHUNK_SIZE_BYTES * 8U];
    const uint32_t imageSize = sizeof(demoImage);
    slave_demo_fill_pattern(demoImage, imageSize);
    const uint16_t expectedCrc = slave_demo_crc16(demoImage, imageSize);

    fw_slave_reset_context(&gFwCtx);
    if (!fw_slave_store_metadata(&gFwCtx, imageSize, expectedCrc, 1U)) {
        return false;
    }
    if (!fw_slave_prepare_storage(&gFwCtx)) {
        return false;
    }

    uint32_t offset = 0;
    while (offset < imageSize) {
        uint32_t remaining = imageSize - offset;
        uint32_t len = remaining < FW_CHUNK_SIZE_BYTES ? remaining : FW_CHUNK_SIZE_BYTES;
        if (!fw_slave_receive_chunk(&gFwCtx, demoImage + offset, len, offset)) {
            return false;
        }
        offset += len;
    }

    if (!fw_slave_finalize(&gFwCtx)) {
        return false;
    }

    fw_slave_dump_context(&gFwCtx);
    SLAVE_LOG("Firmware image accepted; call esp_ota_set_boot_partition() + esp_restart() when running on ESP-IDF.\n");
    return true;
}

int
main(void) {
    if (!slave_demo_canopen_init(0x0AU, 250U)) {
        SLAVE_ERR("Unable to start CANopen\n");
        return -1;
    }

    if (!slave_demo_run_fw_session()) {
        SLAVE_ERR("Firmware session script failed\n");
        slave_demo_canopen_shutdown();
        return -1;
    }

    SLAVE_LOG("Keeping node alive briefly so you can attach a debugger and inspect OD entries.\n");
    slave_demo_canopen_service(5000U);
    slave_demo_canopen_shutdown();
    SLAVE_LOG("Demo complete. Wire the SDO callbacks for 0x1F50/0x1F51/0x1F57/0x1F5A to these helpers in your production app.\n");
    return 0;
}
