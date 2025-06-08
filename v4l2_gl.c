#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>

#include <linux/videodev2.h>

#include <stdbool.h>


// Use GL/glut.h on macOS, GL/freeglut.h on Linux
#include <GL/freeglut.h> 

#ifdef USE_VITURE
#include "3rdparty/include/viture.h"
#else
#include "viture_connection.h" // Include our own Viture connection header
#endif

#include "utility.h"


// --- V4L2 and Frame Configuration ---
#define DEVICE_PATH      "/dev/video0"
#define FRAME_WIDTH      1920 // Requested width
#define FRAME_HEIGHT     1080 // Requested height
#define BUFFER_COUNT     4

// --- Global variables for V4L2 ---
static enum v4l2_buf_type active_buffer_type;
static __u32 active_pixel_format;
static int actual_frame_width = FRAME_WIDTH;   // Initialize with requested, update with actual
static int actual_frame_height = FRAME_HEIGHT; // Initialize with requested, update with actual

struct plane_info {
    void   *start;
    size_t length;
};

struct mplane_buffer {
    struct plane_info planes[VIDEO_MAX_PLANES];
    unsigned int num_planes_in_buffer; 
};

static int fd = -1;
static struct mplane_buffer *buffers_mp = NULL; 
static unsigned int n_buffers = 0;
static unsigned int num_planes_per_buffer = 0; 


// --- Global variables for OpenGL ---
static GLuint texture_id;
static unsigned char *rgb_frames[2] = {NULL, NULL}; 
static int front_buffer_idx = 0;                   
static int back_buffer_idx = 1;                    
static volatile bool new_frame_captured = false;   
static pthread_mutex_t frame_mutex;
static pthread_t capture_thread_id = 0; // Initialize to 0
static volatile bool stop_capture_thread_flag = false;

static bool glut_initialized = false;

static int fullscreen_mode = 0; 
static int display_test_pattern = 0;                
static float g_plane_orbit_distance = 1.0f;         
static float g_plane_scale = 1.0f;                  

// --- Helper Functions ---

static bool use_viture_imu = false;
static volatile float viture_roll = 0.0f;
static volatile float viture_pitch = 0.0f;
static volatile float viture_yaw = 0.0f;
static float initial_roll_offset = 0.0f;  
static float initial_pitch_offset = 0.0f; 
static float initial_yaw_offset = 0.0f;  
static bool initial_offsets_set = false; 

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

static void app_viture_imu_data_handler(uint8_t *data, uint16_t len, uint32_t ts) {
    (void)ts; 
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

}

