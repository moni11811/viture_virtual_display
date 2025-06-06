#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h> // NEW: For mutex

#include <linux/videodev2.h>

#include <stdbool.h>


// Use GL/glut.h on macOS, GL/freeglut.h on Linux
#include <GL/freeglut.h> 

#ifdef USE_VITURE
#include "3rdparty/include/viture.h"
#endif

// --- V4L2 and Frame Configuration ---
#define DEVICE_PATH      "/dev/video0"
#define FRAME_WIDTH      1920
#define FRAME_HEIGHT     1080
#define PIXEL_FORMAT     V4L2_PIX_FMT_NV24
#define BUFFER_COUNT     4

// --- Global variables for V4L2 ---
// NEW: Structures for multi-planar buffer handling
struct plane_info {
    void   *start;
    size_t length;
};

struct mplane_buffer {
    struct plane_info planes[VIDEO_MAX_PLANES];
    unsigned int num_planes_in_buffer; // Actual number of planes for this buffer type
};

static int fd = -1;
// static struct buffer *buffers = NULL; // OLD
static struct mplane_buffer *buffers_mp = NULL; // NEW for MPLANE
static unsigned int n_buffers = 0;
static unsigned int num_planes_per_buffer = 0; // Expected number of planes (e.g., 2 for NV24)


// --- Global variables for OpenGL ---
static GLuint texture_id;
// static unsigned char *rgb_frame = NULL; // OLD single buffer
static unsigned char *rgb_frames[2] = {NULL, NULL}; // NEW: Double buffers for RGB frame
static int front_buffer_idx = 0;                   // NEW: Index of the buffer currently displayed
static int back_buffer_idx = 1;                    // NEW: Index of the buffer being written to
static volatile bool new_frame_captured = false;   // NEW: Flag to indicate a new frame is in back buffer
static pthread_mutex_t frame_mutex;                // NEW: Mutex for synchronizing buffer access

static bool glut_initialized = false;

static int fullscreen_mode = 0; 
static int display_test_pattern = 0;                // For -tp flag (ensure it's here)
static float g_plane_orbit_distance = 1.0f;         // For -pd flag, default to 1.0
static float g_plane_scale = 1.0f;                  // NEW: For scaling the plane

// --- Helper Functions ---

static bool use_viture_imu = false;
// Use volatile to prevent compiler from optimizing away reads, as these are
// updated by a different thread (the Viture callback thread).
static volatile float viture_roll = 0.0f;
static volatile float viture_pitch = 0.0f;
static volatile float viture_yaw = 0.0f;
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

static void viture_imu_callback(uint8_t *data, uint16_t len, uint32_t ts) {
    // We only need the first 12 bytes for Euler angles
    if (len < 12) return;

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

    // NEW: Signal GLUT to redraw the scene after IMU update
    if ( glut_initialized ) glutPostRedisplay();
}

static void viture_mcu_callback(uint16_t msgid, uint8_t *data, uint16_t len, uint32_t ts)
{
    // Dummy
}

static inline unsigned char clamp(int val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (unsigned char)val;
}

// Convert a single NV24 frame to RGB24
// This is a performance-critical function. For higher performance,
// consider using SIMD instructions or a GPU shader (GLSL) for conversion.
// MODIFIED: Signature for separate Y and UV planes
void convert_nv24_to_rgb(const unsigned char *y_plane_data, const unsigned char *uv_plane_data, unsigned char *rgb, int width, int height) {
    const unsigned char *y_plane = y_plane_data;
    const unsigned char *uv_plane = uv_plane_data;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int i = y * width + x;
            int uv_i = i * 2;
            
            int y_val = y_plane[i];
            int cb_val = uv_plane[uv_i];
            int cr_val = uv_plane[uv_i + 1];

            // YCbCr to RGB conversion (standard formula)
            int c = y_val - 16;
            int d = cb_val - 128;
            int e = cr_val - 128;

            int r = clamp((298 * c + 409 * e + 128) >> 8);
            int g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
            int b = clamp((298 * c + 516 * d + 128) >> 8);
            
            rgb[i * 3] = r;
            rgb[i * 3 + 1] = g;
            rgb[i * 3 + 2] = b;
        }
    }
}

