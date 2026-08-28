// screenshot_sender.cpp
// Capture the whole X11 multi-desktop, save it as a PNG locally every
// <save_interval> seconds, and transmit it to a remote host every
// <send_interval> seconds. Skip sending if the image is >= 99% identical to
// the last one actually sent. Fully handles Mirror Mode display
// configurations safely.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static void png_collect(void* ctx, void* data, int size) {
    auto* buf = static_cast<std::vector<uint8_t>*>(ctx);
    auto* p   = static_cast<uint8_t*>(data);
    buf->insert(buf->end(), p, p + size);
}

static int mask_shift(unsigned long mask) {
    int shift = 0;
    if (!mask) return 0;
    while (!(mask & 1)) { mask >>= 1; ++shift; }
    return shift;
}

static bool capture_screen(std::vector<uint8_t>& rgba, int& width, int& height) {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::cerr << "capture: cannot open X display (is DISPLAY set?)\n";
        return false;
    }
    Window root = DefaultRootWindow(dpy);

    int xin_screens = 0;
    int evt_base = 0, err_base = 0;   // separate out-params (was one var reused)
    XineramaScreenInfo* screens = nullptr;

    if (XineramaQueryExtension(dpy, &evt_base, &err_base) && XineramaIsActive(dpy)) {
        screens = XineramaQueryScreens(dpy, &xin_screens);
    }

    if (!screens || xin_screens == 0) {
        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy, root, &attr)) {
            std::cerr << "capture: XGetWindowAttributes failed\n";
            XCloseDisplay(dpy);
            return false;
        }
        width  = attr.width;
        height = attr.height;
    } else {
        int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
        
        // Track unique screens to filtering out clones (Mirror Mode layouts)
        std::vector<XineramaScreenInfo> unique_screens;
        
        for (int i = 0; i < xin_screens; ++i) {
            bool is_duplicate = false;
            for (const auto& uniq : unique_screens) {
                // If coordinates overlap exactly, it is a hardware mirrored replica
                if (screens[i].x_org == uniq.x_org && screens[i].y_org == uniq.y_org) {
                    is_duplicate = true;
                    break;
                }
            }
            if (!is_duplicate) {
                unique_screens.push_back(screens[i]);
            }
        }

        // Calculate bounding box using only unique screens
        for (const auto& scr : unique_screens) {
            if (scr.x_org < min_x) min_x = scr.x_org;
            if (scr.y_org < min_y) min_y = scr.y_org;
            if (scr.x_org + scr.width > max_x)  max_x = scr.x_org + scr.width;
            if (scr.y_org + scr.height > max_y) max_y = scr.y_org + scr.height;
        }
        
        width  = max_x - min_x;
        height = max_y - min_y;
    }

    XImage* img = XGetImage(dpy, root, 0, 0, width, height, AllPlanes, ZPixmap);
    if (!img) {
        std::cerr << "capture: XGetImage failed\n";
        if (screens) XFree(screens);
        XCloseDisplay(dpy);
        return false;
    }

    const int rs = mask_shift(img->red_mask);
    const int gs = mask_shift(img->green_mask);
    const int bs = mask_shift(img->blue_mask);

    rgba.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned long px = XGetPixel(img, x, y);
            uint8_t r = static_cast<uint8_t>((px & img->red_mask)   >> rs);
            uint8_t g = static_cast<uint8_t>((px & img->green_mask) >> gs);
            uint8_t b = static_cast<uint8_t>((px & img->blue_mask)  >> bs);
            size_t o = (static_cast<size_t>(y) * width + x) * 4;
            rgba[o + 0] = r;
            rgba[o + 1] = g;
            rgba[o + 2] = b;
            rgba[o + 3] = 255;  // opaque
        }
    }

    if (screens) XFree(screens);
    XDestroyImage(img);
    XCloseDisplay(dpy);
    return true;
}

static bool encode_png(const std::vector<uint8_t>& rgba, int w, int h,
                       std::vector<uint8_t>& png) {
    png.clear();
    int ok = stbi_write_png_to_func(png_collect, &png, w, h, 4,
                                    rgba.data(), w * 4);
    return ok != 0 && !png.empty();
}

static bool send_all(int fd, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static bool send_image(const std::string& host, const std::string& port,
                       const std::vector<uint8_t>& png) {
    addrinfo hints{};
    hints.ai_family   = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM;   // TCP

    addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (gai != 0) {
        std::cerr << "send: cannot resolve '" << host << "': "
                  << gai_strerror(gai) << "\n";
        return false;
    }

    int fd = -1;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;  // success
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        std::cerr << "send: could not connect to " << host << ":" << port << "\n";
        return false;
    }

    uint64_t len = png.size();
    uint8_t hdr[8];
    for (int i = 0; i < 8; ++i)
        hdr[i] = static_cast<uint8_t>((len >> (8 * (7 - i))) & 0xFF);

    bool ok = send_all(fd, hdr, 8) && send_all(fd, png.data(), png.size());
    close(fd);
    return ok;
}


