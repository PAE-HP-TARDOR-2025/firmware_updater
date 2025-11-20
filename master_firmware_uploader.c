/*
 * Firmware uploader demo that represents the point of view of the Controller Area Network Open master.
 *
 * The code is written to mirror the verbose and defensive style used by main_firmware_update.c so that
 * you can run both sides in lockstep while still keeping the transport logic easy to customize.
 *
 * Replace the stubbed "send_*" helpers with real Service Data Object client calls when you integrate this
 * into the application that actually drives the Controller Area Network bus.  Every helper currently prints
 * detailed steps so you can confirm the sequencing over a serial console even before you hook up hardware.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

enum {
    FW_META_INDEX = 0x1F57,
    FW_CTRL_INDEX = 0x1F51,
    FW_DATA_INDEX = 0x1F50,
    FW_STATUS_INDEX = 0x1F5A
};

typedef enum {
    FW_IMAGE_MAIN = 0,
    FW_IMAGE_BOOTLOADER = 1,
    FW_IMAGE_CONFIG = 2
} fw_image_type_t;

typedef struct {
    const char* firmwarePath;
    fw_image_type_t type;
    uint8_t targetBank;
    uint8_t targetNodeId;
    uint32_t maxChunkBytes;
    uint16_t expectedCrc;
} fw_upload_plan_t;

typedef struct {
    uint8_t* buffer;
    size_t size;
} fw_payload_t;

/* Read the firmware file from disk into memory so it can be sent over the bus. */
static bool
fw_load_payload(const fw_upload_plan_t* plan, fw_payload_t* payload) {
    FILE* f = fopen(plan->firmwarePath, "rb");
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

    payload->buffer = (uint8_t*)malloc((size_t)fileSize);
    RETURN_IF_FALSE(payload->buffer != NULL, "Out of memory while reading firmware");

    size_t readBytes = fread(payload->buffer, 1, (size_t)fileSize, f);
    fclose(f);
    RETURN_IF_FALSE(readBytes == (size_t)fileSize, "Short read: expected %ld bytes, got %zu", fileSize, readBytes);

    payload->size = readBytes;
    log_master("Loaded %zu bytes from %s\n", payload->size, plan->firmwarePath);
    return true;
}

/* Calculate the CRC-16 checksum used by the slave to validate transferred data. */
static uint16_t
fw_crc16(const uint8_t* data, size_t len) {
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

/* Push the metadata record (size, checksum, type, bank) into object 0x1F57. */
static bool
send_metadata_to_slave(const fw_upload_plan_t* plan, const fw_payload_t* payload, uint16_t crc) {
    log_master("Sending metadata to slave node %u\n", plan->targetNodeId);
    log_master(" - image bytes : %zu\n", payload->size);
    log_master(" - crc         : 0x%04X\n", crc);
    log_master(" - image type  : %u\n", plan->type);
    log_master(" - bank        : %u\n", plan->targetBank);

    /* Replace this with CO_SDOclientDownloadInitiate + CO_SDOclientDownload to index 0x1F57. */
    bool linkOk = true;
    RETURN_IF_FALSE(linkOk, "Metadata write failed (stub)" );
    return true;
}

/* Tell the slave to erase flash and enter download mode via object 0x1F51. */
static bool
send_start_command(const fw_upload_plan_t* plan) {
    log_master("Issuing start command through object 0x1F51\n");
    /* Replace with real SDO write of the start token. */
    bool linkOk = true;
    RETURN_IF_FALSE(linkOk, "Control write failed (stub)");
    return true;
}

/* Transfer one data chunk; in a real build this becomes a Service Data Object block download. */
static bool
send_chunk_to_slave(const fw_upload_plan_t* plan, const uint8_t* chunk, size_t len, size_t offset) {
    log_master("Sending chunk offset %zu size %zu\n", offset, len);
    /* Replace with block download segments (CO_SDOclientDownload). */
    bool linkOk = true;
    RETURN_IF_FALSE(linkOk, "Chunk transfer failed (stub)");
    return true;
}

/* Request final verification so the slave compares computed CRC with the advertised value. */
static bool
send_finalize_request(const fw_upload_plan_t* plan, uint16_t crc) {
    log_master("Sending finalize request with crc 0x%04X\n", crc);
    bool linkOk = true;
    RETURN_IF_FALSE(linkOk, "Finalize write failed (stub)");
    return true;
}

/* Iterate through the entire image, chunk by chunk, while keeping offsets aligned. */
static bool
fw_stream_payload(const fw_upload_plan_t* plan, const fw_payload_t* payload) {
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

/* High-level driver that loads the binary, computes CRC, and performs the full transaction. */
static bool
fw_run_upload_session(const fw_upload_plan_t* plan) {
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

/* Command-line entry point that prepares the upload plan and reports result codes. */
int
main(int argc, char** argv) {
    if (argc < 2) {
        log_error("Usage: master_firmware_uploader <firmware.bin> [nodeId] [bank]\n");
        return -1;
    }

    fw_upload_plan_t plan = {.firmwarePath = argv[1],
                             .type = FW_IMAGE_MAIN,
                             .targetBank = argc > 3 ? (uint8_t)atoi(argv[3]) : 1U,
                             .targetNodeId = argc > 2 ? (uint8_t)atoi(argv[2]) : 10U,
                             .maxChunkBytes = 256U,
                             .expectedCrc = 0U};

    if (!fw_run_upload_session(&plan)) {
        log_error("Firmware upload sequence failed\n");
        return -1;
    }

    log_master("Firmware upload sequence completed; request a network reset to boot the new image.\n");
    return 0;
}
