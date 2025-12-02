#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "fw_master_update.h"
#include "OD.h"
#include "../../../demo/demomaster/canopennode/CANopen.h"

#define MASTER_LOG(fmt, ...) printf("[MASTER-DEMO] " fmt, ##__VA_ARGS__)
#define MASTER_ERR(fmt, ...) printf("[MASTER-ERR ] " fmt, ##__VA_ARGS__)

#define MASTER_NMT_CONTROL                                                                                             \
    (CO_NMT_STARTUP_TO_OPERATIONAL | CO_NMT_ERR_ON_ERR_REG | CO_ERR_REG_GENERIC_ERR | CO_ERR_REG_COMMUNICATION)
#define MASTER_FIRST_HB_TIME        500
#define MASTER_SDO_SRV_TIMEOUT_TIME 1000
#define MASTER_SDO_CLI_TIMEOUT_TIME 500
#define MASTER_SDO_CLI_BLOCK        false

static CO_t* gCO = NULL;
static void* gCanInterface = NULL;

static bool
master_demo_canopen_init(uint8_t nodeId, uint16_t bitrate) {
    CO_ReturnError_t err;
    uint32_t heapUsed = 0U;
    CO_config_t* config = NULL;
#ifdef CO_MULTIPLE_OD
    CO_config_t co_config = {0};
    OD_INIT_CONFIG(co_config);
    config = &co_config;
#endif

    gCO = CO_new(config, &heapUsed);
    if (gCO == NULL) {
        MASTER_ERR("CO_new failed\n");
        return false;
    }
    MASTER_LOG("Reserved %lu bytes for CANopen stack\n", (unsigned long)heapUsed);

    CO_CANsetConfigurationMode(gCanInterface);
    err = CO_CANinit(gCO, gCanInterface, bitrate);
    if (err != CO_ERROR_NO) {
        MASTER_ERR("CO_CANinit failed (%d)\n", err);
        return false;
    }

    uint32_t errInfo = 0U;
    err = CO_CANopenInit(gCO, NULL, NULL, OD, NULL, MASTER_NMT_CONTROL, MASTER_FIRST_HB_TIME,
                         MASTER_SDO_SRV_TIMEOUT_TIME, MASTER_SDO_CLI_TIMEOUT_TIME, MASTER_SDO_CLI_BLOCK, nodeId, &errInfo);
    if (err != CO_ERROR_NO && err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) {
        MASTER_ERR("CO_CANopenInit failed (%d) info=0x%lX\n", err, (unsigned long)errInfo);
        return false;
    }

    err = CO_CANopenInitPDO(gCO, gCO->em, OD, nodeId, &errInfo);
    if (err != CO_ERROR_NO && err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) {
        MASTER_ERR("CO_CANopenInitPDO failed (%d) info=0x%lX\n", err, (unsigned long)errInfo);
        return false;
    }

    CO_CANsetNormalMode(gCO->CANmodule);
    MASTER_LOG("CANopen stack started for node %u @ %u kbit/s\n", nodeId, bitrate);
    return true;
}

static void
master_demo_canopen_service(uint32_t iterations) {
    CO_NMT_reset_cmd_t reset = CO_RESET_NOT;
    for (uint32_t i = 0; i < iterations && reset == CO_RESET_NOT; i++) {
        const uint32_t timeDifference_us = 1000U;
        reset = CO_process(gCO, false, timeDifference_us, NULL);
#if (CO_CONFIG_PDO) & CO_CONFIG_RPDO_ENABLE
        CO_process_RPDO(gCO, false, timeDifference_us, NULL);
#endif
#if (CO_CONFIG_PDO) & CO_CONFIG_TPDO_ENABLE
        CO_process_TPDO(gCO, false, timeDifference_us, NULL);
#endif
    }
}

static void
master_demo_canopen_shutdown(void) {
    if (gCO == NULL) {
        return;
    }
    CO_CANsetConfigurationMode(gCanInterface);
    CO_delete(gCO);
    gCO = NULL;
}

int
main(int argc, char** argv) {
    const char* firmwarePath = argc > 1 ? argv[1] : "..\\bins\\bye.bin";
    if (!master_demo_canopen_init(0x7EU, 250U)) {
        MASTER_ERR("Unable to start CANopen\n");
        return -1;
    }

    fw_upload_plan_t plan = {.firmwarePath = firmwarePath,
                             .type = FW_IMAGE_MAIN,
                             .targetBank = 1U,
                             .targetNodeId = 0x0AU,
                             .maxChunkBytes = 256U,
                             .expectedCrc = 0U};

    MASTER_LOG("Starting scripted firmware upload demo for %s\n", plan.firmwarePath);
    if (!fw_master_run_upload_session(&plan)) {
        MASTER_ERR("Firmware upload helpers reported failure\n");
        master_demo_canopen_shutdown();
        return -1;
    }

    MASTER_LOG("Upload helper finished; keeping CANopen node alive for a short period.\n");
    master_demo_canopen_service(5000U);
    master_demo_canopen_shutdown();
    MASTER_LOG("Demo complete. Replace the stubs inside fw_master_update.c with real SDO client calls to transfer data.\n");
    return 0;
}
