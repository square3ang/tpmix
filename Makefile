# vim: noet:ts=4:sw=4:

CXXFLAGS += -I/usr/include/wx-3.2 `wx-config --cxxflags --libs`
CXXFLAGS += -g --debug 
#CXXFLAGS += -O3
CXXFLAGS += -Wall
CXXFLAGS += --std=c++20
# what is this?
CXXFLAGS += -DWXUSINGDLL
CXXFLAGS += -lhidapi-hidraw
CXXFLAGS += -pthread

.PHONY: all clean

all: tpmix

tpmix: tpmix.cpp
	g++  $< $(CXXFLAGS) -o $@ 

clean:
	rm -fv tpmix
