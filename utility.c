
#include "utility.h"
#ifdef ARCH_X86_64
#include "3rdparty/include/SimdLib.h"
#endif
#include <jpeglib.h>

static inline unsigned char clamp(int val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (unsigned char)val;
}

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

void convert_yuyv_to_bgr(const unsigned char *yuyv_data, unsigned char *bgr, int width, int height, size_t bytesused) {
    if (bytesused < (size_t)width * height * 2) {
        fprintf(stderr, "convert_yuyv_to_bgr: Not enough data. Expected %d, got %zu\n", width*height*2, bytesused);
        fill_frame_with_pattern(bgr, width, height);
        return;
    }
#ifdef ARCH_X86_64
    static unsigned char *uyvy_buf = NULL;
    static size_t uyvy_buf_size = 0;
    size_t needed = (size_t)width * height * 2;
    if (uyvy_buf_size < needed) {
        unsigned char *new_buf = (unsigned char *)realloc(uyvy_buf, needed);
        if (!new_buf) {
            fprintf(stderr, "convert_yuyv_to_bgr: Failed to allocate temp buffer.\n");
            fill_frame_with_pattern(bgr, width, height);
            return;
        }
        uyvy_buf = new_buf;
        uyvy_buf_size = needed;
    }
    for (size_t i = 0; i < needed; i += 4) {
        uyvy_buf[i + 0] = yuyv_data[i + 1]; // U
        uyvy_buf[i + 1] = yuyv_data[i + 0]; // Y0
        uyvy_buf[i + 2] = yuyv_data[i + 3]; // V
        uyvy_buf[i + 3] = yuyv_data[i + 2]; // Y1
    }
    SimdUyvy422ToBgr(uyvy_buf, width * 2, width, height, bgr, width * 3, SimdYuvBt601);
#else
    for (int y_coord = 0; y_coord < height; y_coord++) {
        for (int x_coord = 0; x_coord < width; x_coord += 2) {
            int yuyv_idx = (y_coord * width + x_coord) * 2;
            int bgr_idx1 = (y_coord * width + x_coord) * 3;
            int bgr_idx2 = (y_coord * width + x_coord + 1) * 3;

            if ((size_t)(yuyv_idx + 3) >= bytesused) {
                fprintf(stderr, "convert_yuyv_to_bgr: YUYV index out of bounds.\n");
                continue;
            }

            int y0 = yuyv_data[yuyv_idx + 0];
            int u  = yuyv_data[yuyv_idx + 1];
            int y1 = yuyv_data[yuyv_idx + 2];
            int v  = yuyv_data[yuyv_idx + 3];

            int c1 = y0 - 16;
            int d1 = u - 128;
            int e1 = v - 128;
            bgr[bgr_idx1 + 2] = clamp((298 * c1 + 409 * e1 + 128) >> 8);
            bgr[bgr_idx1 + 1] = clamp((298 * c1 - 100 * d1 - 208 * e1 + 128) >> 8);
            bgr[bgr_idx1 + 0] = clamp((298 * c1 + 516 * d1 + 128) >> 8);

            if (x_coord + 1 < width) {
                int c2 = y1 - 16;
                bgr[bgr_idx2 + 2] = clamp((298 * c2 + 409 * e1 + 128) >> 8);
                bgr[bgr_idx2 + 1] = clamp((298 * c2 - 100 * d1 - 208 * e1 + 128) >> 8);
                bgr[bgr_idx2 + 0] = clamp((298 * c2 + 516 * d1 + 128) >> 8);
            }
        }
    }
#endif
}

void convert_mjpeg_to_rgb(const unsigned char *jpeg_data, size_t len, unsigned char *rgb, int width, int height) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_data, len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        fill_frame_with_pattern(rgb, width, height);
        return;
    }
    jpeg_start_decompress(&cinfo);
    if (cinfo.output_width != (JDIMENSION)width || cinfo.output_height != (JDIMENSION)height) {
        fprintf(stderr, "convert_mjpeg_to_rgb: Dimension mismatch (%ux%u != %dx%d)\n",
                cinfo.output_width, cinfo.output_height, width, height);
    }
    unsigned row_stride = cinfo.output_width * cinfo.output_components;
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *buffer_array[1];
        buffer_array[0] = rgb + cinfo.output_scanline * row_stride;
        jpeg_read_scanlines(&cinfo, buffer_array, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
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
