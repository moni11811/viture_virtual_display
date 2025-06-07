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
#else
#include "viture_connection.h" // Include our own Viture connection header
#endif

// --- V4L2 and Frame Configuration ---
#define DEVICE_PATH      "/dev/video0"
#define FRAME_WIDTH      1920
#define FRAME_HEIGHT     1080
// #define PIXEL_FORMAT     V4L2_PIX_FMT_NV24 // Will be determined dynamically
#define BUFFER_COUNT     4

// --- Global variables for V4L2 ---
static enum v4l2_buf_type active_buffer_type;
static __u32 active_pixel_format;
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

// This is the viture_imu_callback specific to v4l2_gl.c, used if USE_VITURE is defined
// OR if our own viture_connection driver is used AND this callback is registered with it.
// For our own driver, default_viture_imu_data_handler in viture_connection.c is now the one
// that directly updates global viture_roll, etc.
static void app_viture_imu_data_handler(uint8_t *data, uint16_t len, uint32_t ts) {
    (void)ts; // ts might be unused for now in this specific handler
    if (len < 12) return;

    if (use_viture_imu && !initial_offsets_set) {
        initial_roll_offset = makeFloat(data);
        initial_pitch_offset = makeFloat(data + 4); 
        initial_yaw_offset = -makeFloat(data + 8);
        initial_offsets_set = true;
        printf("V4L2_GL Viture: Initial offsets captured: Roll=%f, Pitch=%f, Yaw=%f\n", initial_roll_offset, initial_pitch_offset, initial_yaw_offset);
    }

    viture_roll = makeFloat(data);
    viture_pitch = makeFloat(data + 4);
    viture_yaw = -makeFloat(data + 8);

    if ( glut_initialized ) glutPostRedisplay();
}

// This is the viture_mcu_callback specific to v4l2_gl.c, used if USE_VITURE is defined
// OR if our own viture_connection driver is used AND this callback is registered with it.
static void app_viture_mcu_event_handler(uint16_t msgid, uint8_t *data, uint16_t len, uint32_t ts)
{
    (void)ts; // ts might be unused
    printf("V4L2_GL MCU Event: ID=0x%04X, Len=%u, Data: ", msgid, len);
    for (uint16_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}


static inline unsigned char clamp(int val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (unsigned char)val;
}

// Convert a single NV24 frame to RGB24
void convert_nv24_to_rgb(const unsigned char *y_plane_data, const unsigned char *uv_plane_data, unsigned char *rgb, int width, int height) {
    const unsigned char *y_plane = y_plane_data;
    const unsigned char *uv_plane = uv_plane_data;

    for (int y_coord = 0; y_coord < height; y_coord++) {
        for (int x_coord = 0; x_coord < width; x_coord++) {
            int i = y_coord * width + x_coord;
            int uv_idx = i * 2; 
            
            int y_val = y_plane[i];
            int u_val = uv_plane[uv_idx];     
            int v_val = uv_plane[uv_idx + 1]; 

            int c = y_val - 16;
            int d = u_val - 128;
            int e = v_val - 128;

            int r_val = clamp((298 * c + 409 * e + 128) >> 8);
            int g_val = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
            int b_val = clamp((298 * c + 516 * d + 128) >> 8);
            
            rgb[i * 3] = r_val;
            rgb[i * 3 + 1] = g_val;
            rgb[i * 3 + 2] = b_val;
        }
    }
}

// Forward declaration
void fill_frame_with_pattern( unsigned char *rgb, int width, int height );

// Convert YUYV (YUV 4:2:2 packed) to RGB24
void convert_yuyv_to_rgb(const unsigned char *yuyv_data, unsigned char *rgb, int width, int height, size_t bytesused) {
    if (bytesused < (size_t)width * height * 2) {
        fprintf(stderr, "convert_yuyv_to_rgb: Not enough data. Expected %d, got %zu\n", width*height*2, bytesused);
        fill_frame_with_pattern(rgb, width, height); // Fill with pattern on error
        return;
    }

    for (int y_coord = 0; y_coord < height; y_coord++) {
        for (int x_coord = 0; x_coord < width; x_coord += 2) { 
            int yuyv_idx = (y_coord * width + x_coord) * 2; 
            int rgb_idx1 = (y_coord * width + x_coord) * 3;
            int rgb_idx2 = (y_coord * width + x_coord + 1) * 3;

            // Ensure yuyv_idx is non-negative before comparison with unsigned bytesused
            if (yuyv_idx < 0 || (size_t)(yuyv_idx + 3) >= bytesused) { 
                fprintf(stderr, "convert_yuyv_to_rgb: YUYV index out of bounds.\n");
                continue;
            }
            // Ensure rgb_idx2 is non-negative before comparison
             if (x_coord + 1 < width && (rgb_idx2 < 0 || (size_t)(rgb_idx2 + 2) >= (size_t)width*height*3)) { 
                fprintf(stderr, "convert_yuyv_to_rgb: RGB index out of bounds for second pixel.\n");
                continue;
            }
            // Ensure rgb_idx2 is non-negative before comparison
             if (x_coord + 1 < width && (rgb_idx2 < 0 || (size_t)(rgb_idx2 + 2) >= (size_t)width*height*3)) { 
                fprintf(stderr, "convert_yuyv_to_rgb: RGB index out of bounds for second pixel.\n");
                continue;
            }


            int y0 = yuyv_data[yuyv_idx + 0];
            int u  = yuyv_data[yuyv_idx + 1];
            int y1 = yuyv_data[yuyv_idx + 2];
            int v  = yuyv_data[yuyv_idx + 3];

            int c1 = y0 - 16;
            int d1 = u - 128;
            int e1 = v - 128;
            rgb[rgb_idx1 + 0] = clamp((298 * c1 + 409 * e1 + 128) >> 8);
            rgb[rgb_idx1 + 1] = clamp((298 * c1 - 100 * d1 - 208 * e1 + 128) >> 8);
            rgb[rgb_idx1 + 2] = clamp((298 * c1 + 516 * d1 + 128) >> 8);

            if (x_coord + 1 < width) { 
                int c2 = y1 - 16;
                // U and V are shared for both pixels (d1, e1 can be reused as d2, e2)
                rgb[rgb_idx2 + 0] = clamp((298 * c2 + 409 * e1 + 128) >> 8);
                rgb[rgb_idx2 + 1] = clamp((298 * c2 - 100 * d1 - 208 * e1 + 128) >> 8);
                rgb[rgb_idx2 + 2] = clamp((298 * c2 + 516 * d1 + 128) >> 8);
            }
        }
    }
}


void fill_frame_with_pattern( unsigned char *rgb, int width, int height ) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int i = (y * width + x) * 3;
            rgb[i] = (x + y) % 256;       
            rgb[i + 1] = (x * 2 + y) % 256; 
            rgb[i + 2] = (x * 3 + y) % 256; 
        }
    }
}



