#include "fw_master_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FW_MASTER_LOG
#define FW_MASTER_LOG(fmt, ...) printf("[FW-MASTER] " fmt, ##__VA_ARGS__)
#endif

#ifndef FW_MASTER_ERR
#define FW_MASTER_ERR(fmt, ...) printf("[FW-ERROR ] " fmt, ##__VA_ARGS__)
#endif

#define RETURN_IF_FALSE(cond, msg, ...)                                                              \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            FW_MASTER_ERR(msg "\n", ##__VA_ARGS__);                                                 \
            return false;                                                                            \
        }                                                                                            \
    } while (0)

bool
fw_master_load_payload(const fw_upload_plan_t* plan, fw_payload_t* payload) {
    FILE* f = fopen(plan->firmwarePath, "rb");
    RETURN_IF_FALSE(f != NULL, "Cannot open firmware file %s", plan->firmwarePath);

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        FW_MASTER_ERR("Failed to seek to end of %s\n", plan->firmwarePath);
        return false;
    }

    long fileSize = ftell(f);
    RETURN_IF_FALSE(fileSize > 0, "Firmware file %s is empty", plan->firmwarePath);

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        FW_MASTER_ERR("Failed to rewind file %s\n", plan->firmwarePath);
        return false;
    }

    payload->buffer = (uint8_t*)malloc((size_t)fileSize);
    RETURN_IF_FALSE(payload->buffer != NULL, "Out of memory while reading firmware");

    size_t readBytes = fread(payload->buffer, 1, (size_t)fileSize, f);
    fclose(f);
    RETURN_IF_FALSE(readBytes == (size_t)fileSize, "Short read: expected %ld bytes, got %zu", fileSize, readBytes);

    payload->size = readBytes;
    FW_MASTER_LOG("Loaded %zu bytes from %s\n", payload->size, plan->firmwarePath);
    return true;
}

uint16_t
fw_master_crc16(const uint8_t* data, size_t len) {
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

bool
fw_master_send_metadata(const fw_upload_plan_t* plan, const fw_payload_t* payload, uint16_t crc) {
    FW_MASTER_LOG("Sending metadata to slave node %u\n", plan->targetNodeId);
    FW_MASTER_LOG(" - image bytes : %zu\n", payload->size);
    FW_MASTER_LOG(" - crc         : 0x%04X\n", crc);
    FW_MASTER_LOG(" - image type  : %u\n", plan->type);
    FW_MASTER_LOG(" - bank        : %u\n", plan->targetBank);

    /* Replace this stub with real CO_SDOclientDownload* calls to index 0x1F57. */
    bool linkOk = true;
    RETURN_IF_FALSE(linkOk, "Metadata write failed (stub)");
    return true;
}

bool
fw_master_send_start_command(const fw_upload_plan_t* plan) {
    FW_MASTER_LOG("Issuing start command through object 0x1F51\n");
    /* Replace with real SDO write of the start token. */
    bool linkOk = true;
    RETURN_IF_FALSE(linkOk, "Control write failed (stub)");
    return true;
}

bool
fw_master_send_chunk(const fw_upload_plan_t* plan, const uint8_t* chunk, size_t len, size_t offset) {
    (void)plan;
    FW_MASTER_LOG("Sending chunk offset %zu size %zu\n", offset, len);
    /* Replace with block download segments (CO_SDOclientDownload). */
    bool linkOk = true;
    RETURN_IF_FALSE(linkOk, "Chunk transfer failed (stub)");
    return true;
}

bool
fw_master_send_finalize_request(const fw_upload_plan_t* plan, uint16_t crc) {
    (void)plan;
    FW_MASTER_LOG("Sending finalize request with crc 0x%04X\n", crc);
    bool linkOk = true;
    RETURN_IF_FALSE(linkOk, "Finalize write failed (stub)");
    return true;
}

bool
fw_master_stream_payload(const fw_upload_plan_t* plan, const fw_payload_t* payload) {
    size_t offset = 0;
    while (offset < payload->size) {
        size_t remaining = payload->size - offset;
        size_t len = remaining < plan->maxChunkBytes ? remaining : plan->maxChunkBytes;
        if (!fw_master_send_chunk(plan, payload->buffer + offset, len, offset)) {
            return false;
        }
        offset += len;
    }
    return true;
}

bool
fw_master_run_upload_session(const fw_upload_plan_t* plan) {
    fw_payload_t payload = {0};
    if (!fw_master_load_payload(plan, &payload)) {
        return false;
    }

    uint16_t crc = plan->expectedCrc;
    if (crc == 0U) {
        crc = fw_master_crc16(payload.buffer, payload.size);
        FW_MASTER_LOG("Auto-computed crc: 0x%04X\n", crc);
    }

    bool ok = fw_master_send_metadata(plan, &payload, crc) && fw_master_send_start_command(plan) &&
              fw_master_stream_payload(plan, &payload) && fw_master_send_finalize_request(plan, crc);

    free(payload.buffer);
    return ok;
}
