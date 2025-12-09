/**
 * ESP-IDF CANopen Master Firmware Uploader
 *
 * This application:
 *  - Initializes CANopenNode as a master
 *  - Mounts SPIFFS to access firmware binaries
 *  - Queries slave's running firmware CRC via SDO upload (0x1F5B:01)
 *  - If CRC differs, uploads new firmware via CiA-302 objects
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/twai.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "CANopen.h"
#include "OD.h"

#if CONFIG_MASTER_USE_SPIFFS
#include "esp_spiffs.h"
#endif

#include "fw_master_update.h"

static const char *TAG = "master_main";

/* CANopen interrupt handler (defined in CO_driver.c) */
extern void CO_CANinterrupt(CO_CANmodule_t *CANmodule);

#define NMT_CONTROL \
    (CO_NMT_STARTUP_TO_OPERATIONAL | CO_NMT_ERR_ON_ERR_REG | CO_ERR_REG_GENERIC_ERR | CO_ERR_REG_COMMUNICATION)
#define FIRST_HB_TIME        500U
#define SDO_SRV_TIMEOUT_TIME 1000U
#define SDO_CLI_TIMEOUT_TIME 3000U  /* Increased for block transfer */
#define SDO_CLI_BLOCK        true  /* Enable block transfer for faster OTA */

typedef struct {
    CO_t *co;
    CO_SDOclient_t *sdoClient;
    TaskHandle_t processTask;
    TaskHandle_t rxTask;
    TaskHandle_t uploaderTask;
    bool started;
} master_ctx_t;

static master_ctx_t g_master = {0};

/* Forward declarations */
static bool master_canopen_init(void);
static void canopen_process_task(void *arg);
static void canopen_rx_task(void *arg);
static void uploader_task(void *arg);

/* SDO client wrappers for fw_master_update */
static bool sdo_download(uint8_t nodeId, uint16_t index, uint8_t subIndex, const uint8_t *data, size_t len);
static bool sdo_upload(uint8_t nodeId, uint16_t index, uint8_t subIndex, uint8_t *data, size_t maxLen, size_t *actualLen);

static TickType_t wait_ticks(uint32_t ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static void init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

#if CONFIG_MASTER_USE_SPIFFS
static void init_spiffs(void) {
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_MASTER_SPIFFS_BASE_PATH,
        .partition_label = CONFIG_MASTER_SPIFFS_PARTITION_LABEL,
        .max_files = 4,
        .format_if_mount_failed = false
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_FAIL) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS");
    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "SPIFFS partition not found");
    } else {
        ESP_ERROR_CHECK(err);
        size_t total = 0, used = 0;
        ESP_ERROR_CHECK(esp_spiffs_info(conf.partition_label, &total, &used));
        ESP_LOGI(TAG, "SPIFFS: total=%u used=%u bytes", (unsigned)total, (unsigned)used);
    }
}
#endif

/* ------------------------------------------------------------------------- */
/* SDO Client Wrappers                                                       */
/* ------------------------------------------------------------------------- */

static bool sdo_download(uint8_t nodeId, uint16_t index, uint8_t subIndex, const uint8_t *data, size_t len) {
    if (g_master.sdoClient == NULL) {
        ESP_LOGE(TAG, "SDO client not initialized");
        return false;
    }

    CO_SDO_abortCode_t abortCode = CO_SDO_AB_NONE;
    size_t sizeTransferred = 0;
    uint32_t timeoutMs = SDO_CLI_TIMEOUT_TIME;

    /* Setup SDO client for target node */
    CO_SDOclient_setup(g_master.sdoClient,
                       0x600 + nodeId,  /* COB-ID client->server */
                       0x580 + nodeId,  /* COB-ID server->client */
                       nodeId);

    CO_SDO_return_t ret = CO_SDOclientDownloadInitiate(g_master.sdoClient, index, subIndex, len, timeoutMs, SDO_CLI_BLOCK);
    if (ret < CO_SDO_RT_ok_communicationEnd) {
        ESP_LOGE(TAG, "SDO download init failed: %d", ret);
        return false;
    }

    /* 
     * Write data progressively and process download in a loop.
     * SDO buffer is small (32 bytes by default), so we fill it as space becomes available.
     */
    size_t dataOffset = 0;
    bool bufferPartial = true;  /* Indicates more data to write */

    do {
        /* Fill buffer with as much data as it can accept */
        if (dataOffset < len) {
            size_t written = CO_SDOclientDownloadBufWrite(g_master.sdoClient, data + dataOffset, len - dataOffset);
            dataOffset += written;
            bufferPartial = (dataOffset < len);
        } else {
            bufferPartial = false;
        }

        ret = CO_SDOclientDownload(g_master.sdoClient, 1000, false, bufferPartial, &abortCode, &sizeTransferred, NULL);
        if (ret == CO_SDO_RT_waitingResponse) {
            vTaskDelay(wait_ticks(1));
        }
    } while (ret > CO_SDO_RT_ok_communicationEnd);

    if (ret != CO_SDO_RT_ok_communicationEnd) {
        ESP_LOGE(TAG, "SDO download failed: ret=%d abort=0x%08lX", ret, (unsigned long)abortCode);
        return false;
    }

    return true;
}