// --- V4L2 Initialization ---
void init_v4l2() {
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    
    fd = open(DEVICE_PATH, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) { perror("Cannot open device"); exit(EXIT_FAILURE); }

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("VIDIOC_QUERYCAP"); exit(EXIT_FAILURE); }

    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        active_buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        printf("V4L2: Device supports multi-planar video capture.\n");
    } else if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
        active_buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        printf("V4L2: Device supports single-planar video capture.\n");
    } else {
        fprintf(stderr, "Device does not support video capture (single or multi-planar)\n"); exit(EXIT_FAILURE);
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support streaming\n"); exit(EXIT_FAILURE);
    }
    printf("V4L2: Device supports streaming.\n");

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = active_buffer_type;
    bool format_set = false;

    if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        fmt.fmt.pix_mp.width       = FRAME_WIDTH;
        fmt.fmt.pix_mp.height      = FRAME_HEIGHT;
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV24; 
        fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.num_planes  = 2; 

        if (ioctl(fd, VIDIOC_S_FMT, &fmt) == 0) {
            active_pixel_format = fmt.fmt.pix_mp.pixelformat;
            num_planes_per_buffer = fmt.fmt.pix_mp.num_planes;
            if (num_planes_per_buffer >= 2 && active_pixel_format == V4L2_PIX_FMT_NV24) { // Check if NV24 with 2 planes was accepted
                 printf("V4L2: Format set to %dx%d, pixelformat NV24, %u planes (MPLANE)\n",
                       fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, num_planes_per_buffer);
                format_set = true;
            } else {
                 fprintf(stderr, "V4L2: Device did not accept NV24 with 2 planes as expected. Planes: %u, Format: %c%c%c%c\n",
                    num_planes_per_buffer, (active_pixel_format)&0xFF, (active_pixel_format>>8)&0xFF,
                    (active_pixel_format>>16)&0xFF, (active_pixel_format>>24)&0xFF);
                // Fallback will be attempted below
            }
        } else {
            perror("VIDIOC_S_FMT (MPLANE NV24) failed");
        }
    }
    
    if (!format_set) { // Try single-plane YUYV if MPLANE NV24 failed or if single-plane device
        printf("V4L2: Attempting single-plane YUYV format.\n");
        active_buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE; 
        fmt.type = active_buffer_type; 
        fmt.fmt.pix.width       = FRAME_WIDTH;
        fmt.fmt.pix.height      = FRAME_HEIGHT;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) == 0) {
            active_pixel_format = fmt.fmt.pix.pixelformat;
            num_planes_per_buffer = 1; 
            printf("V4L2: Format set to %dx%d, pixelformat YUYV (SINGLE-PLANE)\n",
                   fmt.fmt.pix.width, fmt.fmt.pix.height);
            format_set = true;
        } else {
            perror("VIDIOC_S_FMT (SINGLE-PLANE YUYV) also failed.");
            // Optionally, try MJPEG as a last resort for single-plane
            // fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
            // if (ioctl(fd, VIDIOC_S_FMT, &fmt) == 0) { ... }
            exit(EXIT_FAILURE);
        }
    }
    if (!format_set) { // Should not happen if one of the S_FMT succeeded
        fprintf(stderr, "V4L2: Failed to set any video format.\n");
        exit(EXIT_FAILURE);
    }

    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = active_buffer_type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("VIDIOC_REQBUFS"); exit(EXIT_FAILURE); }
    n_buffers = req.count;
    printf("V4L2: %d buffers requested.\n", n_buffers);

    buffers_mp = calloc(n_buffers, sizeof(*buffers_mp)); 
    for (unsigned int i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = active_buffer_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            struct v4l2_plane planes_query[VIDEO_MAX_PLANES];
            memset(planes_query, 0, sizeof(planes_query));
            buf.m.planes = planes_query;
            buf.length = num_planes_per_buffer; 
        }

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("VIDIOC_QUERYBUF"); exit(EXIT_FAILURE); }

        if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buffers_mp[i].num_planes_in_buffer = num_planes_per_buffer;
            for (unsigned int p = 0; p < num_planes_per_buffer; ++p) {
                buffers_mp[i].planes[p].length = buf.m.planes[p].length;
                buffers_mp[i].planes[p].start = mmap(NULL, buf.m.planes[p].length,
                                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                                     fd, buf.m.planes[p].m.mem_offset);
                if (buffers_mp[i].planes[p].start == MAP_FAILED) { perror("mmap mplane"); exit(EXIT_FAILURE); }
            }
        } else { 
            buffers_mp[i].num_planes_in_buffer = 1;
            buffers_mp[i].planes[0].length = buf.length; 
            buffers_mp[i].planes[0].start = mmap(NULL, buf.length,
                                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                                 fd, buf.m.offset); 
            if (buffers_mp[i].planes[0].start == MAP_FAILED) { perror("mmap splane"); exit(EXIT_FAILURE); }
        }
    }
    printf("V4L2: Buffers and planes mapped.\n");

    for (unsigned int i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = active_buffer_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            struct v4l2_plane planes_q[VIDEO_MAX_PLANES]; 
            memset(planes_q, 0, sizeof(planes_q));
            buf.m.planes = planes_q;
            buf.length = num_planes_per_buffer; 
        }

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("VIDIOC_QBUF"); exit(EXIT_FAILURE); }
    }
    printf("V4L2: Buffers queued.\n");

    if (ioctl(fd, VIDIOC_STREAMON, &active_buffer_type) < 0) { perror("VIDIOC_STREAMON"); exit(EXIT_FAILURE); }
    printf("V4L2: Streaming started.\n");
}

