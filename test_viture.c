#include <stdio.h>
#include <unistd.h> // For sleep()
#include "viture_connection.h"

// MCU Event Callback Implementation
static void my_mcu_event_callback(uint16_t event_id, unsigned char *data, uint16_t len, uint32_t timestamp) {
    printf("MCU Event Received: ID=0x%04X, Len=%u, TS=%u, Data: ", event_id, len, timestamp);
    for (uint16_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

int main() {
    printf("Starting Viture Connection Test...\n");

    if (!viture_driver_init()) {
        fprintf(stderr, "Test: Failed to initialize Viture driver.\n");
        return 1;
    }
    printf("Test: Viture driver initialized successfully.\n");

    // Register MCU event callback
    viture_set_mcu_event_callback(my_mcu_event_callback);
    printf("Test: MCU event callback registered.\n");

    // Enable IMU
    printf("Test: Enabling IMU...\n");
    uint32_t imu_set_status = set_imu(true);
    if (imu_set_status == 0) { // Assuming 0 is success
        printf("Test: set_imu(true) command successful.\n");
    } else {
        fprintf(stderr, "Test: set_imu(true) command failed with status %u.\n", imu_set_status);
    }

    // Wait for some data/events
    // viture_imu_callback in viture_connection.c should be printing IMU data
    // my_mcu_event_callback here should print any MCU events
    printf("Test: Waiting for 10 seconds to receive IMU data and MCU events...\n");
    printf("Test: (Check console for 'Viture: Initial offsets captured...' and 'MCU Event Received...' messages)\n");
    for (int i = 0; i < 10; i++) {
        // Print live IMU data from globals exposed in header
        printf("Test: Live IMU Data - Roll: %.2f, Pitch: %.2f, Yaw: %.2f\n",
               viture_roll, viture_pitch, viture_yaw);
        sleep(1);
    }
    
    // Disable IMU
    printf("Test: Disabling IMU...\n");
    imu_set_status = set_imu(false);
     if (imu_set_status == 0) { // Assuming 0 is success
        printf("Test: set_imu(false) command successful.\n");
    } else {
        fprintf(stderr, "Test: set_imu(false) command failed with status %u.\n", imu_set_status);
    }

    // Close driver
    printf("Test: Closing Viture driver...\n");
    viture_driver_close();
    printf("Test: Viture driver closed.\n");

    printf("Viture Connection Test Finished.\n");
    return 0;
}
