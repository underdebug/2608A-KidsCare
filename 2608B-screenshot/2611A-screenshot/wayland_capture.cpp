#include "wayland_capture.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCastInterface = "org.freedesktop.portal.ScreenCast";

std::string token_path() {
    return std::string(g_get_user_data_dir()) + "/screenshot-sender/restore-token";
}

std::string load_restore_token() {
    gchar* contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(token_path().c_str(), &contents, &length, nullptr)) return {};
    std::string token(contents, length);
    g_free(contents);
    return token;
}

void save_restore_token(const char* token) {
    const std::string path = token_path();
    gchar* directory = g_path_get_dirname(path.c_str());
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        std::cerr << "capture: cannot create restore token directory\n";
        g_free(directory);
        return;
    }
    g_free(directory);
    GError* error = nullptr;
    if (!g_file_set_contents_full(path.c_str(), token, -1,
                                  G_FILE_SET_CONTENTS_CONSISTENT,
                                  0600, &error)) {
        std::cerr << "capture: cannot save restore token: " << error->message << "\n";
        g_error_free(error);
    }
}

struct RequestResult {
    GMainLoop* loop = nullptr;
    guint response = 2;
    GVariant* details = nullptr;
    bool received = false;
};

struct FrameState {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    std::mutex mutex;
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    spa_video_format format = SPA_VIDEO_FORMAT_UNKNOWN;
    std::string error;
    bool ready = false;
};

void on_portal_response(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                       const gchar*, GVariant* parameters, gpointer user_data) {
    auto* result = static_cast<RequestResult*>(user_data);
    g_variant_get(parameters, "(u@a{sv})", &result->response, &result->details);
    result->received = true;
    g_main_loop_quit(result->loop);
}

bool portal_request(GDBusConnection* connection, const char* method,
                    const char* token, GVariant* parameters, GVariant** details) {
    const gchar* unique_name = g_dbus_connection_get_unique_name(connection);
    std::string sender(unique_name ? unique_name : "");
    if (!sender.empty() && sender.front() == ':') sender.erase(sender.begin());
    std::replace(sender.begin(), sender.end(), '.', '_');
    std::string request_path = "/org/freedesktop/portal/desktop/request/" +
                               sender + "/" + token;

    RequestResult result;
    result.loop = g_main_loop_new(nullptr, FALSE);
    guint subscription = g_dbus_connection_signal_subscribe(
        connection, kPortalName, "org.freedesktop.portal.Request", "Response",
        request_path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        on_portal_response, &result, nullptr);

    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection, kPortalName, kPortalPath, kScreenCastInterface, method,
        parameters, G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 30000,
        nullptr, &error);
    bool ok = reply != nullptr;
    if (reply) {
        const gchar* returned_path = nullptr;
        g_variant_get(reply, "(&o)", &returned_path);
        if (request_path != returned_path) {
            std::cerr << "capture: portal returned an unexpected request path\n";
            ok = false;
        }
        g_variant_unref(reply);
    } else {
        std::cerr << "capture: ScreenCast " << method << " failed: "
                  << (error ? error->message : "unknown D-Bus error") << "\n";
    }
    if (error) g_error_free(error);

    if (ok) {
        GSource* timeout = g_timeout_source_new_seconds(120);
        g_source_set_callback(timeout, [](gpointer data) -> gboolean {
            auto* state = static_cast<RequestResult*>(data);
            g_main_loop_quit(state->loop);
            return G_SOURCE_REMOVE;
        }, &result, nullptr);
        g_source_attach(timeout, nullptr);
        g_main_loop_run(result.loop);
        g_source_destroy(timeout);
        g_source_unref(timeout);
        ok = result.received && result.response == 0;
        if (!ok) {
            std::cerr << "capture: ScreenCast " << method
                      << " was denied, cancelled, or timed out\n";
        } else {
            *details = g_variant_ref(result.details);
        }
    }

    g_dbus_connection_signal_unsubscribe(connection, subscription);
    if (result.details) g_variant_unref(result.details);
    g_main_loop_unref(result.loop);
    return ok;
}

std::string make_token() {
    gchar* uuid = g_uuid_string_random();
    std::string token(uuid);
    g_free(uuid);
    std::replace(token.begin(), token.end(), '-', '_');
    return token;
}

bool get_object_path(GVariant* details, const char* key, std::string& path) {
    GVariant* value = g_variant_lookup_value(details, key, nullptr);
    if (!value) return false;

    bool ok = g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH) ||
              g_variant_is_of_type(value, G_VARIANT_TYPE_STRING);
    if (ok) path = g_variant_get_string(value, nullptr);
    g_variant_unref(value);
    return ok;
}

