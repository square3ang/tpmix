# vim: noet:ts=4:sw=4:

CXXFLAGS += `pkg-config --cflags Qt6Widgets` -fPIC
CXXFLAGS += -g --debug
#CXXFLAGS += -O3
CXXFLAGS += -Wall
CXXFLAGS += --std=c++20
LDLIBS += `pkg-config --libs Qt6Widgets`
LDLIBS += -lhidapi-hidraw
LDLIBS += -pthread

.PHONY: all clean

all: tpmix

tpmix: tpmix.cpp
	g++  $< $(CXXFLAGS) $(LDLIBS) -o $@

clean:
	rm -fv tpmix
