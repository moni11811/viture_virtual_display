/*  Read the IMU information from the Viture Pro XR glasses 
    data is sent through HID via USB

    The data is polled continuously from the glasses and the callback is called with the data

    This file implements a reverse engineered version of the protocol instead of the official SDK.


    There are two hid interfaces the MCU and IMU

    MCU is used for sending commands to the glasses and receiving events
    IMU is used for receiving the IMU data
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <pthread.h> 
#include <stdint.h> // For uint8_t, uint16_t, uint32_t
#include <stdbool.h>

#include <hidapi/hidapi.h>
#include <sys/time.h> // For gettimeofday

typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;

// VID and PID for Viture glasses
#define VITURE_VENDOR_ID  0x35CA
#define MCU_INTERFACE_NUMBER 1 // Common for Viture One
#define IMU_INTERFACE_NUMBER 0 // Common for Viture One


// --- CRC Calculation ---
static unsigned short aus_CrcTable[256];
static bool crc_table_initialized = false;

static void init_crc_table(void) {
    if (crc_table_initialized) return;

    ushort polynomial = 0x1021; // CRC-16-CCITT
    for (int i = 0; i < 256; i++) {
        ushort c = (ushort)i << 8;
        for (int j = 0; j < 8; j++) {
            if ((c & 0x8000) != 0) {
                c = (c << 1) ^ polynomial;
            } else {
                c = c << 1;
            }
        }
        aus_CrcTable[i] = c;
    }
    crc_table_initialized = true;
}

// cmd_crc(unsigned char*, unsigned short)
static ushort cmd_crc(uchar *data, ushort len) {
    if (!crc_table_initialized) {
        // This should ideally be called once at init, but as a fallback:
        fprintf(stderr, "Warning: CRC table not initialized, initializing now.\n");
        init_crc_table();
    }
    ushort crc = 0; // Initial value for CRC-16-CCITT is often 0xFFFF or 0x0000.
                    // The decompiled code implies 0x0000.
    for (ushort i = 0; i < len; i++) {
        crc = aus_CrcTable[(data[i] ^ (crc >> 8)) & 0xFF] ^ (crc << 8);
    }
    return crc;
}

// --- Global Variables ---
static hid_device *g_mcu_dev = NULL;
static hid_device *g_imu_dev = NULL;

static char *mcu_hid_path = NULL;
static char *imu_hid_path = NULL;

// MCU command handling
static pthread_mutex_t lock_cmd;
static pthread_cond_t signal_cond_cmd;
static uchar g_mcu_rsp[0x100]; // Buffer for MCU responses (size from mcu_thread)

// Threads
static pthread_t mcu_read_tid;
static pthread_t imu_read_tid;
static volatile bool mcu_thread_flag = false;
static volatile bool imu_thread_flag = false;

// Barriers for thread startup synchronization
static pthread_barrier_t barrier_mcu;
static pthread_barrier_t barrier_imu;

// Callbacks
typedef void (*viture_mcu_event_callback_t)(ushort event_id, uchar *data, ushort len, uint timestamp);
static viture_mcu_event_callback_t ext_mcu_event_callback = NULL;

// IMU Data callback
#include "viture_connection.h" // For viture_imu_data_callback_t
static viture_imu_data_callback_t ext_imu_data_callback = NULL;


static void native_imu_deinit(void);
static void native_mcu_deinit(void);

// --- HID Device Handling ---
static char* find_hid_device_path(int vid, int pid, int interface_number) {
    struct hid_device_info *devs, *cur_dev;
    char *path = NULL;

    devs = hid_enumerate(vid, pid);
    cur_dev = devs;
    while (cur_dev) {
        if (cur_dev->interface_number == interface_number) {
            path = strdup(cur_dev->path);
            break;
        }
        cur_dev = cur_dev->next;
    }
    hid_free_enumeration(devs);
    return path;
}

static hid_device* open_hid_interface(const char* path) {
    if (!path) return NULL;
    hid_device* dev = hid_open_path(path);
    if (!dev) {
        fprintf(stderr, "Failed to open HID device path: %s\n", path);
        const wchar_t *err = hid_error(NULL); // Add const
        if (err) fprintf(stderr, "HID Error: %ls\n", err);
    }
    return dev;
}


// --- Command Building and Parsing ---

static void cmd_build(ushort cmd_id, uchar *data, ushort data_len, uchar *out_buf, ushort *out_total_len) {
    memset(out_buf, 0, 0x40); // Max packet size is 64 bytes for HID

    out_buf[0] = 0xFF;
    out_buf[1] = 0xFE;
    // Bytes 2,3 are CRC - filled later
    // Bytes 4,5 are packet_len - filled later

    // Bytes 6-13 (8 bytes) are some header, often zeroed or fixed
    // memset(out_buf + 6, 0, 8); // Already done by global memset

    *(ushort *)(out_buf + 0xe) = cmd_id; // Command ID
    // Bytes 0x10, 0x11 (2 bytes) also part of header, often zeroed
    // memset(out_buf + 0x10, 0, 2); // Already done

    ushort payload_len_field = 0x0c; // Base length: 8 (zeros) + 2 (cmd_id) + 2 (zeros)
    if (data != NULL && data_len > 0) {
        if (0x12 + data_len > 0x40) {
            fprintf(stderr, "cmd_build: data_len %d too large for 64-byte packet\n", data_len);
            // Truncate or handle error, for now, let it be, but this is an issue.
            // Max data_len is 64 - 18 = 46 bytes.
        }
        memcpy(out_buf + 0x12, data, data_len);
        payload_len_field += data_len;
    }
    
    *(ushort *)(out_buf + 4) = payload_len_field; // Packet length (from this field onwards, excluding CRC and FF FE)

    ushort crc = cmd_crc(out_buf + 4, payload_len_field + 2); // CRC over length field and everything after it
    *(ushort *)(out_buf + 2) = crc;

    *out_total_len = 0x40; // Always send 64 bytes for HID report
}

static void parse_rsp(uchar *rsp_buf, ushort total_rsp_len, uchar *out_data, ushort *out_data_len, ushort *out_cmd_id) {
    if (total_rsp_len == 0) {
        fprintf(stderr, "parse_rsp: invalid response (length 0)\n");
        *out_data_len = 0;
        *out_cmd_id = 0xFFFF; // Indicate error
        return;
    }

    // Assuming rsp_buf points to the start of the 64-byte HID report
    // FF FE (header)
    // CRC (actual_crc_val)
    // Length (payload_len_field)
    // ...
    // CmdID
    // ...
    // Data

    ushort actual_crc_val = *(ushort *)(rsp_buf + 2);
    ushort payload_len_field = *(ushort *)(rsp_buf + 4); // Length from this field onwards
    *out_cmd_id = *(ushort *)(rsp_buf + 0xe);

    if (payload_len_field < 0x0c) { // Minimum length: 8 (zeros) + 2 (cmd_id) + 2 (zeros)
        fprintf(stderr, "parse_rsp: payload_len_field %d too small\n", payload_len_field);
        *out_data_len = 0;
        return;
    }
    
    // Check CRC
    ushort calculated_crc = cmd_crc(rsp_buf + 4, payload_len_field + 2);
    if (calculated_crc != actual_crc_val) {
        fprintf(stderr, "parse_rsp: CRC mismatch. Expected %04X, Got %04X for CmdID %04X\n",
                calculated_crc, actual_crc_val, *out_cmd_id);
        // Continue parsing for debugging, but data might be corrupt
    }

    *out_data_len = payload_len_field - 0x0c; // Subtract header part from payload_len_field
    if (*out_data_len > 0) {
         // Max data_len is 64 - 18 = 46. Max *out_data_len is 46.
        if (0x12 + *out_data_len > total_rsp_len || 0x12 + *out_data_len > 0x40) {
             fprintf(stderr, "parse_rsp: out_data_len %d inconsistent with total_rsp_len %d or packet size\n", *out_data_len, total_rsp_len);
             *out_data_len = 0; // or clamp
             return;
        }
        memcpy(out_data, rsp_buf + 0x12, *out_data_len);
    } else {
        *out_data_len = 0; // Ensure it's zero if no data
    }
}

// --- Command Synchronization ---
static int cmd_wait(int timeout_sec) {
    struct timespec ts;
    struct timeval tv;
    int ret;

    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + timeout_sec;
    ts.tv_nsec = tv.tv_usec * 1000;

    pthread_mutex_lock(&lock_cmd);
    ret = pthread_cond_timedwait(&signal_cond_cmd, &lock_cmd, &ts);
    pthread_mutex_unlock(&lock_cmd);

    return ret; // 0 if signaled, ETIMEDOUT if timeout
}

static void cmd_release(void) {
    pthread_mutex_lock(&lock_cmd);
    pthread_cond_signal(&signal_cond_cmd);
    pthread_mutex_unlock(&lock_cmd);
}


// --- Thread Functions ---
static void event_update(ushort event_id, uchar *data, ushort len, uint timestamp) {
    // This function is called from mcu_thread for asynchronous events
    // fprintf(stderr, "MCU Event: ID=0x%04X, Len=%d, TS=%u\n", event_id, len, timestamp);
    if (ext_mcu_event_callback) {
        ext_mcu_event_callback(event_id, data, len, timestamp);
    }
}

static void imu_update(uchar *data, ushort len, uint timestamp) {
    // This function is called from imu_thread
    if (ext_imu_data_callback) {
        ext_imu_data_callback(data, len, timestamp);
    }
}


static void* mcu_thread(void *arg) {
    // uchar read_buf[0x100]; // Unused
    // int bytes_read_total = 0; // Unused

    (void)arg; // Unused

    pthread_barrier_wait(&barrier_mcu);
    fprintf(stderr, "MCU thread started\n");

    while (mcu_thread_flag) {
        uchar hid_packet[0x40]; // Standard HID packet size
        int res = hid_read_timeout(g_mcu_dev, hid_packet, sizeof(hid_packet), 1000); // 1 sec timeout

        if (res < 0) {
            fprintf(stderr, "MCU HID read error. Stopping thread.\n");
            const wchar_t *err = hid_error(g_mcu_dev); // Add const
            if (err) fprintf(stderr, "HID Error: %ls\n", err);
            mcu_thread_flag = false; // Signal to stop
            break;
        }
        if (res == 0) { // Timeout
            continue;
        }

        // Process the received packet (res bytes in hid_packet)
        // Decompiled logic:
        // if (hid_packet[0] == 0xFF && hid_packet[1] == 0xFE && res >= 0x40) {
        //    ushort cmd_id_in_hdr = *(ushort*)(hid_packet + 0xE);
        //    if (cmd_id_in_hdr == 0) { // This means it's a response to a command
        //        memcpy(g_mcu_rsp, hid_packet, sizeof(hid_packet));
        //        cmd_release();
        //    } else { // This means it's an event
        //        uchar event_data[0x40];
        //        ushort event_data_len, event_id_parsed;
        //        uint timestamp = *(uint*)(hid_packet + 6); // Example, actual timestamp location unknown
        //        parse_rsp(hid_packet, res, event_data, &event_data_len, &event_id_parsed);
        //        event_update(event_id_parsed, event_data, event_data_len, timestamp);
        //    }
        // }
        // Simplified logic based on decompiled mcu_thread:
        // It checks header 0xFF 0xFE, then checks byte at offset 0x7 of the payload length field (hid_packet[4+7]).
        // If hid_packet[4+7] (effectively hid_packet[11]) is 0, it's a command response. Otherwise, an event.
        // This is specific and needs careful mapping.
        // The `local_110._7_1_` in decompiled code refers to `hid_packet[4+7]` if `local_110` is `hid_packet+4`.
        // Let's use the `parse_rsp` cmd_id to differentiate. If cmd_id from `parse_rsp` is the one we are waiting for, it's a response.
        // However, `parse_rsp` itself returns the cmd_id. Events also have cmd_ids.
        // The original SDK seems to use `cmd_id == 0` in the header to mark responses for `cmd_exec`.
        // Let's assume `hid_packet[0xE]` (CmdID field) being 0 indicates a direct response to `cmd_exec`.
        // All other non-zero CmdIDs are events. This is a common pattern.

        if (hid_packet[0] == 0xFF && hid_packet[1] == 0xFE) {
            ushort parsed_cmd_id;
            uchar parsed_data[0x40]; // Max possible data
            ushort parsed_data_len;

            // The timestamp is part of the packet structure, typically in the 8-byte zeroed region.
            // Decompiled code: local_15c = local_110._2_4_; -> if local_110 is hid_packet+4, then this is hid_packet[6,7,8,9]
            uint timestamp_from_packet = 0;
            memcpy(&timestamp_from_packet, hid_packet + 6, sizeof(uint));


            // If the command ID field in the raw packet (offset 0xE) is 0, it's a synchronous response.
            // Otherwise, it's an asynchronous event.
            ushort raw_cmd_id_in_header = *(ushort*)(hid_packet + 0xE);

            if (raw_cmd_id_in_header == 0) { // Synchronous response for cmd_exec
                size_t copy_len = (res > 0 && (size_t)res < sizeof(g_mcu_rsp)) ? (size_t)res : sizeof(g_mcu_rsp);
                memcpy(g_mcu_rsp, hid_packet, copy_len);
                cmd_release();
            } else { // Asynchronous event
                parse_rsp(hid_packet, res, parsed_data, &parsed_data_len, &parsed_cmd_id);
                 if (parsed_cmd_id != 0xFFFF) { // Check if parse_rsp had an error
                    event_update(parsed_cmd_id, parsed_data, parsed_data_len, timestamp_from_packet);
                }
            }
        } else {
            fprintf(stderr, "MCU Read: Invalid packet header\n");
        }
    }
    fprintf(stderr, "MCU thread stopped\n");
    return NULL;
}

static void* imu_thread(void *arg) {
    (void)arg; // Unused

    pthread_barrier_wait(&barrier_imu);
    fprintf(stderr, "IMU thread started\n");

    while (imu_thread_flag) {
        uchar hid_packet[0x40]; // Standard HID packet size
        int res = hid_read_timeout(g_imu_dev, hid_packet, sizeof(hid_packet), 1000); // 1 sec timeout

        if (res < 0) {
            fprintf(stderr, "IMU HID read error. Stopping thread.\n");
            const wchar_t *err = hid_error(g_imu_dev); // Add const
            if (err) fprintf(stderr, "HID Error: %ls\n", err);
            imu_thread_flag = false; // Signal to stop
            break;
        }
        if (res == 0) { // Timeout
            continue;
        }
        
        // Process IMU packet
        // Decompiled imu_thread uses parse_rsp.
        if (hid_packet[0] == 0xFF && hid_packet[1] == 0xFC) { // IMU packets start with FF FC
            uchar imu_data_payload[0x40];
            ushort imu_data_len, imu_cmd_id; // cmd_id for IMU packets might be fixed (e.g. IMU data event)
            
            uint timestamp_from_packet = 0;
            memcpy(&timestamp_from_packet, hid_packet + 6, sizeof(uint));

            parse_rsp(hid_packet, res, imu_data_payload, &imu_data_len, &imu_cmd_id);
            if (imu_cmd_id != 0xFFFF) { // Check if parse_rsp had an error
                 // The cmd_id for IMU data is typically a fixed value indicating IMU report.
                 // e.g. if (imu_cmd_id == EXPECTED_IMU_DATA_CMD_ID)
                imu_update(imu_data_payload, imu_data_len, timestamp_from_packet);
            }
        } else {
             fprintf(stderr, "IMU Read: Invalid packet header %02X %02X (expected FF FC)\n", hid_packet[0], hid_packet[1]);
        }
    }
    fprintf(stderr, "IMU thread stopped\n");
    return NULL;
}

// --- Core Command Execution ---
// This function sends a command and waits for a response via g_mcu_rsp, filled by mcu_thread.
// Returns status code from response payload (byte 0).
static uint cmd_exec(hid_device *dev, ushort cmd_id, uchar *data, ushort data_len, uchar **rsp_data, ushort *rsp_data_len) {
    if (dev == NULL) {
        fprintf(stderr, "cmd_exec: device is null for cmd 0x%04X\n", cmd_id);
        return 0xFFFFFFFD; // Error code like in decompiled SDK
    }

    uchar cmd_buf[0x40];
    ushort cmd_total_len;
    cmd_build(cmd_id, data, data_len, cmd_buf, &cmd_total_len);

    // Critical: For cmd_exec, the cmd_id in the header (offset 0xE) should be 0 for the response.
    // The actual command being executed is identified by the cmd_id parameter.
    // The request packet sent to device should contain the actual cmd_id.
    // The mcu_thread logic expects response packets for cmd_exec to have cmd_id 0 in their header.
    // This means the device itself must set cmd_id to 0 in its response packet header for these.
    // This is a bit confusing. Let's assume cmd_build puts the correct cmd_id.
    // And mcu_thread's logic for distinguishing sync/async is correct.

    int bytes_written = hid_write(dev, cmd_buf, cmd_total_len);
    if (bytes_written < 0 || (ushort)bytes_written != cmd_total_len) { // Check for error (-1) and partial write
        fprintf(stderr, "cmd_exec: HID write failed for cmd 0x%04X. Wrote %d, expected %d\n", cmd_id, bytes_written, cmd_total_len);
        const wchar_t *err = hid_error(dev); // Add const
        if (err) fprintf(stderr, "HID Error: %ls\n", err);
        return 0xFFFFFFFF; // Error code
    }

    if (cmd_wait(2) == ETIMEDOUT) { // 2 second timeout
        fprintf(stderr, "cmd_exec: Timeout waiting for response for cmd 0x%04X\n", cmd_id);
        // Check g_mcu_rsp anyway, as per decompiled code
        // This part is tricky, as g_mcu_rsp might contain stale data or data for a different command
        // For now, let's assume timeout means failure.
        return 0xFFFFFFFE; // Error code for timeout
    }

    // Response is now in g_mcu_rsp
    uchar parsed_rsp_payload[0x40];
    ushort parsed_rsp_payload_len;
    ushort parsed_cmd_id; // This should be 0 if mcu_thread logic is right for sync responses

    parse_rsp(g_mcu_rsp, sizeof(g_mcu_rsp), parsed_rsp_payload, &parsed_rsp_payload_len, &parsed_cmd_id);

    // The decompiled cmd_exec checks if the *original* cmd_id matches the cmd_id in the response *payload*.
    // This is not standard. parse_rsp gets cmd_id from header (offset 0xE).
    // The decompiled code's parse_rsp gets cmd_id from offset 0xE.
    // Then cmd_exec checks `if (param_2 == local_e8)`, where param_2 is original cmd_id, local_e8 is parsed_cmd_id.
    // This implies that responses to commands also carry the original command's ID in their header.
    // This contradicts the mcu_thread logic that cmd_id 0 in header means sync response.
    // Let's stick to: mcu_thread puts response in g_mcu_rsp if header cmd_id is 0.
    // Then parse_rsp will report cmd_id as 0.
    // The actual status of the command (e.g. success/failure of *set_imu*) is in the payload.

    if (parsed_cmd_id != 0) { // Should be 0 for a synchronous response handled by cmd_release path
         fprintf(stderr, "cmd_exec: Response cmd_id 0x%04X not 0 as expected for sync response to cmd 0x%04X\n", parsed_cmd_id, cmd_id);
         // This might indicate an issue with mcu_thread logic or device behavior.
    }


    if (rsp_data != NULL && rsp_data_len != NULL) {
        if (parsed_rsp_payload_len > 1) { // Assuming byte 0 is status
            *rsp_data_len = parsed_rsp_payload_len - 1;
            *rsp_data = (uchar *)calloc(1, *rsp_data_len);
            if (*rsp_data) {
                memcpy(*rsp_data, parsed_rsp_payload + 1, *rsp_data_len);
            } else {
                *rsp_data_len = 0;
                 fprintf(stderr, "cmd_exec: Failed to allocate memory for response data\n");
            }
        } else {
            *rsp_data_len = 0;
            *rsp_data = NULL;
        }
    }
    
    if (parsed_rsp_payload_len > 0) {
        return (uint)parsed_rsp_payload[0]; // Return status code from first byte of payload
    }
    return 0xFFFFFFFE; // Error if no payload
}


// --- Start/Stop Threads ---
static bool startReadMcu(void) {
    if (g_mcu_dev == NULL) return false;
    if (mcu_thread_flag) return true; // Already running

    pthread_barrier_init(&barrier_mcu, NULL, 2);
    mcu_thread_flag = true;
    if (pthread_create(&mcu_read_tid, NULL, mcu_thread, NULL) != 0) {
        fprintf(stderr, "Error creating MCU monitor thread.\n");
        mcu_thread_flag = false;
        return false;
    }
    pthread_barrier_wait(&barrier_mcu); // Wait for thread to start
    fprintf(stderr, "MCU monitor thread created successfully.\n");
    return true;
}

static void stopReadMcu(void) {
    if (mcu_thread_flag) {
        mcu_thread_flag = false;
        if (mcu_read_tid != 0) { // Check if thread was actually created
             pthread_join(mcu_read_tid, NULL);
        }
        mcu_read_tid = 0; // Reset thread ID
        fprintf(stderr, "MCU Read thread stopped.\n");
    }
    pthread_barrier_destroy(&barrier_mcu);
}

static bool startReadImu(void) {
    if (g_imu_dev == NULL) return false;
    if (imu_thread_flag) return true;

    pthread_barrier_init(&barrier_imu, NULL, 2);
    imu_thread_flag = true;
    if (pthread_create(&imu_read_tid, NULL, imu_thread, NULL) != 0) {
        fprintf(stderr, "Error creating IMU monitor thread.\n");
        imu_thread_flag = false;
        return false;
    }
    pthread_barrier_wait(&barrier_imu);
    fprintf(stderr, "IMU monitor thread created successfully.\n");
    return true;
}

static void stopReadImu(void) {
    if (imu_thread_flag) {
        imu_thread_flag = false;
        if (imu_read_tid != 0) {
            pthread_join(imu_read_tid, NULL);
        }
        imu_read_tid = 0;
        fprintf(stderr, "IMU Read thread stopped.\n");
    }
    pthread_barrier_destroy(&barrier_imu);
}

// --- Native Init/Deinit ---
bool native_mcu_init(void) {
    if (g_mcu_dev) return true; // Already initialized

    g_mcu_dev = open_hid_interface(mcu_hid_path);
    if (g_mcu_dev == NULL) {
        fprintf(stderr, "native_mcu_init: Failed to open MCU HID device.\n");
        return false;
    }

    pthread_mutex_init(&lock_cmd, NULL);
    pthread_cond_init(&signal_cond_cmd, NULL);

    if (!startReadMcu()) {
        fprintf(stderr, "native_mcu_init: Failed to start MCU read thread.\n");
        hid_close(g_mcu_dev);
        g_mcu_dev = NULL;
        pthread_mutex_destroy(&lock_cmd);
        pthread_cond_destroy(&signal_cond_cmd);
        return false;
    }
    fprintf(stderr, "Native MCU initialized.\n");
    return true;
}

void native_mcu_deinit(void) {
    if (g_mcu_dev != NULL) {
        stopReadMcu();
        hid_close(g_mcu_dev);
        g_mcu_dev = NULL;
        pthread_mutex_destroy(&lock_cmd);
        pthread_cond_destroy(&signal_cond_cmd);
        fprintf(stderr, "Native MCU deinitialized.\n");
    }
}

bool native_imu_init(void) {
    if (g_imu_dev) return true;

    g_imu_dev = open_hid_interface(imu_hid_path);
    if (g_imu_dev == NULL) {
        fprintf(stderr, "native_imu_init: Failed to open IMU HID device.\n");
        return false;
    }

    if (!startReadImu()) {
        fprintf(stderr, "native_imu_init: Failed to start IMU read thread.\n");
        hid_close(g_imu_dev);
        g_imu_dev = NULL;
        return false;
    }
    fprintf(stderr, "Native IMU initialized.\n");
    return true;
}

void native_imu_deinit(void) {
    if (g_imu_dev != NULL) {
        stopReadImu();
        hid_close(g_imu_dev);
        g_imu_dev = NULL;
        fprintf(stderr, "Native IMU deinitialized.\n");
    }
}

// --- Public API ---
// Sends a command with a single byte of data.
uint native_mcu_exec(ushort cmd_id, uchar data_byte) {
    return cmd_exec(g_mcu_dev, cmd_id, &data_byte, 1, NULL, NULL);
}

// Command ID for set_imu is 0x15 from decompiled SDK
// Data: 0 for off, 1 for on.
uint set_imu(bool enable) {
    if (!g_mcu_dev) {
        fprintf(stderr, "set_imu: MCU not initialized.\n");
        return 0xFFFFFFFD;
    }
    fprintf(stderr, "Setting IMU to: %s\n", enable ? "ON" : "OFF");
    uint result = native_mcu_exec(0x15, enable ? 1 : 0);
    if (result == 0) { // Assuming 0 means success from command payload
        fprintf(stderr, "Set IMU %s successful.\n", enable ? "ON" : "OFF");
    } else {
        fprintf(stderr, "Set IMU %s failed with code %u.\n", enable ? "ON" : "OFF", result);
    }
    return result;
}

void viture_set_mcu_event_callback(viture_mcu_event_callback_t callback) {
    ext_mcu_event_callback = callback;
}

void viture_set_imu_data_callback(viture_imu_data_callback_t callback) {
    ext_imu_data_callback = callback;
}

// Main Init/Deinit for the driver
bool viture_driver_init(void) {
    if (hid_init() != 0) {
        fprintf(stderr, "Failed to initialize HIDAPI.\n");
        return false;
    }
    init_crc_table();

    // Find HID device paths
    // Free these paths in viture_driver_close
    if (mcu_hid_path) free(mcu_hid_path);
    if (imu_hid_path) free(imu_hid_path);
    mcu_hid_path = find_hid_device_path(VITURE_VENDOR_ID, 0, MCU_INTERFACE_NUMBER); // PID 0 to match any
    imu_hid_path = find_hid_device_path(VITURE_VENDOR_ID, 0, IMU_INTERFACE_NUMBER);

    if (!mcu_hid_path) {
        fprintf(stderr, "MCU HID device (VID: %04X, Interface: %d) not found.\n", VITURE_VENDOR_ID, MCU_INTERFACE_NUMBER);
        hid_exit();
        return false;
    }
     fprintf(stderr, "Found MCU HID device path: %s\n", mcu_hid_path);

    if (!imu_hid_path) {
        fprintf(stderr, "IMU HID device (VID: %04X, Interface: %d) not found.\n", VITURE_VENDOR_ID, IMU_INTERFACE_NUMBER);
        if (mcu_hid_path) free(mcu_hid_path);
        mcu_hid_path = NULL;
        hid_exit();
        return false;
    }
    fprintf(stderr, "Found IMU HID device path: %s\n", imu_hid_path);


    if (!native_mcu_init()) {
        fprintf(stderr, "Failed to initialize native MCU.\n");
        if (mcu_hid_path) free(mcu_hid_path);
        if (imu_hid_path) free(imu_hid_path);
        mcu_hid_path = NULL; imu_hid_path = NULL;
        hid_exit();
        return false;
    }
    if (!native_imu_init()) {
        fprintf(stderr, "Failed to initialize native IMU.\n");
        native_mcu_deinit(); // Clean up MCU if IMU fails
        if (mcu_hid_path) free(mcu_hid_path);
        if (imu_hid_path) free(imu_hid_path);
        mcu_hid_path = NULL; imu_hid_path = NULL;
        hid_exit();
        return false;
    }

    fprintf(stderr, "Viture driver initialized successfully.\n");
    return true;
}

void viture_driver_close(void) {
    native_imu_deinit();
    native_mcu_deinit();

    if (mcu_hid_path) {
        free(mcu_hid_path);
        mcu_hid_path = NULL;
    }
    if (imu_hid_path) {
        free(imu_hid_path);
        imu_hid_path = NULL;
    }

    hid_exit();
    fprintf(stderr, "Viture driver closed.\n");
}