static void app_viture_mcu_event_handler(uint16_t msgid, uint8_t *data, uint16_t len, uint32_t ts)
{
    (void)ts; 
    printf("V4L2_GL MCU Event: ID=0x%04X, Len=%u, Data: ", msgid, len);
    for (uint16_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
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
            if ( ( num_planes_per_buffer == 1 || num_planes_per_buffer == 2 ) && active_pixel_format == V4L2_PIX_FMT_NV24) {
                actual_frame_width = fmt.fmt.pix_mp.width;
                actual_frame_height = fmt.fmt.pix_mp.height;
                printf("V4L2: Format set to %dx%d, pixelformat NV24, %u planes (MPLANE)\n",
                       actual_frame_width, actual_frame_height, num_planes_per_buffer);
                format_set = true;
            } else {
                 fprintf(stderr, "V4L2: Device did not accept NV24 with 1 or 2 planes as expected. Planes: %u, Format: %c%c%c%c\n",
                    num_planes_per_buffer, (active_pixel_format)&0xFF, (active_pixel_format>>8)&0xFF,
                    (active_pixel_format>>16)&0xFF, (active_pixel_format>>24)&0xFF);
            }
        } else {
            perror("VIDIOC_S_FMT (MPLANE NV24) failed");
        }
    }
    
    if (!format_set) { 
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
            actual_frame_width = fmt.fmt.pix.width;
            actual_frame_height = fmt.fmt.pix.height;
            printf("V4L2: Format set to %dx%d, pixelformat YUYV (SINGLE-PLANE)\n",
                   actual_frame_width, actual_frame_height);
            format_set = true;
        } else {
            perror("VIDIOC_S_FMT (SINGLE-PLANE YUYV) also failed.");
            exit(EXIT_FAILURE);
        }
    }
    if (!format_set) { 
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
        set_imu(false); 
        deinit();       
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
    
    // Signal capture thread to stop and wait for it
    if (capture_thread_id != 0) {
        printf("V4L2_GL: Signaling capture thread to stop...\n");
        stop_capture_thread_flag = true;
        printf("V4L2_GL: Joining capture thread...\n");
        pthread_join(capture_thread_id, NULL);
        capture_thread_id = 0; // Reset after joining
        printf("V4L2_GL: Capture thread joined.\n");
    }
    
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
    if ( generate_texture ) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, actual_frame_width, actual_frame_height, GL_RGB, GL_UNSIGNED_BYTE, rgb_frames[front_buffer_idx]);

    float aspect_ratio = (float)actual_frame_width / (float)actual_frame_height;
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
        fill_frame_with_pattern(rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height);
    } else {
        if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            if (active_pixel_format == V4L2_PIX_FMT_NV24 && num_planes_per_buffer >= 2) {
                convert_nv24_to_rgb(
                    (const unsigned char *)buffers_mp[buf.index].planes[0].start,
                    (const unsigned char *)buffers_mp[buf.index].planes[1].start,
                    rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height);
            } else if (active_pixel_format == V4L2_PIX_FMT_NV24 && num_planes_per_buffer == 1) { 
                convert_nv24_to_rgb(
                    (const unsigned char *)buffers_mp[buf.index].planes[0].start,
                    (const unsigned char *)buffers_mp[buf.index].planes[0].start + actual_frame_width * actual_frame_height, 
                    rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height);
            } else {
                 fprintf(stderr, "Error: Unsupported MPLANE pixel format %c%c%c%c or plane count %u\n",
                        (active_pixel_format)&0xFF, (active_pixel_format>>8)&0xFF,
                        (active_pixel_format>>16)&0xFF, (active_pixel_format>>24)&0xFF,
                        num_planes_per_buffer);
                fill_frame_with_pattern(rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height);
            }
        } else { // Single-plane
            if (active_pixel_format == V4L2_PIX_FMT_YUYV) {
                convert_yuyv_to_rgb((const unsigned char *)buffers_mp[buf.index].planes[0].start, 
                                     rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height, buf.bytesused);
            } else {
                 fprintf(stderr, "Error: Unsupported SINGLE-PLANE pixel format %c%c%c%c\n",
                        (active_pixel_format)&0xFF, (active_pixel_format>>8)&0xFF,
                        (active_pixel_format>>16)&0xFF, (active_pixel_format>>24)&0xFF);
                fill_frame_with_pattern(rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height);
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

}

#define TARGET_FPS 60 // This can still be used for display refresh rate

// --- Capture Thread ---
void *capture_thread_func(void *arg) {
    (void)arg; // Unused
    printf("V4L2_GL: Capture thread started.\n");
    struct timespec ts;
    ts.tv_sec = 0;
    // Use the TARGET_FPS define here
    ts.tv_nsec = (1000000000L / TARGET_FPS) / 2; // Sleep for a short duration if EAGAIN. Use long literal for 1 billion.

    while (!stop_capture_thread_flag) {
        struct v4l2_buffer buf_check; // For checking DQBUF result
        memset(&buf_check, 0, sizeof(buf_check));
        buf_check.type = active_buffer_type;
        buf_check.memory = V4L2_MEMORY_MMAP;
        if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            struct v4l2_plane planes_temp[VIDEO_MAX_PLANES];
            memset(planes_temp, 0, sizeof(planes_temp));
            buf_check.m.planes = planes_temp;
            buf_check.length = num_planes_per_buffer;
        }

        // Non-blocking check if a buffer is ready
        // We use a temporary buffer struct for ioctl to avoid issues if capture_and_update is slow
        // and another DQBUF happens before QBUF in capture_and_update.
        // However, capture_and_update itself does DQBUF.
        // The main purpose here is to call capture_and_update when data is likely available.
        // A more robust way might be to use select() or poll() on the fd.
        // For now, we'll call capture_and_update and let it handle EAGAIN.

        capture_and_update(); // This function now handles its own EAGAIN

        // If capture_and_update returned due to EAGAIN, we can sleep a bit
        // This check is a bit indirect. A better way would be for capture_and_update to return a status.
        // For now, we assume if new_frame_captured is false after a call, it might have been EAGAIN.
        pthread_mutex_lock(&frame_mutex);
        bool frame_was_newly_captured = new_frame_captured; // Check if capture_and_update set it
        pthread_mutex_unlock(&frame_mutex);

        if (!frame_was_newly_captured) { // If no new frame was processed (e.g. EAGAIN)
             nanosleep(&ts, NULL); // Sleep briefly to avoid busy-waiting
        }
        // No explicit sleep if a frame was captured, as V4L2 DQBUF/QBUF cycle provides throttling
    }
    printf("V4L2_GL: Capture thread stopping.\n");
    return NULL;
}

