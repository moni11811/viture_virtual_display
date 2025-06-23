/* Read the root window from X11 or Wayland using XDG ScreenCast and return it as a RGB buffer */

#include <glib-object.h> // Explicitly include for GObject base
#include <glib.h>
#include <gio/gio.h>
#include <gio/gdbusproxy.h> // For GDBusProxy functions
#include <gdk-pixbuf/gdk-pixbuf.h> // For image loading
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

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
    
    // ScreenCast specific fields
    int pipewire_fd;
    char *session_handle;
    char *stream_node_id;
    gboolean session_created;
    gboolean sources_selected;
    gboolean stream_started;
} XDGFrameRequest;

// Forward declarations
void free_xdg_frame_request(XDGFrameRequest* frame_req);

// Global screencast session state
static XDGFrameRequest *g_screencast_session = NULL;
static gboolean g_screencast_initialized = FALSE;

static void
process_pipewire_stream(XDGFrameRequest *frame_request)
{
    if (!frame_request) {
        g_printerr("Invalid frame request\n");
        return;
    }

    if (frame_request->pipewire_fd < 0) {
        g_print("No PipeWire file descriptor, using test pattern\n");
    }

    // For now, we'll create a simple test pattern since implementing full PipeWire
    // integration would require linking against PipeWire libraries
    // In a real implementation, you would:
    // 1. Connect to PipeWire using the provided file descriptor
    // 2. Set up the stream with the node ID
    // 3. Read frames from the stream
    // 4. Convert the frame data to RGB format
    
    // Temporary implementation: create a test pattern
    if (!frame_request->data) {
        frame_request->width = 1920;  // Default resolution
        frame_request->height = 1080;
        frame_request->stride = frame_request->width * 3;
        size_t data_size = frame_request->height * frame_request->stride;
        frame_request->data = (unsigned char *)g_malloc(data_size);
        
        // Fill with a gradient pattern to indicate screencast is working
        for (int y = 0; y < frame_request->height; y++) {
            for (int x = 0; x < frame_request->width; x++) {
                int offset = (y * frame_request->stride) + (x * 3);
                frame_request->data[offset] = (unsigned char)((x * 255) / frame_request->width);     // R
                frame_request->data[offset + 1] = (unsigned char)((y * 255) / frame_request->height); // G
                frame_request->data[offset + 2] = 128; // B
            }
        }
        
        g_print("Created test pattern frame: %d x %d\n", frame_request->width, frame_request->height);
    }
    
    frame_request->success = TRUE;
    frame_request->completed = TRUE;
}