void fill_frame_with_pattern( unsigned char *rgb, int width, int height ) {
    // Fill the RGB frame with a simple color pattern for testing
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int i = (y * width + x) * 3;
            rgb[i] = (x + y) % 256;       // R
            rgb[i + 1] = (x * 2 + y) % 256; // G
            rgb[i + 2] = (x * 3 + y) % 256; // B
        }
    }
}



// --- V4L2 Initialization ---
void init_v4l2() {
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    
    // 1. Open device
    fd = open(DEVICE_PATH, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) { perror("Cannot open device"); exit(EXIT_FAILURE); }

    // 2. Query capabilities
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("VIDIOC_QUERYCAP"); exit(EXIT_FAILURE); }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) { 
        fprintf(stderr, "Device does not support video capture\n"); exit(EXIT_FAILURE);
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support streaming\n"); exit(EXIT_FAILURE);
    }
    printf("V4L2: Device supports capture and streaming.\n");

    // 3. Set format
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // MODIFIED for MPLANE
    fmt.fmt.pix_mp.width       = FRAME_WIDTH;
    fmt.fmt.pix_mp.height      = FRAME_HEIGHT;
    fmt.fmt.pix_mp.pixelformat = PIXEL_FORMAT; // e.g., V4L2_PIX_FMT_NV24
    fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    // num_planes is often set by the driver, or we can specify if known (e.g., 2 for NV24)
    // For NV24, plane 0 is Y, plane 1 is UV.
    fmt.fmt.pix_mp.num_planes = 2; // Explicitly for NV24

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT failed. Device may not support 1920x1080 NV24 MPLANE");
        exit(EXIT_FAILURE);
    }
    // Store the actual number of planes confirmed by the driver
    num_planes_per_buffer = fmt.fmt.pix_mp.num_planes;
    if (num_planes_per_buffer == 0 || num_planes_per_buffer > VIDEO_MAX_PLANES) {
        fprintf(stderr, "V4L2: Invalid number of planes: %u\n", num_planes_per_buffer);
        exit(EXIT_FAILURE);
    }
    printf("V4L2: Format set to %dx%d, pixelformat %c%c%c%c, %u planes (MPLANE)\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           (fmt.fmt.pix_mp.pixelformat)&0xFF, (fmt.fmt.pix_mp.pixelformat>>8)&0xFF,
           (fmt.fmt.pix_mp.pixelformat>>16)&0xFF, (fmt.fmt.pix_mp.pixelformat>>24)&0xFF,
           num_planes_per_buffer);


    // 4. Request buffers
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // MODIFIED for MPLANE
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("VIDIOC_REQBUFS"); exit(EXIT_FAILURE); }
    n_buffers = req.count;
    printf("V4L2: %d buffers requested.\n", n_buffers);

    // 5. Map buffers
    buffers_mp = calloc(n_buffers, sizeof(*buffers_mp)); // MODIFIED for MPLANE
    for (unsigned int i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes_query[VIDEO_MAX_PLANES]; // Array for plane info
        memset(&buf, 0, sizeof(buf));
        memset(planes_query, 0, sizeof(planes_query)); // Clear plane structures

        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // MODIFIED for MPLANE
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        buf.m.planes = planes_query; // Point to our plane array
        buf.length = num_planes_per_buffer; // Number of planes to query

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("VIDIOC_QUERYBUF"); exit(EXIT_FAILURE); }

        buffers_mp[i].num_planes_in_buffer = num_planes_per_buffer; // Store how many planes this buffer uses
        for (unsigned int p = 0; p < num_planes_per_buffer; ++p) {
            buffers_mp[i].planes[p].length = buf.m.planes[p].length;
            buffers_mp[i].planes[p].start = mmap(NULL, buf.m.planes[p].length,
                                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                                 fd, buf.m.planes[p].m.mem_offset);
            if (buffers_mp[i].planes[p].start == MAP_FAILED) { perror("mmap plane"); exit(EXIT_FAILURE); }
        }
    }
    printf("V4L2: Buffers and planes mapped.\n");

    // 6. Queue buffers
    for (unsigned int i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes_q[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes_q, 0, sizeof(planes_q));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // MODIFIED for MPLANE
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes_q;
        buf.length = num_planes_per_buffer; // Number of planes

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("VIDIOC_QBUF"); exit(EXIT_FAILURE); }
    }
    printf("V4L2: Buffers queued.\n");

    // 7. Start streaming
    enum v4l2_buf_type stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // MODIFIED for MPLANE
    if (ioctl(fd, VIDIOC_STREAMON, &stream_type) < 0) { perror("VIDIOC_STREAMON"); exit(EXIT_FAILURE); }
    printf("V4L2: Streaming started.\n");
}

