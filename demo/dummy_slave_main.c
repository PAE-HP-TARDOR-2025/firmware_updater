/*
 * Minimal ESP-IDF entry point that prints a greeting over the serial monitor.
 * Build this file twice with different SLAVE_GREETING values to demonstrate
 * how a firmware update can change observable behaviour.
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef SLAVE_GREETING
#define SLAVE_GREETING "Hello from slave"
#endif

/* Marker string makes it easy for the desktop demo to locate the greeting in the binary. */
static const char greeting_storage[] = "GREETING:" SLAVE_GREETING;

void
app_main(void) {
    (void)greeting_storage; /* prevent unused warning */
    printf("[SLAVE] Boot image built on %s %s\n", __DATE__, __TIME__);
    printf("[SLAVE] Greeting: %s\n", SLAVE_GREETING);

    while (true) {
        printf("[SLAVE] %s\n", SLAVE_GREETING);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