// Uses classic localtime_r/strftime (C++17-safe) and derives milliseconds
// from the same time_point, so no C++20 <chrono> calendar types are needed.
static std::pair<std::string, std::string> make_timestamp() {
    using namespace std::chrono;

    // 1. Current timepoint
    const auto time_now = system_clock::now();

    // Persistent state for interval measurement
    static system_clock::time_point time_old = time_now;

    // 2. Convert to local calendar time (system_clock is UTC-based).
    const std::time_t tt = system_clock::to_time_t(time_now);
    std::tm local_tm{};
    localtime_r(&tt, &local_tm);

    // 3. Milliseconds within the current second, from the same time_point.
    const auto ms = duration_cast<milliseconds>(time_now.time_since_epoch()) % 1000;

    // 5. Folder segment
    char folder_buf[32];
    std::snprintf(folder_buf, sizeof(folder_buf), "%04d-%02d-%02d",
                  local_tm.tm_year + 1900,
                  local_tm.tm_mon + 1,
                  local_tm.tm_mday);

        // 6. File name
    char file_buf[64];
    std::snprintf(file_buf, sizeof(file_buf), "%02d.%02d.%02d.%03d.png",
                  local_tm.tm_hour,
                  local_tm.tm_min,
                  local_tm.tm_sec,
                  static_cast<int>(ms.count()));

    // // 6. File name
    // // 4. Interval: time_point - time_point yields a duration, not an int
    // const auto delta = duration_cast<milliseconds>(time_now - time_old) % 1000;

    // const auto d_min = duration_cast<minutes>(delta);
    // const auto d_sec = duration_cast<seconds>(delta - d_min);
    // const auto d_ms  = delta - d_min - d_sec;

    // char file_buf[64];
    // std::snprintf(file_buf, sizeof(file_buf), "%04d.%02d.%02d_%02d.%02d.%02d_%02d.%02d.%03d.png",
    //               local_tm.tm_year + 1900,
    //               local_tm.tm_mon + 1,
    //               local_tm.tm_mday,
    //               local_tm.tm_hour,
    //               local_tm.tm_min,
    //               local_tm.tm_sec,
    //               static_cast<int>(d_min.count()),
    //               static_cast<int>(d_sec.count()),
    //               static_cast<int>(d_ms.count()));

    return std::make_pair(std::string(folder_buf), std::string(file_buf));
}

static bool save_png_local(const std::string& save_dir, const std::vector<uint8_t>& png) {
    auto [folder, file] = make_timestamp();
    fs::path path = fs::path(save_dir) / folder / file;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        std::cerr << "save: cannot create " << path.parent_path() << ": " << ec.message() << "\n";
        return false;
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "save: cannot write " << path << "\n";
        return false;
    }
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    std::cout << "Saved " << path.string() << " (" << png.size() << " bytes)\n";
    return true;
}

// Requires matching width/height so a monitor being added/removed/resized
// (which can coincidentally produce a same-size buffer) is never treated as
// "identical" just because the two buffers happen to be the same length.
static bool is_screen_99_percent_same(const std::vector<uint8_t>& current, int cur_w, int cur_h,
                                      const std::vector<uint8_t>& previous, int prev_w, int prev_h) {
    if (cur_w != prev_w || cur_h != prev_h) {
        return false;
    }
    if (current.size() != previous.size() || current.empty()) {
        return false;
    }

    size_t total_pixels = current.size() / 4;
    size_t identical_pixels = 0;

    for (size_t i = 0; i < current.size(); i += 4) {
        if (current[i]     == previous[i]     && 
            current[i + 1] == previous[i + 1] && 
            current[i + 2] == previous[i + 2]) {
            identical_pixels++;
        }
    }

    double matching_ratio = static_cast<double>(identical_pixels) / total_pixels;
    return matching_ratio >= 0.99;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <server_host_or_ip> <port> <save_dir> "
                     "[save_interval_seconds] [send_interval_seconds]\n";
        return 1;
    }

    std::string receiver_host   = argv[1];
    std::string receiver_port   = argv[2];
    int receiver_interval       = std::stoi(argv[3]);
    std::string sender_dir      = argv[4];
    int sender_interval         = std::stoi(argv[5]);

    std::cout << "Saving screenshots to " << sender_dir << " every " << sender_interval
              << "s; sending to " << receiver_host << ":" << receiver_port << " every " << sender_interval
              << "s (skipping send if >=99% identical). Ctrl+C to stop.\n";

    std::vector<uint8_t> previous_receiver_rgba;
    int previous_receiver_w = 0, previous_receiver_h = 0;
    std::vector<uint8_t> previous_sender_rgba;
    int previous_sender_w = 0, previous_sender_h = 0;
    auto last_send = std::chrono::steady_clock::now();

    while (true) {
        auto start = std::chrono::steady_clock::now();

        std::vector<uint8_t> rgba, png;
        int w = 0, h = 0;

        if (capture_screen(rgba, w, h)) {
            if (encode_png(rgba, w, h, png)) {
                if (is_screen_99_percent_same(rgba, w, h, previous_sender_rgba, previous_sender_w, previous_sender_h)) {
                    std::cout << "Screen content is >= 99% identical since last local save. Save dropped.\n";
                } else if (save_png_local(sender_dir, png)) {
                    std::cout << "Sender" << w << "x" << h << " (" << png.size() << " bytes)\n";
                    previous_sender_rgba = rgba;
                    previous_sender_w = w;
                    previous_sender_h = h;
                }

                if (start - last_send >= std::chrono::seconds(receiver_interval)) {
                    if (is_screen_99_percent_same(rgba, w, h, previous_receiver_rgba, previous_receiver_w, previous_receiver_h)) {
                        std::cout << "Screen content is >= 99% identical since last send. Send dropped to save bandwidth.\n";
                    } else if (send_image(receiver_host, receiver_port, png)) {
                        std::cout << "Receiver " << w << "x" << h << " (" << png.size() << " bytes)\n";
                        previous_receiver_rgba = rgba;
                        previous_receiver_w = w;
                        previous_receiver_h = h;
                    } else {
                        std::cerr << "Send failed; will retry next cycle.\n";
                    }
                    last_send = start;
                }
            } else {
                std::cerr << "Encode failed; will retry next cycle.\n";
            }
        } else {
            std::cerr << "Capture failed; will retry next cycle.\n";
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        auto remain  = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::seconds(sender_interval) - elapsed);
        if (remain.count() > 0)
            std::this_thread::sleep_for(remain);
    }
    return 0;
}

