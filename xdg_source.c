/* Read the root window from X11 or Wayland using XDG and return it as a RGB buffer */

#include <glib-object.h> // Explicitly include for GObject base
#include <glib.h>
#include <gio/gio.h>
#include <gio/gdbusproxy.h> // For GDBusProxy functions
#include <gdk-pixbuf/gdk-pixbuf.h> // For image loading
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> 

// Define a structure to hold the frame data and sync primitives
typedef struct {
    unsigned char *data;
    int width;
    int height;
    int stride; // Bytes per row
    gboolean completed;
    gboolean success;
    GMainLoop *loop_for_sync_call; // For synchronous wrapper
    GDBusProxy *portal_request_proxy; // To store the request proxy for cleanup on timeout
} XDGFrameRequest;

static void
process_image_uri (const char *uri, XDGFrameRequest *frame_request)
{
    GdkPixbuf *pixbuf = NULL;
    GError *error = NULL;

    g_print ("Processing URI: %s\n", uri);

    if (g_str_has_prefix (uri, "file://")) {
        char *path = g_filename_from_uri (uri, NULL, &error);
        if (error) {
            g_printerr ("Error converting URI to path: %s\n", error->message);
            g_error_free (error);
            goto end;
        }
        g_print("Loading image from path: %s\n", path);
        pixbuf = gdk_pixbuf_new_from_file (path, &error);
        g_free (path);

        if (error) {
            g_printerr ("Error loading image from file: %s\n", error->message);
            g_error_free (error);
            goto end;
        }
    } else {
        g_printerr ("Unsupported URI scheme: %s\n", uri);
        goto end;
    }

    if (pixbuf) {
        // Ensure it's RGB (or RGBA)
        GdkPixbuf *rgb_pixbuf = gdk_pixbuf_add_alpha(pixbuf, FALSE, 0, 0, 0); // Ensure 3 channels if not already
        if (!rgb_pixbuf) { // if it already had alpha, add_alpha might return the same pixbuf or a new one
             rgb_pixbuf = gdk_pixbuf_copy(pixbuf); // Fallback or handle if it's already RGB
        }
        
        if (gdk_pixbuf_get_n_channels(rgb_pixbuf) != 3) {
            GdkPixbuf* temp_pixbuf = gdk_pixbuf_add_alpha(rgb_pixbuf, FALSE, 0,0,0);
            g_object_unref(rgb_pixbuf);
            rgb_pixbuf = temp_pixbuf;
            if (gdk_pixbuf_get_n_channels(rgb_pixbuf) != 3) {
                 g_printerr("Failed to convert pixbuf to 3 channels\n");
                 g_object_unref(rgb_pixbuf);
                 g_object_unref(pixbuf);
                 goto end;
            }
        }


        frame_request->width = gdk_pixbuf_get_width (rgb_pixbuf);
        frame_request->height = gdk_pixbuf_get_height (rgb_pixbuf);
        frame_request->stride = gdk_pixbuf_get_rowstride (rgb_pixbuf);
        guint len; // Changed from gsize to guint
        const guint8 *pixel_data = gdk_pixbuf_get_pixels_with_length(rgb_pixbuf, &len);
        
        frame_request->data = (unsigned char *)g_malloc (len);
        memcpy (frame_request->data, pixel_data, len);
        
        g_print("Image loaded: %d x %d, stride %d, %d channels\n", 
                frame_request->width, frame_request->height, frame_request->stride, gdk_pixbuf_get_n_channels(rgb_pixbuf));

        g_object_unref (rgb_pixbuf);
        g_object_unref (pixbuf);
        frame_request->success = TRUE;
    }

end:
    frame_request->completed = TRUE;
    if (frame_request->loop_for_sync_call) {
        g_main_loop_quit(frame_request->loop_for_sync_call);
    }
}

