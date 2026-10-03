ifndef SOURCE
SOURCE := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
endif
BUILD ?= $(SOURCE)/build
HOST_CC ?= cc
HOST_AR ?= ar
CFLAGS ?= -O2 -g3
WARNINGS := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror
FORMAT_FLAGS := -std=gnu23 -ffreestanding -fno-builtin -fno-stack-protector
FORMAT_SOURCES := base header record journal
FORMAT_OBJECTS := $(addprefix $(BUILD)/format/,$(addsuffix .o,$(FORMAT_SOURCES)))
FORMAT_ARCHIVE := $(BUILD)/libpyxis-fs-format.a
NATIVE_HOST_OBJECTS := $(addprefix $(BUILD)/host/native/,host.o mkfs.o mkfs_source.o check.o fsck.o inspect.o)
NATIVE_TOOLS := $(BUILD)/mkpyxisfs-native $(BUILD)/pyxisfs-native-fsck $(BUILD)/pyxisfs-native-inspect

.DEFAULT_GOAL := all
.PHONY: all clean

all: $(FORMAT_ARCHIVE) $(NATIVE_TOOLS)

$(BUILD)/host/%.o: $(SOURCE)/host/%.c
	@mkdir -p $(@D)
	$(HOST_CC) $(CPPFLAGS) -I$(SOURCE)/include $(CFLAGS) -std=gnu23 $(WARNINGS) -MMD -MP -c $< -o $@

$(BUILD)/mkpyxisfs-native: $(addprefix $(BUILD)/host/native/,mkfs.o mkfs_source.o host.o) $(FORMAT_ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/pyxisfs-native-fsck: $(addprefix $(BUILD)/host/native/,fsck.o check.o host.o) $(FORMAT_ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/pyxisfs-native-inspect: $(addprefix $(BUILD)/host/native/,inspect.o host.o) $(FORMAT_ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(FORMAT_ARCHIVE): $(FORMAT_OBJECTS)
	$(RM) $@
	$(HOST_AR) rcs $@ $^

$(BUILD)/format/%.o: $(SOURCE)/format/%.c
	@mkdir -p $(@D)
	$(HOST_CC) $(CPPFLAGS) -I$(SOURCE)/include $(CFLAGS) $(FORMAT_FLAGS) $(WARNINGS) -MMD -MP -c $< -o $@

clean:
	$(RM) $(FORMAT_OBJECTS) $(FORMAT_OBJECTS:.o=.d) $(FORMAT_ARCHIVE) $(NATIVE_HOST_OBJECTS) $(NATIVE_HOST_OBJECTS:.o=.d) $(NATIVE_TOOLS)

-include $(FORMAT_OBJECTS:.o=.d) $(NATIVE_HOST_OBJECTS:.o=.d)
