CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -fPIC -Iheaders -IMiDriverVR
LDFLAGS ?= -shared
LIBS = -lssl -lcrypto -lx264 -lX11 -lpulse-simple -lpulse -lpthread

OUT_DIR = bin/linux64
TARGET = $(OUT_DIR)/driver_MiDriverVR.so
ALIAS = $(OUT_DIR)/driver_mi_driver_tutorial.so

SRCS = MiDriverVR/dllmain.cpp \
       MiDriverVR/Usbtrackingserver.cpp \
       MiDriverVR/Vrlowrespreview.cpp

OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(OUT_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@ln -sf driver_MiDriverVR.so $(ALIAS)
	@echo "Build complete: $@"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(ALIAS)

.PHONY: all clean
