cmake_minimum_required(VERSION 3.16)
project(dimmer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

find_package(Vulkan REQUIRED)
find_package(X11 REQUIRED)

if(NOT X11_Xfixes_FOUND)
    message(FATAL_ERROR "libXfixes development files are required")
endif()

if(NOT X11_Xrandr_FOUND)
    message(FATAL_ERROR "libXrandr development files are required")
endif()

add_executable(dimmer dimmer.cpp)

target_link_libraries(dimmer
    PRIVATE
        Vulkan::Vulkan
        ${X11_LIBRARIES}
        ${X11_Xfixes_LIB}
        ${X11_Xext_LIB}
        ${X11_Xrandr_LIB}
)

target_include_directories(dimmer PRIVATE ${X11_INCLUDE_DIR})

# Debian/Ubuntu:  sudo apt install libvulkan-dev libx11-dev libxfixes-dev libxext-dev libxrandr-dev
# Fedora:         sudo dnf install vulkan-loader-devel libX11-devel libXfixes-devel libXext-devel libXrandr-devel
# Arch:           sudo pacman -S vulkan-headers vulkan-icd-loader libx11 libxfixes libxext libxrandr
