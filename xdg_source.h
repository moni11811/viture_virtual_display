#ifndef XDG_SOURCE_H
#define XDG_SOURCE_H

#include <glib.h> // For gboolean

// Define the structure that holds the frame data.
// This is the same structure defined in xdg_source.c.
// If it were more complex or private, xdg_source.c might expose an opaque pointer.
typedef struct {
    unsigned char *data;    // RGB or RGBA pixel data
    int width;
    int height;
    int stride;             // Bytes per row in the data buffer
    // Internal fields used by xdg_source.c, not typically for external use directly
    // but part of the struct returned by get_xdg_root_window_frame_sync()
    gboolean completed; 
    gboolean success;
    GMainLoop *loop_for_sync_call; 
} XDGFrameRequest; // Consider renaming to XDGFrame for public API clarity if preferred

/**
 * @brief Captures the current root window (entire screen) using XDG Desktop Portal.
 * 
 * This function operates synchronously by running a nested GMainLoop.
 * It makes a request to the org.freedesktop.portal.Screenshot interface.
 * The portal typically saves the screenshot to a temporary file and returns a URI.
 * This function then loads the image from the URI (if it's a file URI and supported format like PNG)
 * using GdkPixbuf and converts it to an RGB pixel buffer.
 * 
 * The caller is responsible for calling free_xdg_frame_request() on the returned pointer
 * when the frame data is no longer needed.
 * 
 * @return XDGFrameRequest* A pointer to an XDGFrameRequest structure containing the
 *         pixel data and metadata if successful, or NULL on failure (e.g., portal error,
 *         timeout, unsupported URI, image loading error).
 *         If non-NULL, frame_request->data will point to the allocated pixel buffer.
 *         frame_request->success will be TRUE if data was populated.
 */
XDGFrameRequest* get_xdg_root_window_frame_sync(void);

/**
 * @brief Frees the resources associated with an XDGFrameRequest structure.
 * 
 * This includes the pixel data buffer (frame_req->data) and the structure itself.
 * Should be called on any XDGFrameRequest pointer obtained from 
 * get_xdg_root_window_frame_sync(), whether it was successful or not (if non-NULL).
 * 
 * @param frame_req Pointer to the XDGFrameRequest structure to free.
 */
void free_xdg_frame_request(XDGFrameRequest* frame_req);

#endif // XDG_SOURCE_H