// --- OpenGL/GLUT Functions ---

void cleanup() {
    printf("Cleaning up...\n");
    if (use_viture_imu) {
        printf("Viture: Disabling IMU and de-initializing...\n");

#ifdef USE_VITURE
        set_imu(false); // Assumes this is from 3rdparty/include/viture.h
        deinit();       // Assumes this is from 3rdparty/include/viture.h
#else
        set_imu(false); 
        viture_driver_close(); 
#endif
    }

    if (fd != -1) {
        ioctl(fd, VIDIOC_STREAMOFF, &active_buffer_type);
        if (buffers_mp) {
            for (unsigned int i = 0; i < n_buffers; ++i) {
                for (unsigned int p = 0; p < buffers_mp[i].num_planes_in_buffer; ++p) { 
                     if (buffers_mp[i].planes[p].start && buffers_mp[i].planes[p].start != MAP_FAILED) {
                        munmap(buffers_mp[i].planes[p].start, buffers_mp[i].planes[p].length);
                     }
                }
            }
        }
        close(fd);
        fd = -1;
    }
    if (buffers_mp) { free(buffers_mp); buffers_mp = NULL; }
    if (rgb_frames[0]) { free(rgb_frames[0]); rgb_frames[0] = NULL; }
    if (rgb_frames[1]) { free(rgb_frames[1]); rgb_frames[1] = NULL; }
    pthread_mutex_destroy(&frame_mutex); 
    if (texture_id != 0) glDeleteTextures(1, &texture_id);
    printf("Cleanup complete.\n");
}

