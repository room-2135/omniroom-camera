CC	:= g++
LIBS	:= $(shell pkg-config --libs --cflags gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0)
CFLAGS	:= -O0 -ggdb -fno-omit-frame-pointer -lstdc++ -std=c++17

omniroom-camera: omniroom-camera.cpp
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@
