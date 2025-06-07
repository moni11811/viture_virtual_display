#ifndef VITURE_CONNECTION_H
#define VITURE_CONNECTION_H

#include <stdbool.h>
#include <stdint.h> // For uint types used in callback

// Forward declare uchar, ushort, uint if they are not standard types in this header's context
// However, it's better to use standard types like uint8_t, uint16_t, uint32_t directly
// For simplicity, assuming uchar, ushort, uint are typedef'd appropriately if needed by callback users
// Or, more robustly, use uint8_t etc. in the callback signature.
// The current implementation uses uchar, ushort, uint as typedefs for unsigned char/short/int.

// Callback type for MCU events
typedef void (*viture_mcu_event_callback_t)(uint16_t event_id, unsigned char *data, uint16_t len, uint32_t timestamp);

// Initializes the Viture driver (HID communication, threads, etc.)
// Returns true on success, false on failure.
bool viture_driver_init(void);

// Closes the Viture driver, cleans up resources.
void viture_driver_close(void);

// Enables or disables the IMU data stream.
// Returns a status code from the device (0 typically means success).
uint32_t set_imu(bool enable);

// Executes a raw MCU command with a single byte payload.
// Returns a status code from the device.
uint32_t native_mcu_exec(uint16_t cmd_id, unsigned char data_byte);

// Registers a callback function to receive asynchronous MCU events.
void viture_set_mcu_event_callback(viture_mcu_event_callback_t callback);

// Expose IMU data getters if needed by test (already volatile globals in .c)
// For a test program, direct access to viture_roll, viture_pitch, viture_yaw might be okay,
// or dedicated getter functions could be added.
// For now, the test will rely on printf statements within viture_imu_callback.

extern volatile float viture_roll;
extern volatile float viture_pitch;
extern volatile float viture_yaw;


#endif // VITURE_CONNECTION_H
