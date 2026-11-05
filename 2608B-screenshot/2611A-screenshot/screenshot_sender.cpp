// screenshot_sender.cpp
// Capture the desktop (X11 or Wayland), save it as a PNG locally every
// <save_interval> seconds, and transmit it to a remote host every
// <send_interval> seconds. Skip sending if the image is >= 99% identical to
// the last one actually sent. Fully handles Mirror Mode display
// configurations safely.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "wayland_capture.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>

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
        std::cerr << "capture: cannot open X display; check DISPLAY and XAUTHORITY. Full desktop capture requires an X11 session\n";
        return false;
    }
    Window root = DefaultRootWindow(dpy);

    // The root window already covers the full X11 desktop. Xinerama bounds
    // can exceed it (for example with mirrored outputs), causing BadMatch.
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, root, &attr) || attr.width <= 0 || attr.height <= 0) {
        std::cerr << "capture: cannot determine root window dimensions\n";
        XCloseDisplay(dpy);
        return false;
    }
    width = attr.width;
    height = attr.height;

    XImage* img = XGetImage(dpy, root, 0, 0, width, height, AllPlanes, ZPixmap);
    if (!img) {
        std::cerr << "capture: XGetImage failed\n";
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
    f.close();
    if (!f) {
        std::cerr << "save: failed to write " << path << "\n";
        return false;
    }

    const std::time_t now = std::time(nullptr);
    std::tm local_tm{};
    char time_buf[9];
    std::ofstream log(path.parent_path() / "log.txt", std::ios::app);
    if (!log || !localtime_r(&now, &local_tm) ||
        std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &local_tm) == 0 ||
        !(log << time_buf << " : " << path.filename().string() << '\n')) {
        std::cerr << "save: cannot write log in " << path.parent_path() << "\n";
    }
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
    if (argc < 4 || argc > 6) {
        std::cerr << "Usage: " << argv[0]
                  << " <server_host_or_ip> <port> <save_dir> "
                     "[save_interval_seconds] [send_interval_seconds]\n";
        return 1;
    }

    std::string receiver_host   = argv[1];
    std::string receiver_port   = argv[2];
    std::string sender_dir      = argv[3];
    int sender_interval = 120;
    int receiver_interval = 300;
    try {
        auto parse_interval = [](const char* value) {
            std::string text(value);
            size_t used = 0;
            int result = std::stoi(text, &used);
            if (used != text.size() || result <= 0) throw std::invalid_argument("interval");
            return result;
        };
        if (argc > 4) sender_interval = parse_interval(argv[4]);
        if (argc > 5) receiver_interval = parse_interval(argv[5]);
    } catch (const std::exception&) {
        std::cerr << "Intervals must be positive whole numbers of seconds.\n";
        return 1;
    }

    std::cout << "Saving screenshots to " << sender_dir << " every " << sender_interval
              << "s; sending to " << receiver_host << ":" << receiver_port << " every " << receiver_interval
              << "s (skipping send if >=99% identical). Ctrl+C to stop.\n";

    std::vector<uint8_t> previous_receiver_rgba;
    int previous_receiver_w = 0, previous_receiver_h = 0;
    auto last_send = std::chrono::steady_clock::now();

    while (true) {
        auto start = std::chrono::steady_clock::now();

        std::vector<uint8_t> rgba, png;
        int w = 0, h = 0;

        const char* session = std::getenv("XDG_SESSION_TYPE");
        const bool wayland = session && std::string(session) == "wayland";
        if (wayland ? capture_wayland(rgba, w, h) : capture_screen(rgba, w, h)) {
            if (encode_png(rgba, w, h, png)) {
                if (save_png_local(sender_dir, png)) {
                    std::cout << "Sender " << w << "x" << h << " (" << png.size() << " bytes)\n";
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

/*
sudo apt install build-essential libx11-dev libxinerama-dev libglib2.0-dev libpipewire-0.3-dev
sudo bash install-sender.sh
journalctl -u screenshot-sender -f
echo "$XDG_SESSION_TYPE" // wayland or x11

gnome-shell --version
GNOME Shell 50.1


cat /etc/os-release
PRETTY_NAME="Ubuntu 26.04.1 LTS"
NAME="Ubuntu"
VERSION_ID="26.04"
VERSION="26.04.1 LTS (Resolute Raccoon)"
VERSION_CODENAME=resolute
ID=ubuntu
ID_LIKE=debian
HOME_URL="https://www.ubuntu.com/"
SUPPORT_URL="https://help.ubuntu.com/"
BUG_REPORT_URL="https://bugs.launchpad.net/ubuntu/"
PRIVACY_POLICY_URL="https://www.ubuntu.com/legal/terms-and-policies/privacy-policy"
UBUNTU_CODENAME=resolute
LOGO=ubuntu-logo
*/