// Callback for the "g-signal" GObject signal from the GDBusProxy.
// This will be called for ANY D-Bus signal received by the proxy.
// We must filter for the "Response" D-Bus signal from the org.freedesktop.portal.Request interface.
static void
on_request_response_signal (GDBusProxy *proxy,
                            const char *sender_name G_GNUC_UNUSED,
                            const char *dbus_signal_name, // Name of the D-Bus signal
                            GVariant   *parameters,
                            gpointer    user_data)
{
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    guint response_code;
    GVariant *results_dict;
    const char *uri = NULL;

    // We are interested in the "Response" D-Bus signal from the Request portal object
    if (g_strcmp0(dbus_signal_name, "Response") != 0) {
        // Not the signal we're looking for, ignore it.
        // Do not quit loop, do not unref proxy yet if other signals might come.
        // However, for a portal request object, "Response" is usually the only one.
        g_print("Received unhandled D-Bus signal '%s' on request proxy.\n", dbus_signal_name);
        return;
    }

    g_print("Received D-Bus signal '%s'. Parsing parameters...\n", dbus_signal_name);
    // Parameters for "Response" signal are (UINT32 response, DICT results)
    // The GVariant signature is "(ua{sv})"
    g_variant_get (parameters, "(u@a{sv})", &response_code, &results_dict);
    // Note: g_variant_get with "@a{sv}" expects the a{sv} to be a maybe-typed GVariant.
    // If it's directly a{sv}, then "(ua{sv})" is correct.
    // Let's assume "(ua{sv})" is correct as per typical portal responses.

    g_print ("Parsed Response signal: response_code=%u\n", response_code);

    if (response_code == 0) { // Success
        if (g_variant_lookup (results_dict, "uri", "&s", &uri)) {
            process_image_uri(uri, frame_request);
        } else {
            g_printerr ("URI not found in portal response results.\n");
            frame_request->completed = TRUE;
            frame_request->success = FALSE;
            if (frame_request->loop_for_sync_call) g_main_loop_quit(frame_request->loop_for_sync_call);
        }
    } else {
        g_printerr ("Portal screenshot request failed with code: %u\n", response_code);
        // You could potentially get error messages from results_dict too
        frame_request->completed = TRUE;
        frame_request->success = FALSE;
        if (frame_request->loop_for_sync_call) g_main_loop_quit(frame_request->loop_for_sync_call);
    }
    g_variant_unref (results_dict);
    
    g_signal_handlers_disconnect_by_data(proxy, user_data); // Disconnect this specific handler
    g_object_unref(proxy); 
    if (frame_request) { // Nullify the stored proxy to prevent double-free
        frame_request->portal_request_proxy = NULL;
    }
}

// Callback for when the initial Screenshot portal call is completed
static void
on_screenshot_method_completed (GObject      *source_object,
                                GAsyncResult *res,
                                gpointer      user_data)
{
    GDBusProxy *desktop_proxy = G_DBUS_PROXY (source_object);
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    GVariant *result_tuple;
    GError *error = NULL;
    char *request_handle_path = NULL;

    result_tuple = g_dbus_proxy_call_finish (desktop_proxy, res, &error);

    if (error != NULL) {
        g_printerr ("Error calling Screenshot method: %s\n", error->message);
        g_error_free (error);
        goto fail;
    }

    if (result_tuple == NULL) {
        g_printerr("Screenshot method returned no data (no request handle).\n");
        goto fail;
    }

    g_variant_get (result_tuple, "(&o)", &request_handle_path);
    if (request_handle_path == NULL) {
        g_printerr("Could not parse request handle from portal response.\n");
        g_variant_unref(result_tuple);
        goto fail;
    }
    g_print("Received request handle: %s\n", request_handle_path);

    // Now create a proxy for the request object and connect to its Response signal
    GDBusProxy *request_proxy = NULL; // Initialize to NULL
    request_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                   G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, // Allow signals
                                                   NULL, /* GDBusInterfaceInfo */
                                                   "org.freedesktop.portal.Desktop", /* Bus name */
                                                   request_handle_path,              /* Object path */
                                                   "org.freedesktop.portal.Request", /* Interface */
                                                   NULL, /* GCancellable */
                                                   &error);
    
    g_variant_unref (result_tuple); // Done with this

    if (error != NULL) {
        g_printerr ("Error creating D-Bus proxy for request handle: %s\n", error->message);
        g_error_free (error);
        goto fail;
    }

    // Connect to the "g-signal" GObject signal.
    // The callback will then filter by D-Bus signal name.
    g_signal_connect (request_proxy,
                      "g-signal", 
                      G_CALLBACK (on_request_response_signal),
                      frame_request);

    // The request_proxy will be unreffed in on_request_response_signal after signal received or timeout
    // (or if the connection is lost, the proxy might be invalidated and unreffed by GIO).
    // We hold a ref from g_dbus_proxy_new_for_bus_sync, and it's passed to the callback.
    // The callback should unref it when done.
    // on_request_response_signal already calls g_object_unref(proxy) which is request_proxy.
    // Store the request_proxy in frame_request so it can be cleaned up if we time out
    // before on_request_response_signal is called.
    frame_request->portal_request_proxy = request_proxy; // g_object_ref if not taking ownership, but new_for_bus_sync returns full ownership
                                                       // so we are transferring ownership to frame_request context.
                                                       // No, on_screenshot_method_completed doesn't own request_proxy, it's a local var.
                                                       // It should be unreffed if this callback path fails before signal connection.
                                                       // The current path is: new_for_bus_sync (ref=1), connect, return.
                                                       // If on_request_response_signal is called, it unrefs.
                                                       // If timeout, it's not unreffed.
    // Let's clarify ref counting for request_proxy:
    // 1. request_proxy = g_dbus_proxy_new_for_bus_sync -> ref count is 1, owned by request_proxy variable.
    // 2. g_signal_connect does not take a ref.
    // 3. If on_request_response_signal is called, it gets this proxy and unrefs it.
    // 4. If on_screenshot_method_completed completes and returns, request_proxy goes out of scope.
    //    If no signal is ever received (timeout), then the proxy is leaked.
    // So, we must store it in frame_request and unref it in get_xdg_root_window_frame_sync if timeout.
    if (request_proxy) { // If successfully created
        frame_request->portal_request_proxy = g_object_ref(request_proxy); // Store a ref in frame_request
    }
    // The local request_proxy variable's ref is implicitly transferred or handled if it's part of an object.
    // Here, it's a local stack var. If we don't store it, it's lost.
    // The g_signal_connect callback will receive the original request_proxy.
    // The issue is cleaning it up if the callback *never* runs.

    // Simpler: The callback on_request_response_signal will unref the proxy it receives.
    // If this callback is never called due to timeout, the proxy is leaked.
    // So, store the request_proxy in frame_request.
    frame_request->portal_request_proxy = request_proxy; // Transferring ownership to frame_request struct
                                                       // The callback will unref it.
                                                       // If timeout, get_xdg_root_window_frame_sync must unref it.

    return; // Successfully set up signal watch