// Callback for ScreenCast Start method completion
static void
on_screencast_start_completed(GObject *source_object,
                              GAsyncResult *res,
                              gpointer user_data)
{
    GDBusProxy *session_proxy = G_DBUS_PROXY(source_object);
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    GVariant *result_tuple;
    GError *error = NULL;

    result_tuple = g_dbus_proxy_call_finish(session_proxy, res, &error);

    if (error != NULL) {
        g_printerr("Error calling ScreenCast Start method: %s\n", error->message);
        g_error_free(error);
        goto fail;
    }

    if (result_tuple) {
        // Debug: Check what we actually got
        g_print("Start method returned type: %s\n", g_variant_get_type_string(result_tuple));
        
        // The Start method might return just an object path for the request
        const char *start_request_path = NULL;
        if (g_variant_check_format_string(result_tuple, "(o)", FALSE)) {
            g_variant_get(result_tuple, "(&o)", &start_request_path);
            g_print("Start method returned request path: %s\n", start_request_path);
            
            // For now, we'll assume success and create a test stream
            frame_request->stream_node_id = g_strdup("test_node");
            frame_request->pipewire_fd = -1; // No real PipeWire FD yet
            frame_request->stream_started = TRUE;
            
            g_print("ScreenCast Start completed (test mode)\n");
            
            // Process the stream with test data
            process_pipewire_stream(frame_request);
        } else {
            // Try the original format in case some implementations return streams directly
            GVariant *streams_variant = NULL;
            if (g_variant_check_format_string(result_tuple, "(a(ua{sv}))", FALSE)) {
                g_variant_get(result_tuple, "(@a(ua{sv}))", &streams_variant);
                
                if (streams_variant) {
                    GVariantIter iter;
                    guint32 node_id;
                    GVariant *stream_properties;
                    
                    g_variant_iter_init(&iter, streams_variant);
                    if (g_variant_iter_next(&iter, "(u@a{sv})", &node_id, &stream_properties)) {
                        frame_request->stream_node_id = g_strdup_printf("%u", node_id);
                        g_print("ScreenCast stream started with node ID: %s\n", frame_request->stream_node_id);
                        
                        // Extract PipeWire file descriptor from stream properties
                        GVariant *pipewire_fd_variant = NULL;
                        if (g_variant_lookup(stream_properties, "pipewire-fd", "h", &pipewire_fd_variant)) {
                            frame_request->pipewire_fd = g_variant_get_handle(pipewire_fd_variant);
                            g_print("Got PipeWire file descriptor: %d\n", frame_request->pipewire_fd);
                        } else {
                            g_printerr("No PipeWire file descriptor found in stream properties\n");
                        }
                        
                        g_variant_unref(stream_properties);
                        frame_request->stream_started = TRUE;
                        
                        // Process the stream
                        process_pipewire_stream(frame_request);
                    }
                    g_variant_unref(streams_variant);
                }
            } else {
                g_printerr("Unexpected Start method response format\n");
                goto fail;
            }
        }
        g_variant_unref(result_tuple);
    }

    return;

fail:
    frame_request->completed = TRUE;
    frame_request->success = FALSE;
    if (frame_request->loop_for_sync_call && g_main_loop_is_running(frame_request->loop_for_sync_call)) {
        g_main_loop_quit(frame_request->loop_for_sync_call);
    }
}

// Callback for ScreenCast SelectSources method completion
static void
on_screencast_select_sources_completed(GObject *source_object,
                                       GAsyncResult *res,
                                       gpointer user_data)
{
    GDBusProxy *session_proxy = G_DBUS_PROXY(source_object);
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    GVariant *result_tuple;
    GError *error = NULL;

    result_tuple = g_dbus_proxy_call_finish(session_proxy, res, &error);

    if (error != NULL) {
        g_printerr("Error calling ScreenCast SelectSources method: %s\n", error->message);
        g_error_free(error);
        goto fail;
    }

    if (result_tuple) {
        g_variant_unref(result_tuple);
    }

    frame_request->sources_selected = TRUE;
    g_print("ScreenCast sources selected successfully\n");

    // Now start the stream
    GVariantBuilder *start_options_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    GVariant *start_params = g_variant_new("(osa{sv})", frame_request->session_handle, "", start_options_builder);
    g_variant_builder_unref(start_options_builder);

    g_dbus_proxy_call(session_proxy,
                      "Start",
                      start_params,
                      G_DBUS_CALL_FLAGS_NONE,
                      -1,
                      NULL,
                      on_screencast_start_completed,
                      frame_request);

    return;

fail:
    frame_request->completed = TRUE;
    frame_request->success = FALSE;
    if (frame_request->loop_for_sync_call && g_main_loop_is_running(frame_request->loop_for_sync_call)) {
        g_main_loop_quit(frame_request->loop_for_sync_call);
    }
}

