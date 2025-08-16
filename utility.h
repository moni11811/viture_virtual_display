#ifndef _V4L2_GL_UTILITY_H_
#define _V4L2_GL_UTILITY_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdbool.h>


void convert_nv24_to_rgb(const unsigned char *y_plane_data, const unsigned char *uv_plane_data, unsigned char *rgb, int width, int height);
void fill_frame_with_pattern(unsigned char *rgb, int width, int height);
void convert_yuyv_to_bgr(const unsigned char *yuyv_data, unsigned char *bgr, int width, int height, size_t bytesused);
void convert_mjpeg_to_rgb(const unsigned char *jpeg_data, size_t len, unsigned char *rgb, int width, int height);

#endif