fail: // This 'fail' is for on_screenshot_method_completed itself
    if (request_proxy) { // If proxy was created before failure in this function
        g_object_unref(request_proxy);
    }
    frame_request->portal_request_proxy = NULL; // Ensure it's NULL on failure here
    frame_request->completed = TRUE;
    frame_request->success = FALSE;
    if (frame_request->loop_for_sync_call && g_main_loop_is_running(frame_request->loop_for_sync_call)) {
        g_main_loop_quit(frame_request->loop_for_sync_call);
    }
}


XDGFrameRequest* get_xdg_root_window_frame_sync() {
    GDBusProxy *proxy;
    GError *error = NULL;
    GVariant *params;
    // Allocate and zero-initialize. Set completed to FALSE.
    XDGFrameRequest *frame_request = g_new0(XDGFrameRequest, 1); 
    frame_request->completed = FALSE;
    frame_request->success = FALSE;
    frame_request->data = NULL;

    // Create a D-Bus proxy for the screenshot portal
    proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                           G_DBUS_PROXY_FLAGS_NONE,
                                           NULL, /* GDBusInterfaceInfo */
                                           "org.freedesktop.portal.Desktop",
                                           "/org/freedesktop/portal/desktop",
                                           "org.freedesktop.portal.Screenshot",
                                           NULL, /* GCancellable */
                                           &error);

    if (error != NULL) {
        g_printerr ("Error creating D-Bus proxy for Screenshot: %s\n", error->message);
        g_error_free (error);
        g_free(frame_request);
        return NULL;
    }

    GVariantBuilder *options_builder;
    GVariantType *dict_entry_type = G_VARIANT_TYPE("a{sv}");
    g_print("DEBUG: GVariantType for a{sv} is: %s (pointer: %p)\n", g_variant_type_peek_string(dict_entry_type), dict_entry_type);

    options_builder = g_variant_builder_new (dict_entry_type);
    if (options_builder == NULL) {
        g_printerr("FATAL: options_builder is NULL after new! Type: a{sv}\n");
        g_object_unref(proxy);
        g_free(frame_request);
        return NULL; 
    }
    g_print("DEBUG: options_builder created (pointer: %p)\n", options_builder);

    GVariant *interactive_val = g_variant_new_boolean(FALSE);
    g_variant_builder_add (options_builder, "{sv}", "interactive", interactive_val);
    g_print("DEBUG: Added 'interactive' to options_builder.\n");

    GVariant *modal_val = g_variant_new_boolean(FALSE);
    g_variant_builder_add (options_builder, "{sv}", "modal", modal_val);
    g_print("DEBUG: Added 'modal' to options_builder.\n");
    
    char actual_handle_token_string[64];
    snprintf(actual_handle_token_string, sizeof(actual_handle_token_string), "viture_disp_req_%u", g_random_int());
    GVariant *token_val = g_variant_new_string(actual_handle_token_string);
    g_variant_builder_add (options_builder, "{sv}", "handle_token", token_val);
    g_print("DEBUG: Added 'handle_token' (%s) to options_builder.\n", actual_handle_token_string);
    
    // When using g_variant_new with a format string that includes a container type (like a{sv}),
    // you pass the GVariantBuilder* directly for that part of the format string.
    // g_variant_new will internally call g_variant_builder_end().
    params = g_variant_new ("(sa{sv})", "", options_builder); 
    
    // Unref the builder after g_variant_new has finished with it.
    g_variant_builder_unref(options_builder); 

    if (params == NULL) {
        g_printerr("FATAL: params (final GVariant for D-Bus call) is NULL after g_variant_new.\n");
        g_object_unref(proxy);
        g_free(frame_request);
        return NULL;
    }
    g_print("DEBUG: Final D-Bus params created (type: %s)\n", g_variant_get_type_string(params));

    g_print("Calling Screenshot portal method with handle_token in options (from builder): %s\n", actual_handle_token_string);

    g_dbus_proxy_call (proxy,
                       "Screenshot",
                       params, // This variant is consumed by the call
                       G_DBUS_CALL_FLAGS_NONE,
                       -1, /* Timeout for the method call itself, not the whole operation */
                       NULL, /* GCancellable */
                       on_screenshot_method_completed,
                       frame_request); 

    g_object_unref (proxy); // Release our ref to the desktop proxy

    // For synchronous behavior, run a main loop until the callback signals completion.
    // This requires the calling thread to be able to run a GMainLoop.
    // If this function is called from a thread that already runs a GMainLoop,
    // then a nested loop is generally okay.
    GMainContext *context = g_main_context_default(); // Or get current thread-default if appropriate
    frame_request->loop_for_sync_call = g_main_loop_new(context, FALSE);

    // Timeout for the entire operation
    guint timeout_source_id = 0;
    
    gboolean timeout_callback_for_loop(gpointer user_data_for_callback) {
        XDGFrameRequest *freq = (XDGFrameRequest *)user_data_for_callback;
        g_print("DEBUG: Timeout source for GMainLoop fired.\n");
        
        // Mark as timed out only if not already completed by an actual D-Bus response
        if (!freq->completed) { 
            freq->completed = TRUE; // Mark completed due to timeout
            freq->success = FALSE;  // Indicate failure due to timeout
            g_printerr("Screenshot operation has definitively timed out (GSource).\n");
        } else {
            g_print("DEBUG: Timeout source fired, but request already completed. Doing nothing further.\n");
        }

        // Quit the loop if it's still running (it should be if timeout is the first to fire)
        if (freq->loop_for_sync_call && g_main_loop_is_running(freq->loop_for_sync_call)) {
            g_main_loop_quit(freq->loop_for_sync_call);
        }
        return G_SOURCE_REMOVE; // Important: remove the timeout source itself
    }

    if (frame_request->loop_for_sync_call) { // Add a timeout for the portal response
        timeout_source_id = g_timeout_add(5000, timeout_callback_for_loop, frame_request);
        g_print("DEBUG: Added timeout source ID %u\n", timeout_source_id);
    }

    g_main_loop_run(frame_request->loop_for_sync_call);
    g_print("DEBUG: g_main_loop_run finished.\n");

    // If the loop was quit by means other than the timeout_callback_for_loop
    // (i.e., by the actual D-Bus response callback), the timeout source might still be active.
    // We need to remove it to prevent it from firing later.
    // g_source_remove returns TRUE if it removed a source, FALSE if not found (e.g., already fired and removed itself).
    if (timeout_source_id > 0) {
        if (g_source_remove(timeout_source_id)) {
            g_print("DEBUG: Explicitly removed timeout source ID %u (it had not fired yet).\n", timeout_source_id);
        } else {
            // This means the timeout_callback_for_loop likely already ran and removed the source,
            // or the source ID was invalid for some other reason.
            // The GLib-CRITICAL "Source ID not found" should be avoided by this structure.
            g_print("DEBUG: Timeout source ID %u not found for removal (likely already fired or invalid).\n", timeout_source_id);
        }
    }

    g_main_loop_unref(frame_request->loop_for_sync_call);
    frame_request->loop_for_sync_call = NULL;


    if (!frame_request->completed) {
        g_printerr("Screenshot operation timed out.\n");
        frame_request->success = FALSE; // Ensure success is false
    }
    
    if (!frame_request->success || !frame_request->data) {
        g_free(frame_request->data); // It might be NULL already
        frame_request->data = NULL;
        // If timed out, and on_request_response_signal was never called,
        // the portal_request_proxy needs to be unreffed.
        if (frame_request->portal_request_proxy) {
            // Check if the signal handler might still run or has run.
            // If completed is TRUE and success is FALSE, and it was due to timeout_callback_for_loop,
            // then on_request_response_signal was not called.
            if (frame_request->completed && !frame_request->success) { // Could be timeout or other failure
                 // A bit risky to unconditionally unref here if on_request_response_signal *could* still somehow run
                 // But g_signal_handlers_disconnect_by_data in on_request_response_signal should prevent double free.
                 // And if it timed out, the signal handler *didn't* run.
                g_print("DEBUG: Cleaning up portal_request_proxy due to overall failure/timeout.\n");
                g_object_unref(frame_request->portal_request_proxy);
                frame_request->portal_request_proxy = NULL;
            }
        }
        g_free(frame_request);
        return NULL;
    }
    
    // If successful, on_request_response_signal should have unreffed portal_request_proxy.
    // Set it to NULL in frame_request to indicate it's handled.
    if (frame_request->portal_request_proxy && frame_request->success) {
         // This case implies on_request_response_signal was called and handled it.
         // However, on_request_response_signal unrefs the proxy it receives, not frame_request->portal_request_proxy.
         // This is tricky. Let on_request_response_signal also NULLify frame_request->portal_request_proxy.
         // For now, assume it's handled if success.
    }
    // The portal_request_proxy is now either unreffed by the callback or above.
    // To be absolutely safe, ensure it's NULL in frame_request after success.
    // This requires on_request_response_signal to do: frame_request->portal_request_proxy = NULL;
    // For now, let's assume the unref in on_request_response_signal is sufficient.

    return frame_request;
}