// static clock_t last_redisplay_time = 0; // Moved TARGET_FPS definition earlier
static clock_t last_redisplay_time = 0;
void idle()
{
    // This function is now only responsible for triggering redisplay
    // The actual frame capture is handled by capture_thread_func
    clock_t current_time = clock();
    if ( (current_time - last_redisplay_time) * 1000 / CLOCKS_PER_SEC >= (1000 / TARGET_FPS) ) {
        last_redisplay_time = current_time;
        glutPostRedisplay();
    }
}

void init_gl() {
    if (pthread_mutex_init(&frame_mutex, NULL) != 0) {
        perror("Mutex init failed");
        exit(EXIT_FAILURE);
    }

    // Allocate RGB frames based on actual dimensions AFTER V4L2 init
    // This means init_gl() should be called after init_v4l2()
    rgb_frames[0] = malloc(actual_frame_width * actual_frame_height * 3);
    rgb_frames[1] = malloc(actual_frame_width * actual_frame_height * 3);

    if (!rgb_frames[0] || !rgb_frames[1]) {
        fprintf(stderr, "Failed to allocate memory for RGB frames\n");
        exit(EXIT_FAILURE);
    }
    memset(rgb_frames[0], 0, actual_frame_width * actual_frame_height * 3);
    memset(rgb_frames[1], 0, actual_frame_width * actual_frame_height * 3);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);

    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, actual_frame_width, actual_frame_height, 0,
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
    printf("Viture: Initializing with official SDK...\n");
    init(app_viture_imu_data_handler, app_viture_mcu_event_handler); 
    set_imu(true); 
    printf("Viture: IMU stream enabled via official SDK.\n");
#else
    printf("Viture: Initializing with custom driver...\n");
    if (!viture_driver_init()) { 
        fprintf(stderr, "V4L2_GL: Failed to initialize custom Viture driver.\n");
        use_viture_imu = false; 
    } else {
        viture_set_imu_data_callback(app_viture_imu_data_handler); 
        viture_set_mcu_event_callback(app_viture_mcu_event_handler); 
        
        uint32_t imu_set_status = set_imu(true); 
        if (imu_set_status != 0) { 
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
    
    init_v4l2(); 
    init_gl();   

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutIdleFunc(idle);
    atexit(cleanup);

    // Create and start the capture thread
    printf("V4L2_GL: Creating capture thread...\n");
    if (pthread_create(&capture_thread_id, NULL, capture_thread_func, NULL) != 0) {
        perror("Failed to create capture thread");
        // Perform cleanup before exiting if thread creation fails
        cleanup(); 
        exit(EXIT_FAILURE);
    }
    printf("V4L2_GL: Capture thread created.\n");

    printf("\n--- Starting main loop ---\n");
    glutMainLoop();
    return 0;
}
