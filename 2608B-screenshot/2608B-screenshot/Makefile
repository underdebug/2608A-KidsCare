CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall
STB      := stb_image_write.h
STB_URL  := https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h

.PHONY: all clean

# `make` builds BOTH programs.
all: screenshot_sender screenshot_receiver

# Fetch the single-header PNG encoder once (only the sender needs it).
$(STB):
	wget -q $(STB_URL) -O $(STB)

# Sender: needs X11 + Xinerama.
screenshot_sender: screenshot_sender.cpp $(STB)
	$(CXX) $(CXXFLAGS) screenshot_sender.cpp -o $@ -lX11 -lXinerama

# Receiver: no external libraries.
screenshot_receiver: screenshot_receiver.cpp
	$(CXX) $(CXXFLAGS) screenshot_receiver.cpp -o $@

clean:
	rm -f screenshot_sender screenshot_receiver