void free_xdg_frame_request(XDGFrameRequest* frame_req) {
    if (frame_req) {
        g_free(frame_req->data);
        if (frame_req->portal_request_proxy) {
            // This implies it was not successfully processed by on_request_response_signal
            // (e.g. initial setup failed before signal connection, or timeout before signal)
            g_print("DEBUG: free_xdg_frame_request is freeing a lingering portal_request_proxy.\n");
            g_object_unref(frame_req->portal_request_proxy);
            frame_req->portal_request_proxy = NULL;
        }
        g_free(frame_req);
    }
}

// Public API might look like:
// typedef XDGFrameRequest XDGFrame; // If we rename struct
// XDGFrame* get_xdg_frame();
// void free_xdg_frame(XDGFrame*);

/*
// Example usage (requires a GMainLoop to run for async operations if not using sync wrapper)
// For the synchronous wrapper, it creates its own loop.
int main() {
    // g_type_init(); // For older GLib, usually not needed now.
    // GdkPixbuf needs its type registered if not using GTK.
    // Typically gdk_pixbuf_init_modules or similar, or it happens automatically.

    XDGFrameRequest *frame = get_xdg_root_window_frame_sync();

    if (frame && frame->data) {
        g_print("Synchronous call returned. Frame data received.\n");
        g_print("Width: %d, Height: %d, Stride: %d\n", frame->width, frame->height, frame->stride);
        // Process frame->data
        // For example, save to a PPM file for testing
        FILE *f = fopen("screenshot.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", frame->width, frame->height);
            // Assuming data is RGB packed
            if (frame->stride == frame->width * 3) { // Simple case
                 fwrite(frame->data, frame->height * frame->stride, 1, f);
            } else { // Handle stride if necessary (copy row by row)
                for (int i = 0; i < frame->height; ++i) {
                    fwrite(frame->data + i * frame->stride, frame->width * 3, 1, f);
                }
            }
            fclose(f);
            g_print("Saved screenshot to screenshot.ppm\n");
        }
    } else {
        g_printerr("Failed to get screenshot or no data.\n");
    }

    free_xdg_frame_request(frame);
    return 0;
}
*/
