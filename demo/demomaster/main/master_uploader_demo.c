#include "master_uploader_demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "CANopen.h"

#define log_master(fmt, ...) printf("[FW-MASTER] " fmt, ##__VA_ARGS__)
#define log_error(fmt, ...)  printf("[FW-ERROR ] " fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)   printf("[FW-WARN  ] " fmt, ##__VA_ARGS__)

#define RETURN_IF_FALSE(cond, msg, ...)                                                                                \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            log_error(msg "\n", ##__VA_ARGS__);                                                                       \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

#define SDO_TIMEOUT_US 1000000U
#define SDO_POLL_US     1000U

enum {
    FW_META_INDEX = 0x1F57,
    FW_CTRL_INDEX = 0x1F51,
    FW_DATA_INDEX = 0x1F50,
    FW_STATUS_INDEX = 0x1F5A
};

typedef struct {
    uint8_t *buffer;
    size_t size;
} fw_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t imageBytes;
    uint16_t crc;
    uint8_t imageType;
    uint8_t bank;
} fw_metadata_record_t;

enum {
    FW_CTRL_CMD_START = 0x01
};

static CO_SDOclient_t *s_sdo_client = NULL;
static uint8_t s_bound_node_id = 0U;

static bool fw_master_select_target(uint8_t nodeId);
static bool fw_sdo_download(uint16_t index, uint8_t subIndex, const uint8_t *data, size_t len, const char *label);

bool fw_master_bind_sdo_client(CO_SDOclient_t *client) {
    s_sdo_client = client;
    s_bound_node_id = 0U;
    return s_sdo_client != NULL;
}

static bool fw_master_select_target(uint8_t nodeId) {
    RETURN_IF_FALSE(s_sdo_client != NULL, "SDO client not available");
    if (s_bound_node_id == nodeId) {
        return true;
    }

    CO_SDO_return_t ret =
        CO_SDOclient_setup(s_sdo_client, CO_CAN_ID_SDO_CLI + nodeId, CO_CAN_ID_SDO_SRV + nodeId, nodeId);
    RETURN_IF_FALSE(ret == CO_SDO_RT_ok_communicationEnd, "CO_SDOclient_setup failed (ret=%d)", ret);

    s_bound_node_id = nodeId;
    return true;
}

static bool fw_sdo_download(uint16_t index, uint8_t subIndex, const uint8_t *data, size_t len, const char *label) {
    RETURN_IF_FALSE(s_sdo_client != NULL, "SDO client not available");

    CO_SDO_return_t ret = CO_SDOclientDownloadInitiate(s_sdo_client, index, subIndex, len, SDO_TIMEOUT_US, false);
    RETURN_IF_FALSE(ret == CO_SDO_RT_ok_communicationEnd, "SDO init failed for %s (ret=%d)", label, ret);

    size_t totalWritten = 0U;
    bool_t bufferPartial = false;
    if (len > 0U) {
        size_t written = CO_SDOclientDownloadBufWrite(s_sdo_client, data, len);
        totalWritten = written;
        bufferPartial = totalWritten < len;
    }

    do {
        CO_SDO_abortCode_t abortCode = CO_SDO_AB_NONE;
        ret = CO_SDOclientDownload(s_sdo_client, SDO_POLL_US, false, bufferPartial, &abortCode, NULL, NULL);
        if (ret < 0) {
            log_error("SDO download for %s aborted (0x%08X)", label, abortCode);
            return false;
        }

        if (ret > 0) {
            if (bufferPartial && totalWritten < len) {
                size_t written = CO_SDOclientDownloadBufWrite(s_sdo_client, data + totalWritten, len - totalWritten);
                totalWritten += written;
                bufferPartial = totalWritten < len;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    } while (ret > 0);

    return true;
}

static bool fw_load_payload(const fw_upload_plan_t *plan, fw_payload_t *payload) {
    FILE *f = fopen(plan->firmwarePath, "rb");
    RETURN_IF_FALSE(f != NULL, "Cannot open firmware file %s", plan->firmwarePath);

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        log_error("Failed to seek to end of %s\n", plan->firmwarePath);
        return false;
    }

    long fileSize = ftell(f);
    RETURN_IF_FALSE(fileSize > 0, "Firmware file %s is empty", plan->firmwarePath);

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        log_error("Failed to rewind file %s\n", plan->firmwarePath);
        return false;
    }

    payload->buffer = (uint8_t *)malloc((size_t)fileSize);
    RETURN_IF_FALSE(payload->buffer != NULL, "Out of memory while reading firmware");

    size_t readBytes = fread(payload->buffer, 1, (size_t)fileSize, f);
    fclose(f);
    RETURN_IF_FALSE(readBytes == (size_t)fileSize, "Short read: expected %ld bytes, got %zu", fileSize, readBytes);

    payload->size = readBytes;
    log_master("Loaded %zu bytes from %s\n", payload->size, plan->firmwarePath);
    return true;
}