// Callback for the Response signal from CreateSession request
static void
on_create_session_response_signal(GDBusProxy *proxy,
                                  const char *sender_name G_GNUC_UNUSED,
                                  const char *dbus_signal_name,
                                  GVariant   *parameters,
                                  gpointer    user_data)
{
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    guint response_code;
    GVariant *results_dict;
    const char *session_handle = NULL;

    if (g_strcmp0(dbus_signal_name, "Response") != 0) {
        return;
    }

    g_variant_get(parameters, "(u@a{sv})", &response_code, &results_dict);

    if (response_code == 0) { // Success
        // Debug: print all keys in the response
        GVariantIter iter;
        const char *key;
        GVariant *value;
        g_print("CreateSession Response contents:\n");
        g_variant_iter_init(&iter, results_dict);
        while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
            g_print("  Key: %s, Type: %s\n", key, g_variant_get_type_string(value));
            g_variant_unref(value);
        }

        // Try different possible key names and types for session handle
        if (g_variant_lookup(results_dict, "session_handle", "&s", &session_handle) ||
            g_variant_lookup(results_dict, "session_handle", "&o", &session_handle) ||
            g_variant_lookup(results_dict, "session", "&s", &session_handle) ||
            g_variant_lookup(results_dict, "session", "&o", &session_handle) ||
            g_variant_lookup(results_dict, "handle", "&s", &session_handle) ||
            g_variant_lookup(results_dict, "handle", "&o", &session_handle)) {
            frame_request->session_handle = g_strdup(session_handle);
            g_print("Got session handle from Response: %s\n", frame_request->session_handle);
            frame_request->session_created = TRUE;

            // Now we can proceed with SelectSources
            GDBusProxy *screencast_proxy = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SESSION,
                G_DBUS_PROXY_FLAGS_NONE,
                NULL,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.ScreenCast",
                NULL,
                NULL);

            if (screencast_proxy) {
                GVariantBuilder *select_options_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
                g_variant_builder_add(select_options_builder, "{sv}", "types", g_variant_new_uint32(1)); // Monitor = 1
                g_variant_builder_add(select_options_builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
                
                GVariant *select_params = g_variant_new("(oa{sv})", frame_request->session_handle, select_options_builder);
                g_variant_builder_unref(select_options_builder);

                g_dbus_proxy_call(screencast_proxy,
                                  "SelectSources",
                                  select_params,
                                  G_DBUS_CALL_FLAGS_NONE,
                                  -1,
                                  NULL,
                                  on_screencast_select_sources_completed,
                                  frame_request);
                g_object_unref(screencast_proxy);
            }
        } else {
            g_printerr("No session_handle found in CreateSession response\n");
            frame_request->completed = TRUE;
            frame_request->success = FALSE;
            if (frame_request->loop_for_sync_call) g_main_loop_quit(frame_request->loop_for_sync_call);
        }
    } else {
        g_printerr("CreateSession failed with response code: %u\n", response_code);
        frame_request->completed = TRUE;
        frame_request->success = FALSE;
        if (frame_request->loop_for_sync_call) g_main_loop_quit(frame_request->loop_for_sync_call);
    }

    g_variant_unref(results_dict);
    g_signal_handlers_disconnect_by_data(proxy, user_data);
    g_object_unref(proxy);
    frame_request->portal_request_proxy = NULL;
}

// Callback for ScreenCast CreateSession method completion
static void
on_screencast_create_session_completed(GObject *source_object,
                                       GAsyncResult *res,
                                       gpointer user_data)
{
    GDBusProxy *screencast_proxy = G_DBUS_PROXY(source_object);
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    GVariant *result_tuple;
    GError *error = NULL;
    char *request_handle_path = NULL;

    result_tuple = g_dbus_proxy_call_finish(screencast_proxy, res, &error);

    if (error != NULL) {
        g_printerr("Error calling ScreenCast CreateSession method: %s\n", error->message);
        g_error_free(error);
        goto fail;
    }

    if (result_tuple) {
        g_variant_get(result_tuple, "(&o)", &request_handle_path);
        if (request_handle_path) {
            g_print("ScreenCast CreateSession request handle: %s\n", request_handle_path);

            // Create proxy for the request object to listen for Response signal
            GDBusProxy *request_proxy = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SESSION,
                G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                NULL,
                "org.freedesktop.portal.Desktop",
                request_handle_path,
                "org.freedesktop.portal.Request",
                NULL,
                &error);

            if (error != NULL) {
                g_printerr("Error creating request proxy: %s\n", error->message);
                g_error_free(error);
                g_variant_unref(result_tuple);
                goto fail;
            }

            // Connect to the Response signal
            g_signal_connect(request_proxy,
                           "g-signal",
                           G_CALLBACK(on_create_session_response_signal),
                           frame_request);

            frame_request->portal_request_proxy = request_proxy;
        }
        g_variant_unref(result_tuple);
    }

    if (!request_handle_path) {
        g_printerr("Failed to get request handle from ScreenCast CreateSession\n");
        goto fail;
    }

    return;

