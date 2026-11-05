#pragma once

#include <cstdint>
#include <vector>

bool capture_wayland(std::vector<uint8_t>& rgba, int& width, int& height);
