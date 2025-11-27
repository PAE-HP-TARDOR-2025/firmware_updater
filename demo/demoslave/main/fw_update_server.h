#pragma once

#include <stdbool.h>
#include "CANopen.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the firmware download object handlers for the CANopen slave. */
bool fw_server_init(CO_t *co);

#ifdef __cplusplus
}
#endif