void display() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0); 

    if (use_viture_imu) {
        glRotatef(viture_yaw - initial_yaw_offset, 0.0f, 1.0f, 0.0f);   
        glRotatef(viture_pitch - initial_pitch_offset, 1.0f, 0.0f, 0.0f); 
        glRotatef(viture_roll - initial_roll_offset, 0.0f, 0.0f, 1.0f);  
    } else {
        static float angle = 0.0f;
        angle += 0.2f;
        if (angle > 360.0f) angle -= 360.0f;
        glRotatef(15.0f, 1.0f, 0.0f, 0.0f); 
        glRotatef(angle, 0.0f, 1.0f, 0.0f); 
    }

    glTranslatef(0.0f, 0.0f, -g_plane_orbit_distance);
    glScalef(g_plane_scale, g_plane_scale, 1.0f);

    bool generate_texture = false;
    pthread_mutex_lock(&frame_mutex);
    if (new_frame_captured) {
        int temp = front_buffer_idx;
        front_buffer_idx = back_buffer_idx;
        back_buffer_idx = temp;
        new_frame_captured = false;
        generate_texture = true; 
    }
    pthread_mutex_unlock(&frame_mutex);

    glBindTexture(GL_TEXTURE_2D, texture_id);
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
    struct v4l2_plane planes_dq[VIDEO_MAX_PLANES]; 
    memset(&buf, 0, sizeof(buf));
    
    buf.type = active_buffer_type;
    buf.memory = V4L2_MEMORY_MMAP;

    if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        memset(planes_dq, 0, sizeof(planes_dq));
        buf.m.planes = planes_dq;
        buf.length = num_planes_per_buffer; 
    }

    if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
        if (errno == EAGAIN) {
            return;
        }
        perror("VIDIOC_DQBUF");
        exit(EXIT_FAILURE);
    }
    
    if (display_test_pattern) {
        fill_frame_with_pattern(rgb_frames[back_buffer_idx], FRAME_WIDTH, FRAME_HEIGHT);
    } else {
        if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            if (active_pixel_format == V4L2_PIX_FMT_NV24 && num_planes_per_buffer >= 2) {
                convert_nv24_to_rgb(
                    (const unsigned char *)buffers_mp[buf.index].planes[0].start,
                    (const unsigned char *)buffers_mp[buf.index].planes[1].start,
                    rgb_frames[back_buffer_idx], FRAME_WIDTH, FRAME_HEIGHT);
            } else if (active_pixel_format == V4L2_PIX_FMT_NV24 && num_planes_per_buffer == 1) { 
                // This case was modified by the user - assumes NV24 data packed in one plane
                convert_nv24_to_rgb(
                    (const unsigned char *)buffers_mp[buf.index].planes[0].start,
                    (const unsigned char *)buffers_mp[buf.index].planes[0].start + FRAME_WIDTH * FRAME_HEIGHT, // Offset for UV data
                    rgb_frames[back_buffer_idx], FRAME_WIDTH, FRAME_HEIGHT);
            } else {
                 fprintf(stderr, "Error: Unsupported MPLANE pixel format %c%c%c%c or plane count %u\n",
                        (active_pixel_format)&0xFF, (active_pixel_format>>8)&0xFF,
                        (active_pixel_format>>16)&0xFF, (active_pixel_format>>24)&0xFF,
                        num_planes_per_buffer);
                fill_frame_with_pattern(rgb_frames[back_buffer_idx], FRAME_WIDTH, FRAME_HEIGHT);
            }
        } else { // Single-plane
            if (active_pixel_format == V4L2_PIX_FMT_YUYV) {
                convert_yuyv_to_rgb((const unsigned char *)buffers_mp[buf.index].planes[0].start, 
                                     rgb_frames[back_buffer_idx], FRAME_WIDTH, FRAME_HEIGHT, buf.bytesused);
            } else {
                 fprintf(stderr, "Error: Unsupported SINGLE-PLANE pixel format %c%c%c%c\n",
                        (active_pixel_format)&0xFF, (active_pixel_format>>8)&0xFF,
                        (active_pixel_format>>16)&0xFF, (active_pixel_format>>24)&0xFF);
                fill_frame_with_pattern(rgb_frames[back_buffer_idx], FRAME_WIDTH, FRAME_HEIGHT);
            }
        }
    }

    pthread_mutex_lock(&frame_mutex);
    new_frame_captured = true;
    pthread_mutex_unlock(&frame_mutex);

    if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
        perror("VIDIOC_QBUF");
        exit(EXIT_FAILURE);
    }

    glutPostRedisplay(); 
}

