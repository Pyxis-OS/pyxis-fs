ifndef SOURCE
SOURCE := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
endif
BUILD ?= $(SOURCE)/build
HOST_CC ?= cc
HOST_AR ?= ar
CFLAGS ?= -O2 -g3
WARNINGS := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror
CORE_FLAGS := -std=gnu23 -ffreestanding -fno-builtin -fno-stack-protector
SOURCES := base block record tree platform build pool
OBJECTS := $(addprefix $(BUILD)/core/,$(addsuffix .o,$(SOURCES)))
ARCHIVE := $(BUILD)/libpyxis-fs.a
HOST_OBJECTS := $(addprefix $(BUILD)/host/,host.o mkpyxisfs.o inspect.o)
TOOLS := $(BUILD)/mkpyxisfs $(BUILD)/pyxisfs-inspect

.DEFAULT_GOAL := all
.PHONY: all clean

all: $(ARCHIVE) $(TOOLS)

$(BUILD)/mkpyxisfs: $(BUILD)/host/mkpyxisfs.o $(BUILD)/host/host.o $(ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/pyxisfs-inspect: $(BUILD)/host/inspect.o $(BUILD)/host/host.o $(ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/host/%.o: $(SOURCE)/host/%.c
	@mkdir -p $(@D)
	$(HOST_CC) $(CPPFLAGS) -I$(SOURCE)/include $(CFLAGS) -std=gnu23 $(WARNINGS) -MMD -MP -c $< -o $@

$(ARCHIVE): $(OBJECTS)
	$(RM) $@
	$(HOST_AR) rcs $@ $(OBJECTS)

$(BUILD)/core/%.o: $(SOURCE)/core/%.c
	@mkdir -p $(@D)
	$(HOST_CC) $(CPPFLAGS) -I$(SOURCE)/include -I$(SOURCE)/core $(CFLAGS) $(CORE_FLAGS) $(WARNINGS) -MMD -MP -c $< -o $@

clean:
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) $(ARCHIVE) $(HOST_OBJECTS) $(HOST_OBJECTS:.o=.d) $(TOOLS)

-include $(OBJECTS:.o=.d) $(HOST_OBJECTS:.o=.d)
