CC	:= g++
LIBS	:= $(shell pkg-config --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 glib-2.0 libsoup-2.4 json-glib-1.0)
CFLAGS	:= -O0 -ggdb -fno-omit-frame-pointer -lstdc++ -std=c++17 $(shell pkg-config --cflags gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 glib-2.0 libsoup-2.4 json-glib-1.0)

omniroom-camera: omniroom-camera.cpp
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@
