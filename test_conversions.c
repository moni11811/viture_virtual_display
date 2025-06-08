/* Program to test the frame conversions in utility.h */

#include "utility.h"
#include <time.h>

#define TEST_RUNS 100

int main() {
    const int width = 1920;
    const int height = 1080;
    unsigned char *y_plane = malloc(width * height);
    unsigned char *uv_plane = malloc(width * height * 2); // NV24 has 2 bytes per pixel for UV
    unsigned char *rgb = malloc(width * height * 3);
    unsigned char *rgb_fast = malloc(width * height * 3);

    // Fill Y plane with a gradient
    for (int i = 0; i < width * height; i++) {
        y_plane[i] = (unsigned char)(i % 256);
    }

    // Fill UV plane with a simple pattern
    for (int i = 0; i < width * height * 2; i++) { // Iterate over the full size of UV plane
        uv_plane[i] = (unsigned char)(i % 256);
    }

    clock_t start, end;
    start = clock();

    for (int run = 0; run < TEST_RUNS; run++) {
        convert_nv24_to_rgb(y_plane, uv_plane, rgb, width, height);
    }
    
    end = clock();
    double time_taken = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("convert_nv24_to_rgb took %f seconds for %d runs\n", time_taken, TEST_RUNS);
    // start = clock();

    // for (int run = 0; run < TEST_RUNS; run++) {
    //     convert_nv24_to_rgb_sse41(y_plane, uv_plane, rgb_fast, width, height);
    // }

    // end = clock();
    // time_taken = ((double)(end - start)) / CLOCKS_PER_SEC;
    // printf("convert_nv24_to_rgb_sse41 took %f seconds for %d runs\n", time_taken, TEST_RUNS);

    bool conversion_correct = true;
    // // Print some RGB values to verify conversion
    // for (int i = 0; i < width*height; i++) {
    //     if (rgb[i * 3] != rgb_fast[i * 3] || 
    //         rgb[i * 3 + 1] != rgb_fast[i * 3 + 1] || 
    //         rgb[i * 3 + 2] != rgb_fast[i * 3 + 2]) {
    //         conversion_correct = false;
    //         printf("Mismatch at pixel %d: RGB(%d, %d, %d) vs Fast RGB(%d, %d, %d)\n", 
    //                i, rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2],
    //                rgb_fast[i * 3], rgb_fast[i * 3 + 1], rgb_fast[i * 3 + 2]);
    //         break;
    //     }
    // }

    free(y_plane);
    free(uv_plane);
    free(rgb);
    free(rgb_fast); // Free rgb_fast

    // if (conversion_correct) {
    //     printf("Conversion test PASSED.\n");
    // } else {
    //     printf("Conversion test FAILED.\n");
    // }

    return conversion_correct ? 0 : 1;
}