// --- OpenGL/GLUT Functions ---

void cleanup() {
    printf("Cleaning up...\n");
#ifdef USE_VITURE
    if (use_viture_imu) {
        printf("Viture: Disabling IMU and de-initializing...\n");
        set_imu(false);
        deinit();
    }
#endif

    if (fd != -1) {
        enum v4l2_buf_type stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // MODIFIED for MPLANE
        ioctl(fd, VIDIOC_STREAMOFF, &stream_type);
        if (buffers_mp) { // MODIFIED for MPLANE
            for (unsigned int i = 0; i < n_buffers; ++i) {
                for (unsigned int p = 0; p < buffers_mp[i].num_planes_in_buffer; ++p) {
                     if (buffers_mp[i].planes[p].start) {
                        munmap(buffers_mp[i].planes[p].start, buffers_mp[i].planes[p].length);
                     }
                }
            }
        }
        close(fd);
    }
    if (buffers_mp) free(buffers_mp); // MODIFIED for MPLANE
    // if (rgb_frame) free(rgb_frame); // OLD
    if (rgb_frames[0]) free(rgb_frames[0]); // NEW
    if (rgb_frames[1]) free(rgb_frames[1]); // NEW
    pthread_mutex_destroy(&frame_mutex); // NEW
    glDeleteTextures(1, &texture_id);
    printf("Cleanup complete.\n");
}

void display() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0); // Camera looks at origin

    // Rotations are applied first, affecting the coordinate system at the origin
    if (use_viture_imu) {
        // Apply rotation from Viture glasses IMU
        // The order of rotations (e.g., Yaw, Pitch, Roll) is important.
        // This order often feels natural for head tracking.
        glRotatef(viture_yaw - initial_yaw_offset, 0.0f, 1.0f, 0.0f);   // Yaw around Y-axis
        glRotatef(viture_pitch - initial_pitch_offset, 1.0f, 0.0f, 0.0f); // Pitch around X-axis
        glRotatef(viture_roll - initial_roll_offset, 0.0f, 0.0f, 1.0f);  // Roll around Z-axis
    } else {
        // Fallback to the original automatic rotation
        static float angle = 0.0f;
        angle += 0.2f;
        if (angle > 360.0f) angle -= 360.0f;
        glRotatef(15.0f, 1.0f, 0.0f, 0.0f); // Static tilt
        glRotatef(angle, 0.0f, 1.0f, 0.0f); // Auto-rotate
    }

    // NEW: Translate the plane along the (now rotated) Z-axis to make it orbit the origin
    glTranslatef(0.0f, 0.0f, -g_plane_orbit_distance);

    // NEW: Apply scaling to the plane
    glScalef(g_plane_scale, g_plane_scale, 1.0f);

    bool generate_texture = false;
    pthread_mutex_lock(&frame_mutex);
    if (new_frame_captured) {
        // Swap buffers
        int temp = front_buffer_idx;
        front_buffer_idx = back_buffer_idx;
        back_buffer_idx = temp;
        new_frame_captured = false;
        generate_texture = true; 
    }
    pthread_mutex_unlock(&frame_mutex);


    glBindTexture(GL_TEXTURE_2D, texture_id);
    // MODIFIED: Use the front buffer for display

    // NEW: Double buffering logic
    if ( generate_texture ) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FRAME_WIDTH, FRAME_HEIGHT, GL_RGB, GL_UNSIGNED_BYTE, rgb_frames[front_buffer_idx]);

    float aspect_ratio = (float)FRAME_WIDTH / (float)FRAME_HEIGHT;
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex3f(-aspect_ratio, -1.0f, 0.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex3f( aspect_ratio, -1.0f, 0.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex3f( aspect_ratio,  1.0f, 0.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(-aspect_ratio,  1.0f, 0.0f);
    glEnd();
    glutSwapBuffers();
}

void reshape(int w, int h) {
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)w / (double)h, 1.0, 100.0);
}