void idle() {
    capture_and_update();
}

void init_gl() {
    if (pthread_mutex_init(&frame_mutex, NULL) != 0) {
        perror("Mutex init failed");
        exit(EXIT_FAILURE);
    }

    rgb_frames[0] = malloc(FRAME_WIDTH * FRAME_HEIGHT * 3);
    rgb_frames[1] = malloc(FRAME_WIDTH * FRAME_HEIGHT * 3);

    if (!rgb_frames[0] || !rgb_frames[1]) {
        fprintf(stderr, "Failed to allocate memory for RGB frames\n");
        exit(EXIT_FAILURE);
    }
    memset(rgb_frames[0], 0, FRAME_WIDTH * FRAME_HEIGHT * 3);
    memset(rgb_frames[1], 0, FRAME_WIDTH * FRAME_HEIGHT * 3);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);

    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, FRAME_WIDTH, FRAME_HEIGHT, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, rgb_frames[front_buffer_idx]); 

    glut_initialized = true; 
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
                if (endptr != argv[i+1] && *endptr == '\0') { 
                    g_plane_orbit_distance = val;
                    i++; 
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
                if (endptr != argv[i+1] && *endptr == '\0') { 
                    g_plane_scale = val;
                    if (g_plane_scale <= 0.0f) { 
                        fprintf(stderr, "Warning: Plane scale should be positive. Using 1.0.\n");
                        g_plane_scale = 1.0f;
                    }
                    i++; 
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

if (use_viture_imu) {
#ifdef USE_VITURE
    // This block uses the official Viture SDK (assumed to be in 3rdparty)
    printf("Viture: Initializing with official SDK...\n");
    // The 'init' and 'set_imu' here are from the official SDK's viture.h
    // Their signatures might be different from our reimplemented ones.
    // The app_viture_imu_data_handler and app_viture_mcu_event_handler are local to v4l2_gl.c
    init(app_viture_imu_data_handler, app_viture_mcu_event_handler); 
    set_imu(true); 
    printf("Viture: IMU stream enabled via official SDK.\n");
#else
    // This block uses our reimplemented Viture driver (viture_connection.c)
    printf("Viture: Initializing with custom driver...\n");
    if (!viture_driver_init()) { 
        fprintf(stderr, "V4L2_GL: Failed to initialize custom Viture driver.\n");
        // Decide if this is fatal or if the app can continue without IMU
        use_viture_imu = false; // Disable IMU features if driver fails
    } else {
        // Register callbacks with our driver
        viture_set_imu_data_callback(app_viture_imu_data_handler); // Uses the handler in viture_connection.c
        viture_set_mcu_event_callback(app_viture_mcu_event_handler); // Uses the handler in v4l2_gl.c
        
        uint32_t imu_set_status = set_imu(true); // Calls our set_imu
        if (imu_set_status != 0) { // Assuming 0 is success for our driver
            fprintf(stderr, "V4L2_GL: set_imu(true) command failed with status %u using custom driver.\n", imu_set_status);
        } else {
            printf("Viture: IMU stream enabled via custom driver.\n");
        }
    }
#endif
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