void on_stream_state_changed(void* data, pw_stream_state, pw_stream_state state,
                             const char* error) {
    auto* frame = static_cast<FrameState*>(data);
    if (state == PW_STREAM_STATE_ERROR) {
        std::lock_guard<std::mutex> lock(frame->mutex);
        frame->error = error ? error : "PipeWire stream failed";
    }
}

void on_stream_param_changed(void* data, uint32_t id, const spa_pod* param) {
    auto* frame = static_cast<FrameState*>(data);
    if (id != SPA_PARAM_Format || !param) return;

    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(param, &info) < 0) return;
    std::lock_guard<std::mutex> lock(frame->mutex);
    if (info.size.width == 0 || info.size.height == 0 ||
        info.size.width > 16384 || info.size.height > 16384 ||
        static_cast<uint64_t>(info.size.width) * info.size.height > 100000000) {
        frame->error = "PipeWire returned invalid frame dimensions";
        return;
    }
    frame->width = static_cast<int>(info.size.width);
    frame->height = static_cast<int>(info.size.height);
    frame->format = info.format;
}

void on_stream_process(void* data) {
    auto* frame = static_cast<FrameState*>(data);
    pw_buffer* queued = pw_stream_dequeue_buffer(frame->stream);
    if (!queued) return;

    spa_buffer* buffer = queued->buffer;
    if (!buffer || buffer->n_datas == 0) {
        pw_stream_queue_buffer(frame->stream, queued);
        return;
    }

    spa_data* plane = &buffer->datas[0];
    spa_chunk* chunk = plane->chunk;
    bool invalid = !plane->data || !chunk || chunk->stride <= 0;
    {
        std::lock_guard<std::mutex> lock(frame->mutex);
        if (!frame->ready && invalid && frame->error.empty()) {
            frame->error = "PipeWire returned an invalid video buffer";
        }
        if (!frame->ready && !invalid && frame->width > 0 && frame->height > 0) {
            const size_t row_bytes = static_cast<size_t>(frame->width) * 4;
            const size_t stride = static_cast<size_t>(chunk->stride);
            const size_t offset = chunk->offset;
            const size_t required = offset + (static_cast<size_t>(frame->height) - 1) *
                                    stride + row_bytes;
            if (stride < row_bytes || required > plane->maxsize ||
                chunk->size < (static_cast<size_t>(frame->height) - 1) * stride +
                                  row_bytes) {
                frame->error = "PipeWire returned an incomplete video frame";
            } else if (frame->format != SPA_VIDEO_FORMAT_BGRx &&
                       frame->format != SPA_VIDEO_FORMAT_RGBx &&
                       frame->format != SPA_VIDEO_FORMAT_BGRA &&
                       frame->format != SPA_VIDEO_FORMAT_RGBA) {
                frame->error = "PipeWire returned an unsupported pixel format";
            } else {
                frame->rgba.resize(static_cast<size_t>(frame->width) *
                                   frame->height * 4);
                const auto* source = static_cast<const uint8_t*>(plane->data) + offset;
                for (int y = 0; y < frame->height; ++y) {
                    for (int x = 0; x < frame->width; ++x) {
                        const uint8_t* pixel = source + static_cast<size_t>(y) * stride +
                                               static_cast<size_t>(x) * 4;
                        uint8_t* output = frame->rgba.data() +
                            (static_cast<size_t>(y) * frame->width + x) * 4;
                        if (frame->format == SPA_VIDEO_FORMAT_BGRx ||
                            frame->format == SPA_VIDEO_FORMAT_BGRA) {
                            output[0] = pixel[2];
                            output[1] = pixel[1];
                            output[2] = pixel[0];
                        } else {
                            output[0] = pixel[0];
                            output[1] = pixel[1];
                            output[2] = pixel[2];
                        }
                        output[3] = (frame->format == SPA_VIDEO_FORMAT_BGRA ||
                                     frame->format == SPA_VIDEO_FORMAT_RGBA)
                                        ? pixel[3] : 255;
                    }
                }
                frame->ready = true;
            }
        }
    }
    pw_stream_queue_buffer(frame->stream, queued);
}

}  // namespace

