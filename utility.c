
#include "utility.h"


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

void convert_yuyv_to_rgb(const unsigned char *yuyv_data, unsigned char *rgb, int width, int height, size_t bytesused) {
    if (bytesused < (size_t)width * height * 2) {
        fprintf(stderr, "convert_yuyv_to_rgb: Not enough data. Expected %d, got %zu\n", width*height*2, bytesused);
        fill_frame_with_pattern(rgb, width, height); 
        return;
    }

    for (int y_coord = 0; y_coord < height; y_coord++) {
        for (int x_coord = 0; x_coord < width; x_coord += 2) { 
            int yuyv_idx = (y_coord * width + x_coord) * 2; 
            int rgb_idx1 = (y_coord * width + x_coord) * 3;
            int rgb_idx2 = (y_coord * width + x_coord + 1) * 3;

            if (yuyv_idx < 0 || (size_t)(yuyv_idx + 3) >= bytesused) { 
                fprintf(stderr, "convert_yuyv_to_rgb: YUYV index out of bounds.\n");
                continue;
            }
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
