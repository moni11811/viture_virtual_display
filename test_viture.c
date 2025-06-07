#include <stdio.h>
#include <unistd.h> // For sleep()
#include "viture_connection.h"

static bool use_viture_imu = false;
// Use volatile to prevent compiler from optimizing away reads, as these are
// updated by a different thread (the Viture callback thread).
/*static*/ volatile float viture_roll = 0.0f; // Remove static for external linkage
/*static*/ volatile float viture_pitch = 0.0f; // Remove static for external linkage
/*static*/ volatile float viture_yaw = 0.0f;   // Remove static for external linkage
static float initial_roll_offset = 0.0f;  
static float initial_pitch_offset = 0.0f; 
static float initial_yaw_offset = 0.0f;  
static bool initial_offsets_set = false; 


// --- Helper Functions ---

static float makeFloat(uint8_t *data) {
    float value = 0;
    uint8_t tem[4];
    tem[0] = data[3];
    tem[1] = data[2];
    tem[2] = data[1];
    tem[3] = data[0];
    memcpy(&value, tem, 4);
    return value;
}

// Renamed from viture_imu_callback, made non-static for external use if needed (e.g. by test program as default)
void default_viture_imu_data_handler(uint8_t *data, uint16_t len, uint32_t ts) {
    // We only need the first 12 bytes for Euler angles
    if (len < 12) return;
    // ts parameter is unused in this default handler, but kept for consistent callback signature
    (void)ts; 

    // NEW: Set initial offsets on first valid IMU data if using Viture
    if (use_viture_imu && !initial_offsets_set) {
        initial_roll_offset = makeFloat(data);
        initial_pitch_offset = makeFloat(data + 4); 
        initial_yaw_offset = -makeFloat(data + 8);
        initial_offsets_set = true;
        printf("Viture: Initial offsets captured: Roll=%f, Pitch=%f, Yaw=%f\n", initial_roll_offset, initial_pitch_offset, initial_yaw_offset);
    }

    // Adjust pitch and yaw based on feedback.
    // Roll remains as is.
    viture_roll = makeFloat(data);
    viture_pitch = makeFloat(data + 4);
    viture_yaw = -makeFloat(data + 8);
}

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

    // Register IMU data callback (using the default handler from viture_connection.c)
    viture_set_imu_data_callback(default_viture_imu_data_handler);
    printf("Test: IMU data callback registered (using default_viture_imu_data_handler).\n");

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