bool capture_wayland(std::vector<uint8_t>& rgba, int& width, int& height) {
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!connection) {
        std::cerr << "capture: cannot connect to the desktop session bus: "
                  << (error ? error->message : "unknown error") << "\n";
        if (error) g_error_free(error);
        return false;
    }

    const std::string session_token = make_token();
    const std::string create_token = make_token();
    GVariantBuilder create_options;
    g_variant_builder_init(&create_options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&create_options, "{sv}", "handle_token",
                          g_variant_new_string(create_token.c_str()));
    g_variant_builder_add(&create_options, "{sv}", "session_handle_token",
                          g_variant_new_string(session_token.c_str()));
    GVariant* details = nullptr;
    bool ok = portal_request(connection, "CreateSession",
                             create_token.c_str(),
                             g_variant_new("(@a{sv})",
                                           g_variant_builder_end(&create_options)),
                             &details);

    std::string session_path;
    if (ok) {
        ok = get_object_path(details, "session_handle", session_path);
        if (!ok) {
            gchar* response = g_variant_print(details, TRUE);
            std::cerr << "capture: ScreenCast CreateSession response has no valid "
                         "session_handle: "
                      << (response ? response : "<unavailable>") << "\n";
            g_free(response);
        }
        g_variant_unref(details);
        details = nullptr;
    }

    if (ok) {
        GVariantBuilder select_options;
        g_variant_builder_init(&select_options, G_VARIANT_TYPE("a{sv}"));
        const std::string select_token = make_token();
        g_variant_builder_add(&select_options, "{sv}", "handle_token",
                              g_variant_new_string(select_token.c_str()));
        g_variant_builder_add(&select_options, "{sv}", "types", g_variant_new_uint32(1));
        g_variant_builder_add(&select_options, "{sv}", "multiple",
                              g_variant_new_boolean(FALSE));
        g_variant_builder_add(&select_options, "{sv}", "cursor_mode",
                              g_variant_new_uint32(2));
        g_variant_builder_add(&select_options, "{sv}", "persist_mode",
                              g_variant_new_uint32(2));
        const std::string restore_token = load_restore_token();
        if (!restore_token.empty()) {
            g_variant_builder_add(&select_options, "{sv}", "restore_token",
                                  g_variant_new_string(restore_token.c_str()));
        }
        ok = portal_request(connection, "SelectSources",
                            select_token.c_str(),
                            g_variant_new("(o@a{sv})", session_path.c_str(),
                                          g_variant_builder_end(&select_options)),
                            &details);
        if (details) {
            g_variant_unref(details);
            details = nullptr;
        }
    }

    if (ok) {
        GVariantBuilder start_options;
        g_variant_builder_init(&start_options, G_VARIANT_TYPE("a{sv}"));
        const std::string start_token = make_token();
        g_variant_builder_add(&start_options, "{sv}", "handle_token",
                              g_variant_new_string(start_token.c_str()));
        ok = portal_request(connection, "Start",
                            start_token.c_str(),
                            g_variant_new("(os@a{sv})", session_path.c_str(), "",
                                          g_variant_builder_end(&start_options)),
                            &details);
    }

    guint32 node_id = 0;
    if (ok) {
        // Tokens are single-use; keep the replacement returned by every Start.
        const gchar* restore_token = nullptr;
        if (g_variant_lookup(details, "restore_token", "&s", &restore_token) &&
            *restore_token) {
            save_restore_token(restore_token);
        } else {
            g_unlink(token_path().c_str());
            std::cerr << "capture: portal did not grant persistent screen sharing; "
                         "future captures may require permission again\n";
        }
        GVariant* streams = g_variant_lookup_value(details, "streams",
                                                   G_VARIANT_TYPE("a(ua{sv})"));
        GVariantIter iter;
        GVariant* properties = nullptr;
        if (!streams || !g_variant_iter_init(&iter, streams) ||
            !g_variant_iter_next(&iter, "(u@a{sv})", &node_id, &properties)) {
            std::cerr << "capture: ScreenCast did not return a PipeWire stream\n";
            ok = false;
        }
        if (properties) g_variant_unref(properties);
        if (streams) g_variant_unref(streams);
        g_variant_unref(details);
        details = nullptr;
    }

    int remote_fd = -1;
    if (ok) {
        GUnixFDList* fd_list = nullptr;
        GVariant* reply = g_dbus_connection_call_with_unix_fd_list_sync(
            connection, kPortalName, kPortalPath, kScreenCastInterface,
            "OpenPipeWireRemote", g_variant_new("(o@a{sv})", session_path.c_str(),
                                                g_variant_new_array(
                                                    G_VARIANT_TYPE("{sv}"), nullptr, 0)),
            G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 30000, nullptr,
            &fd_list, nullptr, &error);
        if (!reply) {
            std::cerr << "capture: cannot open the ScreenCast PipeWire stream: "
                      << (error ? error->message : "unknown D-Bus error") << "\n";
            if (error) {
                g_error_free(error);
                error = nullptr;
            }
            ok = false;
        } else {
            gint handle = -1;
            g_variant_get(reply, "(h)", &handle);
            remote_fd = g_unix_fd_list_get(fd_list, handle, &error);
            if (remote_fd < 0) {
                std::cerr << "capture: cannot access the PipeWire remote descriptor: "
                          << (error ? error->message : "unknown error") << "\n";
                if (error) {
                    g_error_free(error);
                    error = nullptr;
                }
                ok = false;
            }
            g_variant_unref(reply);
        }
        if (fd_list) g_object_unref(fd_list);
    }

    FrameState frame;
    pw_thread_loop* thread_loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    bool thread_started = false;
    static const pw_stream_events events = [] {
        pw_stream_events value{};
        value.version = PW_VERSION_STREAM_EVENTS;
        value.state_changed = on_stream_state_changed;
        value.param_changed = on_stream_param_changed;
        value.process = on_stream_process;
        return value;
    }();
    if (ok) {
        pw_init(nullptr, nullptr);
        thread_loop = pw_thread_loop_new("screenshot-capture", nullptr);
        context = thread_loop ? pw_context_new(pw_thread_loop_get_loop(thread_loop),
                                               nullptr, 0) : nullptr;
        if (context) {
            core = pw_context_connect_fd(context, remote_fd, nullptr, 0);
            if (core) remote_fd = -1;
        }
        if (!thread_loop || !context || !core ||
            pw_thread_loop_start(thread_loop) < 0) {
            std::cerr << "capture: failed to initialize PipeWire\n";
            ok = false;
        } else {
            thread_started = true;
            pw_thread_loop_lock(thread_loop);
            frame.loop = thread_loop;
            pw_properties* properties = pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Video",
                PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Screen",
                nullptr);
            stream = pw_stream_new(core, "screenshot-capture", properties);
            if (stream) {
                frame.stream = stream;
                pw_stream_add_listener(stream, &listener, &events, &frame);
                uint8_t pod_buffer[1024];
                spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer,
                                                               sizeof(pod_buffer));
                const spa_pod* params[] = {
                    static_cast<const spa_pod*>(spa_pod_builder_add_object(
                        &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format,
                        SPA_POD_CHOICE_ENUM_Id(4, SPA_VIDEO_FORMAT_BGRx,
                                               SPA_VIDEO_FORMAT_RGBx,
                                               SPA_VIDEO_FORMAT_BGRA,
                                               SPA_VIDEO_FORMAT_RGBA),
                        nullptr))
                };
                if (pw_stream_connect(stream, PW_DIRECTION_INPUT, node_id,
                                     static_cast<pw_stream_flags>(
                                         PW_STREAM_FLAG_AUTOCONNECT |
                                         PW_STREAM_FLAG_MAP_BUFFERS),
                                     params, 1) < 0) {
                    std::cerr << "capture: cannot connect to the PipeWire stream\n";
                    ok = false;
                }
            } else {
                std::cerr << "capture: cannot create the PipeWire stream\n";
                ok = false;
            }
            pw_thread_loop_unlock(thread_loop);
        }
    }

    if (remote_fd >= 0) close(remote_fd);
    if (ok) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(frame.mutex);
                if (frame.ready) {
                    rgba = std::move(frame.rgba);
                    width = frame.width;
                    height = frame.height;
                    break;
                }
                if (!frame.error.empty()) {
                    std::cerr << "capture: " << frame.error << "\n";
                    ok = false;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (rgba.empty()) {
            if (ok) std::cerr << "capture: timed out waiting for a PipeWire frame\n";
            ok = false;
        }
    }

    if (thread_loop && thread_started) pw_thread_loop_lock(thread_loop);
    if (thread_loop) {
        if (stream) {
            spa_hook_remove(&listener);
            pw_stream_destroy(stream);
        }
        if (core) pw_core_disconnect(core);
        if (context) pw_context_destroy(context);
        if (thread_started) {
            pw_thread_loop_unlock(thread_loop);
            pw_thread_loop_stop(thread_loop);
        }
        pw_thread_loop_destroy(thread_loop);
    } else {
        if (core) pw_core_disconnect(core);
        if (context) pw_context_destroy(context);
    }

    if (!session_path.empty()) {
        GVariant* close_reply = g_dbus_connection_call_sync(
            connection, kPortalName, session_path.c_str(),
            "org.freedesktop.portal.Session", "Close", nullptr,
            G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
        if (close_reply) g_variant_unref(close_reply);
        if (error) {
            std::cerr << "capture: could not close the ScreenCast session: "
                      << error->message << "\n";
            g_error_free(error);
        }
    }
    g_object_unref(connection);
    return ok;
}
