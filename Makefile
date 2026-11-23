BUILD_DIR := build/linux
PROGRAMS := steam-haptics-sink steam-haptics-player range measure
HIDAPI_PKG ?= hidapi-hidraw

CPPFLAGS += -IsharedSrc -ITritonLib/include $(shell pkg-config --cflags $(HIDAPI_PKG))
CXXFLAGS += -std=c++20 -Wall -Werror -fPIE
LDFLAGS += -pie
LDLIBS += $(shell pkg-config --libs $(HIDAPI_PKG))

ifneq ($(filter release,$(MAKECMDGOALS)),)
CXXFLAGS += -O2
LDFLAGS += -s
else
CXXFLAGS += -g -Og
endif

objects = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(wildcard $(1)/*.cpp))
triton := $(call objects,TritonLib/src)
shared := $(call objects,sharedSrc)
sink := $(call objects,sinkSrc)
player := $(call objects,playPCMSrc)
range_obj := $(call objects,rangeSrc)
measure_obj := $(call objects,measureSrc)

.PHONY: all release clean FORCE
all: $(PROGRAMS)
release: all

steam-haptics-sink: $(sink) $(triton)
steam-haptics-sink: private LDLIBS += -pthread $(shell pkg-config --libs libpipewire-0.3)
$(sink): private CPPFLAGS += -pthread $(shell pkg-config --cflags libpipewire-0.3)
steam-haptics-player: $(player) $(shared) $(triton)
range: $(range_obj) $(shared) $(triton)
measure: $(measure_obj) $(shared) $(triton)

$(PROGRAMS):
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/.compiler-flags: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(CXX) $(CPPFLAGS) $(CXXFLAGS)' | cmp -s - $@ || printf '%s\n' '$(CXX) $(CPPFLAGS) $(CXXFLAGS)' > $@

$(BUILD_DIR)/%.o: %.cpp $(BUILD_DIR)/.compiler-flags
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(patsubst %.o,%.d,$(sink) $(player) $(range_obj) $(measure_obj) $(shared) $(triton))

clean:
	rm -rf $(BUILD_DIR) $(PROGRAMS)
