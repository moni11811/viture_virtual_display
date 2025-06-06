#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

// Use GL/glut.h on macOS, GL/freeglut.h on Linux
#include <GL/freeglut.h> 

#include "3rdparty/include/viture.h"

// --- V4L2 and Frame Configuration ---
#define DEVICE_PATH      "/dev/video0"
#define FRAME_WIDTH      1920
#define FRAME_HEIGHT     1080
#define PIXEL_FORMAT     V4L2_PIX_FMT_NV24
#define BUFFER_COUNT     4

// --- Global variables for V4L2 ---
struct buffer {
    void   *start;
    size_t length;
};

static int fd = -1;
static struct buffer *buffers = NULL;
static unsigned int n_buffers = 0;

// --- Global variables for OpenGL ---
static GLuint texture_id;
static unsigned char *rgb_frame = NULL; 
static int fullscreen_mode = 0; 
static int display_test_pattern = 0; 
static float g_plane_orbit_distance = 1.0f; 

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
void convert_nv24_to_rgb(const unsigned char *nv24, unsigned char *rgb, int width, int height) {
    const unsigned char *y_plane = nv24;
    const unsigned char *uv_plane = nv24 + (width * height);

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
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "Device does not support video capture\n"); exit(EXIT_FAILURE);
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support streaming\n"); exit(EXIT_FAILURE);
    }
    printf("V4L2: Device supports capture and streaming.\n");

    // 3. Set format
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = FRAME_WIDTH;
    fmt.fmt.pix.height      = FRAME_HEIGHT;
    fmt.fmt.pix.pixelformat = PIXEL_FORMAT;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT failed. Device may not support 1920x1080 NV24");
        exit(EXIT_FAILURE);
    }
    printf("V4L2: Format set to %dx%d NV24\n", FRAME_WIDTH, FRAME_HEIGHT);

    // 4. Request buffers
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("VIDIOC_REQBUFS"); exit(EXIT_FAILURE); }
    n_buffers = req.count;
    printf("V4L2: %d buffers requested.\n", n_buffers);

    // 5. Map buffers
    buffers = calloc(n_buffers, sizeof(*buffers));
    for (unsigned int i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("VIDIOC_QUERYBUF"); exit(EXIT_FAILURE); }
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) { perror("mmap"); exit(EXIT_FAILURE); }
    }
    printf("V4L2: Buffers mapped.\n");

    // 6. Queue buffers
    for (unsigned int i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("VIDIOC_QBUF"); exit(EXIT_FAILURE); }
    }
    printf("V4L2: Buffers queued.\n");

    // 7. Start streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("VIDIOC_STREAMON"); exit(EXIT_FAILURE); }
    printf("V4L2: Streaming started.\n");
}

// --- OpenGL/GLUT Functions ---

void cleanup() {
    printf("Cleaning up...\n");
    if (use_viture_imu) {
        printf("Viture: Disabling IMU and de-initializing...\n");
        set_imu(false);
        deinit();
    }
    if (fd != -1) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        for (unsigned int i = 0; i < n_buffers; ++i) {
            munmap(buffers[i].start, buffers[i].length);
        }
        close(fd);
    }
    if (buffers) free(buffers);
    if (rgb_frame) free(rgb_frame);
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

    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FRAME_WIDTH, FRAME_HEIGHT, GL_RGB, GL_UNSIGNED_BYTE, rgb_frame);

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
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

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
    if (display_test_pattern) {
        fill_frame_with_pattern(rgb_frame, FRAME_WIDTH, FRAME_HEIGHT);
    } else {
        convert_nv24_to_rgb(buffers[buf.index].start, rgb_frame, FRAME_WIDTH, FRAME_HEIGHT);
    }

    // Re-queue the buffer
    if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
        perror("VIDIOC_QBUF");
        exit(EXIT_FAILURE);
    }

    // Tell GLUT to redraw the window
    glutPostRedisplay();
}

void idle() {
    capture_and_update();
}

void init_gl() {
    rgb_frame = malloc(FRAME_WIDTH * FRAME_HEIGHT * 3); // 3 bytes for R, G, B
    if (!rgb_frame) {
        fprintf(stderr, "Failed to allocate memory for RGB frame\n");
        exit(EXIT_FAILURE);
    }
    // Initialize with a black screen
    memset(rgb_frame, 0, FRAME_WIDTH * FRAME_HEIGHT * 3);

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
                 GL_RGB, GL_UNSIGNED_BYTE, rgb_frame);
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
        }
    }
    
    printf("Starting V4L2-OpenGL real-time viewer...\n");

    if (use_viture_imu) {
        printf("Viture: Initializing with IMU callback...\n");
        init(viture_imu_callback, viture_mcu_callback);
        set_imu(true); // Start the IMU data stream
        printf("Viture: IMU stream enabled.\n");
    }

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