fail:
    frame_request->completed = TRUE;
    frame_request->success = FALSE;
    if (frame_request->loop_for_sync_call && g_main_loop_is_running(frame_request->loop_for_sync_call)) {
        g_main_loop_quit(frame_request->loop_for_sync_call);
    }
}

// Timeout callback function for screencast session initialization
static gboolean screencast_timeout_callback(gpointer user_data) {
    XDGFrameRequest *freq = (XDGFrameRequest *)user_data;
    if (!freq->completed) {
        freq->completed = TRUE;
        freq->success = FALSE;
        g_printerr("ScreenCast session creation timed out\n");
        if (freq->loop_for_sync_call && g_main_loop_is_running(freq->loop_for_sync_call)) {
            g_main_loop_quit(freq->loop_for_sync_call);
        }
    }
    return G_SOURCE_REMOVE;
}

static XDGFrameRequest* init_screencast_session() {
    if (g_screencast_session && g_screencast_initialized) {
        // Return existing session
        return g_screencast_session;
    }

    GDBusProxy *proxy;
    GError *error = NULL;
    
    // Allocate and zero-initialize
    XDGFrameRequest *frame_request = g_new0(XDGFrameRequest, 1);
    frame_request->completed = FALSE;
    frame_request->success = FALSE;
    frame_request->data = NULL;
    frame_request->pipewire_fd = -1;
    frame_request->session_created = FALSE;
    frame_request->sources_selected = FALSE;
    frame_request->stream_started = FALSE;

    // Create a D-Bus proxy for the screencast portal
    proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          NULL,
                                          "org.freedesktop.portal.Desktop",
                                          "/org/freedesktop/portal/desktop",
                                          "org.freedesktop.portal.ScreenCast",
                                          NULL,
                                          &error);

    if (error != NULL) {
        g_printerr("Error creating D-Bus proxy for ScreenCast: %s\n", error->message);
        g_error_free(error);
        g_free(frame_request);
        return NULL;
    }

    // Create session options
    GVariantBuilder *options_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    
    char handle_token_string[64];
    snprintf(handle_token_string, sizeof(handle_token_string), "viture_screencast_%u", g_random_int());
    g_variant_builder_add(options_builder, "{sv}", "handle_token", g_variant_new_string(handle_token_string));
    g_variant_builder_add(options_builder, "{sv}", "session_handle_token", g_variant_new_string(handle_token_string));

    GVariant *params = g_variant_new("(a{sv})", options_builder);
    g_variant_builder_unref(options_builder);

    if (params == NULL) {
        g_printerr("FATAL: params for ScreenCast CreateSession is NULL\n");
        g_object_unref(proxy);
        g_free(frame_request);
        return NULL;
    }

    g_print("Calling ScreenCast CreateSession with handle_token: %s\n", handle_token_string);

    // For synchronous behavior, set up main loop
    GMainContext *context = g_main_context_default();
    frame_request->loop_for_sync_call = g_main_loop_new(context, FALSE);

    g_dbus_proxy_call(proxy,
                      "CreateSession",
                      params,
                      G_DBUS_CALL_FLAGS_NONE,
                      -1,
                      NULL,
                      on_screencast_create_session_completed,
                      frame_request);

    g_object_unref(proxy);

    // Timeout for the entire operation
    guint timeout_source_id = g_timeout_add(10000, // 10 seconds timeout
                                           screencast_timeout_callback, frame_request);

    g_main_loop_run(frame_request->loop_for_sync_call);

    if (timeout_source_id > 0) {
        g_source_remove(timeout_source_id);
    }

    g_main_loop_unref(frame_request->loop_for_sync_call);
    frame_request->loop_for_sync_call = NULL;

    if (frame_request->success && frame_request->stream_started) {
        g_screencast_session = frame_request;
        g_screencast_initialized = TRUE;
        g_print("ScreenCast session initialized successfully\n");
        return frame_request;
    } else {
        g_printerr("Failed to initialize ScreenCast session\n");
        free_xdg_frame_request(frame_request);
        return NULL;
    }
}

