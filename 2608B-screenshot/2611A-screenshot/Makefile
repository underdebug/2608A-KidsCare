CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall
STB      := stb_image_write.h
STB_URL  := https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
PORTAL_CFLAGS := $(shell pkg-config --cflags gio-unix-2.0 libpipewire-0.3)
PORTAL_LIBS   := $(shell pkg-config --libs gio-unix-2.0 libpipewire-0.3)

.PHONY: all clean

# `make` builds BOTH programs.
all: screenshot_sender screenshot_receiver

# Fetch the single-header PNG encoder once (only the sender needs it).
$(STB):
	wget -q $(STB_URL) -O $(STB)

# Sender: X11 capture plus the ScreenCast portal and PipeWire for Wayland.
screenshot_sender: screenshot_sender.cpp wayland_capture.cpp wayland_capture.h $(STB)
	$(CXX) $(CXXFLAGS) $(PORTAL_CFLAGS) screenshot_sender.cpp wayland_capture.cpp -o $@ -lX11 -lXinerama $(PORTAL_LIBS)

# Receiver: no external libraries.
screenshot_receiver: screenshot_receiver.cpp
	$(CXX) $(CXXFLAGS) screenshot_receiver.cpp -o $@

clean:
	rm -f screenshot_sender screenshot_receiver