void capture_and_update() {
    struct v4l2_buffer buf;
    struct v4l2_plane planes_dq[VIDEO_MAX_PLANES]; // Array for plane info
    memset(&buf, 0, sizeof(buf));
    memset(planes_dq, 0, sizeof(planes_dq));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // MODIFIED for MPLANE
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes_dq;
    buf.length = num_planes_per_buffer; // Number of planes

    // Dequeue a buffer (non-blocking)
    if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
        if (errno == EAGAIN) {
            // No new frame available yet
            return;
        }
        perror("VIDIOC_DQBUF");
        exit(EXIT_FAILURE);
    }
    
    // Process the new frame
    // MODIFIED: Write to the back buffer
    // Access Y plane from planes[0].start and UV plane from planes[1].start for NV24
    // Ensure num_planes_per_buffer is at least 2 for NV24.
    if (display_test_pattern) {
        fill_frame_with_pattern(rgb_frames[back_buffer_idx], FRAME_WIDTH, FRAME_HEIGHT);
    } else {
        if (num_planes_per_buffer >= 2) { // Check for NV24 structure
            convert_nv24_to_rgb(
                (const unsigned char *)buffers_mp[buf.index].planes[0].start,
                (const unsigned char *)buffers_mp[buf.index].planes[1].start,
                rgb_frames[back_buffer_idx], FRAME_WIDTH, FRAME_HEIGHT);
        } else if (num_planes_per_buffer == 1) {
            //If it's a single plane the Cr and Cb are after the Y plane
            convert_nv24_to_rgb(
                (const unsigned char *)buffers_mp[buf.index].planes[0].start,
                (const unsigned char *)buffers_mp[buf.index].planes[0].start + FRAME_WIDTH * FRAME_HEIGHT,
                rgb_frames[back_buffer_idx], FRAME_WIDTH, FRAME_HEIGHT);
        } else {
            fprintf(stderr, "Error: Expected 2 planes for NV24, got %u\n", num_planes_per_buffer);
            // Optionally fill with a default color or pattern to indicate error
            fill_frame_with_pattern(rgb_frames[back_buffer_idx], FRAME_WIDTH, FRAME_HEIGHT);
        }
    }

    // NEW: Signal that a new frame is ready in the back buffer
    pthread_mutex_lock(&frame_mutex);
    new_frame_captured = true;
    pthread_mutex_unlock(&frame_mutex);

    // Re-queue the buffer
    // buf.type, buf.memory, buf.index, buf.m.planes, buf.length are already set from DQBUF
    // The driver might update bytesused in buf.m.planes[p] upon DQBUF.
    // For QBUF, these usually don't need to be reset unless changing buffer contents outside V4L2.
    if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
        perror("VIDIOC_QBUF");
        exit(EXIT_FAILURE);
    }

    // Tell GLUT to redraw the window // OLD: Removed as per request
    glutPostRedisplay(); 
}

void idle() {
    capture_and_update();
}