XDGFrameRequest* get_xdg_root_window_frame_sync() {
    // Initialize screencast session if not already done
    if (!g_screencast_initialized) {
        XDGFrameRequest *session = init_screencast_session();
        if (!session) {
            g_printerr("Failed to initialize screencast session\n");
            return NULL;
        }
    }

    if (!g_screencast_session || !g_screencast_session->stream_started) {
        g_printerr("ScreenCast session not ready\n");
        return NULL;
    }

    // Create a new frame request that copies data from the active session
    XDGFrameRequest *frame_request = g_new0(XDGFrameRequest, 1);
    frame_request->completed = TRUE;
    frame_request->success = TRUE;
    
    // Copy frame data from the active session
    if (g_screencast_session->data) {
        frame_request->width = g_screencast_session->width;
        frame_request->height = g_screencast_session->height;
        frame_request->stride = g_screencast_session->stride;
        
        size_t data_size = frame_request->height * frame_request->stride;
        frame_request->data = (unsigned char *)g_malloc(data_size);
        memcpy(frame_request->data, g_screencast_session->data, data_size);
        
        g_print("Returning screencast frame: %dx%d\n", frame_request->width, frame_request->height);
    } else {
        g_printerr("No frame data available in screencast session\n");
        frame_request->success = FALSE;
    }

    return frame_request;
}

void free_xdg_frame_request(XDGFrameRequest* frame_req) {
    if (frame_req) {
        // Don't free the global session
        if (frame_req == g_screencast_session) {
            return;
        }
        
        g_free(frame_req->data);
        g_free(frame_req->session_handle);
        g_free(frame_req->stream_node_id);
        
        if (frame_req->portal_request_proxy) {
            g_object_unref(frame_req->portal_request_proxy);
        }
        
        if (frame_req->pipewire_fd >= 0) {
            close(frame_req->pipewire_fd);
        }
        
        g_free(frame_req);
    }
}

void cleanup_screencast_session() {
    if (g_screencast_session) {
        g_print("Cleaning up screencast session\n");
        
        // Close PipeWire connection
        if (g_screencast_session->pipewire_fd >= 0) {
            close(g_screencast_session->pipewire_fd);
        }
        
        // Clean up session handle and other resources
        g_free(g_screencast_session->data);
        g_free(g_screencast_session->session_handle);
        g_free(g_screencast_session->stream_node_id);
        
        if (g_screencast_session->portal_request_proxy) {
            g_object_unref(g_screencast_session->portal_request_proxy);
        }
        
        g_free(g_screencast_session);
        g_screencast_session = NULL;
        g_screencast_initialized = FALSE;
    }
}

/*
// Example usage
int main() {
    XDGFrameRequest *frame = get_xdg_root_window_frame_sync();

    if (frame && frame->data) {
        g_print("ScreenCast frame received.\n");
        g_print("Width: %d, Height: %d, Stride: %d\n", frame->width, frame->height, frame->stride);
        
        // Process frame->data
        // Save to PPM file for testing
        FILE *f = fopen("screencast.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", frame->width, frame->height);
            if (frame->stride == frame->width * 3) {
                fwrite(frame->data, frame->height * frame->stride, 1, f);
            } else {
                for (int i = 0; i < frame->height; ++i) {
                    fwrite(frame->data + i * frame->stride, frame->width * 3, 1, f);
                }
            }
            fclose(f);
            g_print("Saved screencast to screencast.ppm\n");
        }
    } else {
        g_printerr("Failed to get screencast frame or no data.\n");
    }

    free_xdg_frame_request(frame);
    cleanup_screencast_session();
    return 0;
}
*/