static bool sdo_upload(uint8_t nodeId, uint16_t index, uint8_t subIndex, uint8_t *data, size_t maxLen, size_t *actualLen) {
    if (g_master.sdoClient == NULL) {
        ESP_LOGE(TAG, "SDO client not initialized");
        return false;
    }

    CO_SDO_abortCode_t abortCode = CO_SDO_AB_NONE;
    size_t sizeTransferred = 0;
    uint32_t timeoutMs = SDO_CLI_TIMEOUT_TIME;

    /* Setup SDO client for target node */
    CO_SDOclient_setup(g_master.sdoClient,
                       0x600 + nodeId,
                       0x580 + nodeId,
                       nodeId);

    CO_SDO_return_t ret = CO_SDOclientUploadInitiate(g_master.sdoClient, index, subIndex, timeoutMs, SDO_CLI_BLOCK);
    if (ret < CO_SDO_RT_ok_communicationEnd) {
        ESP_LOGE(TAG, "SDO upload init failed: %d", ret);
        return false;
    }

    /* Process upload */
    size_t sizeIndicated = 0;
    do {
        ret = CO_SDOclientUpload(g_master.sdoClient, 1000, false, &abortCode, &sizeIndicated, &sizeTransferred, NULL);
        if (ret == CO_SDO_RT_waitingResponse) {
            vTaskDelay(wait_ticks(1));
        }
    } while (ret > CO_SDO_RT_ok_communicationEnd);

    if (ret != CO_SDO_RT_ok_communicationEnd) {
        ESP_LOGE(TAG, "SDO upload failed: ret=%d abort=0x%08lX", ret, (unsigned long)abortCode);
        return false;
    }

    /* Read data from buffer */
    size_t dataSize = CO_SDOclientUploadBufRead(g_master.sdoClient, data, maxLen);
    if (actualLen != NULL) {
        *actualLen = dataSize;
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/* FW Master Update Implementations                                          */
/* ------------------------------------------------------------------------- */

bool fw_master_send_metadata(const fw_upload_plan_t *plan, const fw_payload_t *payload, uint16_t crc) {
    ESP_LOGI(TAG, "Sending metadata: size=%zu crc=0x%04X type=%u bank=%u version=%u",
             payload->size, crc, plan->type, plan->targetBank, plan->firmwareVersion);

    /* Metadata format: [size(4) | crc(2) | type(1) | bank(1) | version(2)] = 10 bytes */
    uint8_t meta[10];
    uint32_t size = (uint32_t)payload->size;
    meta[0] = (uint8_t)(size & 0xFF);
    meta[1] = (uint8_t)((size >> 8) & 0xFF);
    meta[2] = (uint8_t)((size >> 16) & 0xFF);
    meta[3] = (uint8_t)((size >> 24) & 0xFF);
    meta[4] = (uint8_t)(crc & 0xFF);
    meta[5] = (uint8_t)((crc >> 8) & 0xFF);
    meta[6] = (uint8_t)plan->type;
    meta[7] = plan->targetBank;
    meta[8] = (uint8_t)(plan->firmwareVersion & 0xFF);
    meta[9] = (uint8_t)((plan->firmwareVersion >> 8) & 0xFF);

    return sdo_download(plan->targetNodeId, 0x1F57, 1, meta, sizeof(meta));
}

bool fw_master_send_start_command(const fw_upload_plan_t *plan) {
    ESP_LOGI(TAG, "Sending start command to node %u", plan->targetNodeId);
    uint8_t cmd[3] = {0x01, 0x00, 0x00}; /* Start token */
    return sdo_download(plan->targetNodeId, 0x1F51, 1, cmd, sizeof(cmd));
}

bool fw_master_send_chunk(const fw_upload_plan_t *plan, const uint8_t *chunk, size_t len, size_t offset) {
    ESP_LOGD(TAG, "Sending chunk: offset=%zu len=%zu", offset, len);
    return sdo_download(plan->targetNodeId, 0x1F50, 1, chunk, len);
}

bool fw_master_send_finalize_request(const fw_upload_plan_t *plan, uint16_t crc) {
    ESP_LOGI(TAG, "Sending finalize with CRC 0x%04X", crc);
    uint8_t status[2] = {(uint8_t)(crc & 0xFF), (uint8_t)((crc >> 8) & 0xFF)};
    return sdo_download(plan->targetNodeId, 0x1F5A, 1, status, sizeof(status));
}

bool fw_master_query_slave_crc(const fw_upload_plan_t *plan, uint16_t *slaveCrc) {
    ESP_LOGI(TAG, "Querying slave CRC from node %u (0x1F5B:01)", plan->targetNodeId);

    uint8_t buf[2] = {0};
    size_t actualLen = 0;

    if (!sdo_upload(plan->targetNodeId, 0x1F5B, 1, buf, sizeof(buf), &actualLen)) {
        ESP_LOGW(TAG, "Failed to query slave CRC");
        return false;
    }

    if (actualLen < 2) {
        ESP_LOGW(TAG, "Short response from slave CRC query: %zu bytes", actualLen);
        return false;
    }

    *slaveCrc = (uint16_t)(buf[0] | (buf[1] << 8));
    ESP_LOGI(TAG, "Slave running firmware CRC: 0x%04X", *slaveCrc);
    return true;
}

bool fw_master_query_slave_version(const fw_upload_plan_t *plan, uint16_t *slaveVersion) {
    ESP_LOGI(TAG, "Querying slave version from node %u (0x1F5C:01)", plan->targetNodeId);

    uint8_t buf[2] = {0};
    size_t actualLen = 0;

    if (!sdo_upload(plan->targetNodeId, 0x1F5C, 1, buf, sizeof(buf), &actualLen)) {
        ESP_LOGW(TAG, "Failed to query slave version");
        return false;
    }

    if (actualLen < 2) {
        ESP_LOGW(TAG, "Short response from slave version query: %zu bytes", actualLen);
        return false;
    }

    *slaveVersion = (uint16_t)(buf[0] | (buf[1] << 8));
    ESP_LOGI(TAG, "Slave running firmware version: %u", *slaveVersion);
    return true;
}

/* ------------------------------------------------------------------------- */
/* CANopen Tasks                                                             */
/* ------------------------------------------------------------------------- */

static void canopen_process_task(void *arg) {
    master_ctx_t *ctx = (master_ctx_t *)arg;
    int64_t last = esp_timer_get_time();

    while (true) {
        if (ctx->co != NULL) {
            int64_t now = esp_timer_get_time();
            uint32_t diffUs = (uint32_t)(now - last);
            last = now;

            CO_NMT_reset_cmd_t reset = CO_process(ctx->co, false, diffUs, NULL);
            if (reset != CO_RESET_NOT) {
                ESP_LOGW(TAG, "CANopen reset requested: %d", reset);
            }
        }
        vTaskDelay(wait_ticks(1));
    }
}

static void canopen_rx_task(void *arg) {
    master_ctx_t *ctx = (master_ctx_t *)arg;

    while (true) {
        if (ctx->co != NULL && ctx->co->CANmodule != NULL && ctx->co->CANmodule->CANnormal) {
            CO_CANinterrupt(ctx->co->CANmodule);
        } else {
            vTaskDelay(wait_ticks(10));
        }
    }
}

static void uploader_task(void *arg) {
    (void)arg;  /* Unused */

    /* Wait for CANopen to be ready */
    vTaskDelay(wait_ticks(2000));

    ESP_LOGI(TAG, "Starting firmware upload task");

    fw_upload_plan_t plan = {
        .firmwarePath = CONFIG_MASTER_FIRMWARE_PATH,
        .type = FW_IMAGE_MAIN,
        .targetBank = 1,
        .targetNodeId = CONFIG_MASTER_TARGET_NODE_ID,
        .maxChunkBytes = CONFIG_MASTER_MAX_CHUNK_BYTES,
        .expectedCrc = 0,
        .firmwareVersion = CONFIG_MASTER_FIRMWARE_VERSION
    };

    ESP_LOGI(TAG, "Upload plan: file=%s node=%u version=%u", plan.firmwarePath, plan.targetNodeId, plan.firmwareVersion);

    /* Streaming upload - don't load entire file to RAM */
    FILE *f = fopen(plan.firmwarePath, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Cannot open firmware file: %s", plan.firmwarePath);
        vTaskDelete(NULL);
        return;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fileSize <= 0) {
        ESP_LOGE(TAG, "Invalid firmware file size");
        fclose(f);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Firmware file size: %ld bytes", fileSize);

    /* Compute CRC by streaming through file */
    uint16_t crc = 0xFFFFU;
    uint8_t *chunk = malloc(plan.maxChunkBytes);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "Failed to allocate chunk buffer");
        fclose(f);
        vTaskDelete(NULL);
        return;
    }

    size_t bytesRead;
    while ((bytesRead = fread(chunk, 1, plan.maxChunkBytes, f)) > 0) {
        for (size_t i = 0; i < bytesRead; i++) {
            crc ^= (uint16_t)chunk[i] << 8;
            for (int bit = 0; bit < 8; bit++) {
                if (crc & 0x8000U) {
                    crc = (uint16_t)((crc << 1) ^ 0x1021U);
                } else {
                    crc <<= 1;
                }
            }
        }
    }
    ESP_LOGI(TAG, "Computed CRC: 0x%04X", crc);

#if CONFIG_MASTER_SKIP_IF_CRC_MATCH
    /* Check if slave already has this firmware (CRC AND version must match) */
    uint16_t slaveCrc = 0;
    uint16_t slaveVer = 0;
    bool crcQueried = fw_master_query_slave_crc(&plan, &slaveCrc);
    bool verQueried = fw_master_query_slave_version(&plan, &slaveVer);

    if (crcQueried && verQueried) {
        if (slaveCrc == crc && slaveVer == plan.firmwareVersion) {
            ESP_LOGI(TAG, "Slave already has matching firmware (CRC=0x%04X, ver=%u), skipping upload", 
                     slaveCrc, slaveVer);
            free(chunk);
            fclose(f);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "Slave firmware differs: CRC=0x%04X (local=0x%04X), ver=%u (local=%u), proceeding", 
                 slaveCrc, crc, slaveVer, plan.firmwareVersion);
    } else if (crcQueried) {
        ESP_LOGW(TAG, "Slave CRC=0x%04X (version query failed), proceeding", slaveCrc);
    } else {
        ESP_LOGW(TAG, "Could not query slave CRC, proceeding with upload");
    }
#endif

    /* Create pseudo-payload for metadata */
    fw_payload_t payload = { .buffer = NULL, .size = (size_t)fileSize };

    /* Send metadata */
    if (!fw_master_send_metadata(&plan, &payload, crc)) {
        ESP_LOGE(TAG, "Failed to send metadata");
        free(chunk);
        fclose(f);
        vTaskDelete(NULL);
        return;
    }

    /* Send start command */
    if (!fw_master_send_start_command(&plan)) {
        ESP_LOGE(TAG, "Failed to send start command");
        free(chunk);
        fclose(f);
        vTaskDelete(NULL);
        return;
    }

    /* Stream firmware data */
    fseek(f, 0, SEEK_SET);
    size_t offset = 0;
    size_t totalSent = 0;

    while ((bytesRead = fread(chunk, 1, plan.maxChunkBytes, f)) > 0) {
        if (!fw_master_send_chunk(&plan, chunk, bytesRead, offset)) {
            ESP_LOGE(TAG, "Failed to send chunk at offset %zu", offset);
            free(chunk);
            fclose(f);
            vTaskDelete(NULL);
            return;
        }
        offset += bytesRead;
        totalSent += bytesRead;
        
        /* Progress every 10% */
        if ((totalSent * 10 / fileSize) != ((totalSent - bytesRead) * 10 / fileSize)) {
            ESP_LOGI(TAG, "Upload progress: %zu/%ld bytes (%d%%)", 
                     totalSent, fileSize, (int)(totalSent * 100 / fileSize));
        }
    }

    ESP_LOGI(TAG, "Sent %zu bytes total", totalSent);

    /* Send finalize */
    if (!fw_master_send_finalize_request(&plan, crc)) {
        ESP_LOGE(TAG, "Failed to send finalize request");
        free(chunk);
        fclose(f);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Firmware upload completed successfully!");

    free(chunk);
    fclose(f);

    /* Task done, delete itself */
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* CANopen Initialization                                                    */
/* ------------------------------------------------------------------------- */

static bool master_canopen_init(void) {
    if (g_master.started) {
        return true;
    }

    uint32_t heapBytes = 0;

    /* Allocate CANopen object (pass NULL when CO_MULTIPLE_OD is not defined) */
    g_master.co = CO_new(NULL, &heapBytes);
    if (g_master.co == NULL) {
        ESP_LOGE(TAG, "CO_new failed");
        return false;
    }
    ESP_LOGI(TAG, "CANopen allocated %u bytes", (unsigned)heapBytes);

    /* Initialize CANopen - CO_CANinit will install the TWAI driver */
    CO_ReturnError_t err;

    err = CO_CANinit(g_master.co, NULL, 500);  /* 500 kbps to match slave */
    if (err != CO_ERROR_NO) {
        ESP_LOGE(TAG, "CO_CANinit failed: %d", err);
        return false;
    }

    err = CO_CANopenInit(g_master.co,
                         NULL, NULL, OD,
                         NULL, NMT_CONTROL,
                         FIRST_HB_TIME,
                         SDO_SRV_TIMEOUT_TIME,
                         SDO_CLI_TIMEOUT_TIME,
                         SDO_CLI_BLOCK,
                         CONFIG_MASTER_NODE_ID,
                         NULL);
    if (err != CO_ERROR_NO) {
        ESP_LOGE(TAG, "CO_CANopenInit failed: %d", err);
        return false;
    }

    /* Get SDO client reference */
    g_master.sdoClient = g_master.co->SDOclient;
    if (g_master.sdoClient == NULL) {
        ESP_LOGE(TAG, "SDO client not available");
        return false;
    }

    /* Start CANopen */
    CO_CANsetNormalMode(g_master.co->CANmodule);
    ESP_LOGI(TAG, "CANopen started, node ID %u", CONFIG_MASTER_NODE_ID);

    g_master.started = true;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Main Entry Point                                                          */
/* ------------------------------------------------------------------------- */

void app_main(void) {
    ESP_LOGI(TAG, "Master Firmware Uploader starting...");

    init_nvs();

#if CONFIG_MASTER_USE_SPIFFS
    init_spiffs();
#endif

    if (!master_canopen_init()) {
        ESP_LOGE(TAG, "Failed to initialize CANopen");
        return;
    }

    /* Start CANopen tasks */
    xTaskCreate(canopen_process_task, "co_process", 4096, &g_master, 5, &g_master.processTask);
    xTaskCreate(canopen_rx_task, "co_rx", 4096, &g_master, 10, &g_master.rxTask);

#if CONFIG_MASTER_UPLOAD_ON_STARTUP
    xTaskCreate(uploader_task, "uploader", 8192, &g_master, 3, &g_master.uploaderTask);
#endif

    ESP_LOGI(TAG, "Master running");
}