void init_gl() {
    // NEW: Initialize mutex
    if (pthread_mutex_init(&frame_mutex, NULL) != 0) {
        perror("Mutex init failed");
        exit(EXIT_FAILURE);
    }

    // NEW: Allocate double buffers
    rgb_frames[0] = malloc(FRAME_WIDTH * FRAME_HEIGHT * 3);
    rgb_frames[1] = malloc(FRAME_WIDTH * FRAME_HEIGHT * 3);

    if (!rgb_frames[0] || !rgb_frames[1]) {
        fprintf(stderr, "Failed to allocate memory for RGB frames\n");
        exit(EXIT_FAILURE);
    }
    // Initialize with a black screen
    memset(rgb_frames[0], 0, FRAME_WIDTH * FRAME_HEIGHT * 3);
    memset(rgb_frames[1], 0, FRAME_WIDTH * FRAME_HEIGHT * 3);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);

    // Create a texture
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Allocate texture memory on the GPU
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, FRAME_WIDTH, FRAME_HEIGHT, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, rgb_frames[front_buffer_idx]); // NEW: use front buffer

    glut_initialized = true; // Set flag to indicate GLUT is initialized
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fullscreen") == 0) {
            fullscreen_mode = 1;
            printf("Argument: Fullscreen mode enabled.\n");
        } else if (strcmp(argv[i], "--viture") == 0) {
            use_viture_imu = true;
            printf("Argument: Viture IMU enabled.\n");
        } else if (strcmp(argv[i], "-tp") == 0 || strcmp(argv[i], "--test-pattern") == 0) {
            display_test_pattern = 1;
            printf("Argument: Displaying test pattern.\n");
        } else if ((strcmp(argv[i], "-pd") == 0 || strcmp(argv[i], "--plane-distance") == 0)) {
            if (i + 1 < argc) {
                char *endptr;
                float val = strtof(argv[i+1], &endptr);
                if (endptr != argv[i+1] && *endptr == '\0') { // Check if conversion was successful
                    g_plane_orbit_distance = val;
                    i++; // Consume the value argument
                    printf("Argument: Plane orbit distance set to %f.\n", g_plane_orbit_distance);
                } else {
                    fprintf(stderr, "Error: Invalid float value for %s: %s\n", argv[i], argv[i+1]);
                }
            } else {
                fprintf(stderr, "Error: %s requires a float value.\n", argv[i]);
            }
        } else if ((strcmp(argv[i], "-ps") == 0 || strcmp(argv[i], "--plane-scale") == 0)) {
            if (i + 1 < argc) {
                char *endptr;
                float val = strtof(argv[i+1], &endptr);
                if (endptr != argv[i+1] && *endptr == '\0') { // Check if conversion was successful
                    g_plane_scale = val;
                    if (g_plane_scale <= 0.0f) { // Basic validation for scale
                        fprintf(stderr, "Warning: Plane scale should be positive. Using 1.0.\n");
                        g_plane_scale = 1.0f;
                    }
                    i++; // Consume the value argument
                    printf("Argument: Plane scale set to %f.\n", g_plane_scale);
                } else {
                    fprintf(stderr, "Error: Invalid float value for %s: %s\n", argv[i], argv[i+1]);
                }
            } else {
                fprintf(stderr, "Error: %s requires a float value.\n", argv[i]);
            }
        }
    }
    
    printf("Starting V4L2-OpenGL real-time viewer...\n");

#ifdef USE_VITURE
    if (use_viture_imu) {
        printf("Viture: Initializing with IMU callback...\n");
        init(viture_imu_callback, viture_mcu_callback);
        set_imu(true); // Start the IMU data stream
        printf("Viture: IMU stream enabled.\n");
    }
#endif

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);

    if (fullscreen_mode) {
        printf("Mode: Fullscreen\n");
        glutCreateWindow("V4L2 Real-time Display");
        glutFullScreen();
    } else {
        printf("Mode: Windowed\n");
        glutInitWindowSize(1280, 720); 
        glutCreateWindow("V4L2 Real-time Display");
    }

    init_gl();
    init_v4l2();

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutIdleFunc(idle);
    atexit(cleanup);

    printf("\n--- Starting main loop ---\n");
    glutMainLoop();
    return 0;
}