static uint16_t fw_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static bool send_metadata_to_slave(const fw_upload_plan_t *plan, const fw_payload_t *payload, uint16_t crc) {
    log_master("Sending metadata to slave node %u\n", plan->targetNodeId);
    log_master(" - image bytes : %zu\n", payload->size);
    log_master(" - crc         : 0x%04X\n", crc);
    log_master(" - image type  : %u\n", plan->type);
    log_master(" - bank        : %u\n", plan->targetBank);
    RETURN_IF_FALSE(fw_master_select_target(plan->targetNodeId), "Unable to reach node %u", plan->targetNodeId);

    const fw_metadata_record_t meta = {
        .imageBytes = (uint32_t)payload->size,
        .crc = crc,
        .imageType = (uint8_t)plan->type,
        .bank = plan->targetBank};

    return fw_sdo_download(FW_META_INDEX, 1U, (const uint8_t *)&meta, sizeof(meta), "metadata");
}

static bool send_start_command(const fw_upload_plan_t *plan) {
    log_master("Issuing start command through object 0x1F51\n");
    RETURN_IF_FALSE(fw_master_select_target(plan->targetNodeId), "Unable to reach node %u", plan->targetNodeId);

    const uint8_t controlPayload[3] = {FW_CTRL_CMD_START, (uint8_t)plan->type, plan->targetBank};
    return fw_sdo_download(FW_CTRL_INDEX, 1U, controlPayload, sizeof(controlPayload), "start command");
}

static bool send_chunk_to_slave(const fw_upload_plan_t *plan, const uint8_t *chunk, size_t len, size_t offset) {
    log_master("Sending chunk offset %zu size %zu\n", offset, len);
    RETURN_IF_FALSE(fw_master_select_target(plan->targetNodeId), "Unable to reach node %u", plan->targetNodeId);
    return fw_sdo_download(FW_DATA_INDEX, 1U, chunk, len, "chunk");
}

static bool send_finalize_request(const fw_upload_plan_t *plan, uint16_t crc) {
    log_master("Sending finalize request with crc 0x%04X\n", crc);
    RETURN_IF_FALSE(fw_master_select_target(plan->targetNodeId), "Unable to reach node %u", plan->targetNodeId);
    uint8_t crcBytes[2] = {(uint8_t)(crc & 0xFFU), (uint8_t)(crc >> 8)};
    return fw_sdo_download(FW_STATUS_INDEX, 1U, crcBytes, sizeof(crcBytes), "finalize request");
}

static bool fw_stream_payload(const fw_upload_plan_t *plan, const fw_payload_t *payload) {
    size_t offset = 0;
    while (offset < payload->size) {
        size_t remaining = payload->size - offset;
        size_t len = remaining < plan->maxChunkBytes ? remaining : plan->maxChunkBytes;
        if (!send_chunk_to_slave(plan, payload->buffer + offset, len, offset)) {
            return false;
        }
        offset += len;
    }
    return true;
}

bool fw_run_upload_session(const fw_upload_plan_t *plan) {
    RETURN_IF_FALSE(plan != NULL, "Upload plan is NULL");
    RETURN_IF_FALSE(s_sdo_client != NULL, "CANopen transport not bound");
    RETURN_IF_FALSE(fw_master_select_target(plan->targetNodeId), "Failed to select node %u", plan->targetNodeId);

    fw_payload_t payload = {0};
    if (!fw_load_payload(plan, &payload)) {
        return false;
    }

    uint16_t crc = plan->expectedCrc;
    if (crc == 0U) {
        crc = fw_crc16(payload.buffer, payload.size);
        log_master("Auto-computed crc: 0x%04X\n", crc);
    }

    bool ok = send_metadata_to_slave(plan, &payload, crc) && send_start_command(plan) &&
              fw_stream_payload(plan, &payload) && send_finalize_request(plan, crc);

    free(payload.buffer);
    return ok;
}
