// screenshot_receiver.cpp
// Listen for incoming screenshots and save each one as
//     <output_dir>/YYYY-MM-DD/HH.MM.SS.mmm.png
//
// Runs on the collector machine.
//
// Build:
//   g++ -O2 -std=c++20 screenshot_receiver.cpp -o screenshot_receiver
//   (GCC >= 10 needed for <chrono> calendar types; no -lstdc++fs required)
//
// Run:
//   ./screenshot_receiver <receiver_port> [output_dir]
//   e.g.  ./screenshot_receiver 5000 /home/roots/shots

#include <arpa/inet.h>
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
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// Receive exactly len bytes (handles partial reads).
static bool recv_all(int fd, uint8_t* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, data + got, len - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
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

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <receiver_port> [output_dir]\n";
        return 1;
    }
    uint16_t    receiver_port   = static_cast<uint16_t>(std::stoi(argv[1]));
    std::string receiver_dir = argv[2];

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(receiver_port);
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, 4) < 0) { perror("listen"); return 1; }

    std::cout << "Listening on receiver_port " << receiver_port << ", saving under "
              << receiver_dir << "/YYYY-MM-DD/. Ctrl+C to stop.\n";

    while (true) {
        sockaddr_in cli{};
        socklen_t   clilen = sizeof(cli);
        int fd = accept(srv, reinterpret_cast<sockaddr*>(&cli), &clilen);
        if (fd < 0) { perror("accept"); continue; }

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));

        uint8_t hdr[8];
        if (!recv_all(fd, hdr, 8)) {
            std::cerr << "Short header from " << ip << "\n";
            close(fd);
            continue;
        }
        uint64_t len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8) | hdr[i];

        // Sanity cap: reject empty or absurdly large frames (> 256 MB).
        if (len == 0 || len > (256ull << 20)) {
            std::cerr << "Bad length " << len << " from " << ip << "\n";
            close(fd);
            continue;
        }

        std::vector<uint8_t> png(len);
        if (!recv_all(fd, png.data(), len)) {
            std::cerr << "Short payload from " << ip << "\n";
            close(fd);
            continue;
        }
        close(fd);

        // Build <receiver_dir>/<date>/<time>.png and create the day folder if needed.
        auto [folder, file] = make_timestamp();
        fs::path dir  = fs::path(receiver_dir) / folder;
        fs::path path = dir / file;

        std::error_code ec;
        fs::create_directories(dir, ec);   // like `mkdir -p`
        if (ec) {
            std::cerr << "Cannot create " << dir << ": " << ec.message() << "\n";
            continue;
        }

        std::ofstream f(path, std::ios::binary);
        if (!f) {
            std::cerr << "Cannot write " << path << "\n";
            continue;
        }
        f.write(reinterpret_cast<const char*>(png.data()),
                static_cast<std::streamsize>(png.size()));
        std::cout << "Saved " << path.string() << " (" << len
                  << " bytes) from " << ip << "\n";
    }
    return 0;
}
