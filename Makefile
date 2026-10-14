ifndef SOURCE
SOURCE := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
endif
BUILD ?= $(SOURCE)/build
HOST_CC ?= cc
HOST_AR ?= ar
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g3
WARNINGS := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror
FORMAT_FLAGS := -std=gnu23 -ffreestanding -fno-builtin -fno-stack-protector
FORMAT_SOURCES := base header record journal
FORMAT_OBJECTS := $(addprefix $(BUILD)/format/,$(addsuffix .o,$(FORMAT_SOURCES)))
FORMAT_ARCHIVE := $(BUILD)/libnpfs-format.a
FAT32_SOURCES := base gpt volume directory read check
FAT32_OBJECTS := $(addprefix $(BUILD)/fat32/,$(addsuffix .o,$(FAT32_SOURCES)))
FAT32_ARCHIVE := $(BUILD)/libfat32-read.a
FAT32_WRITE_ARCHIVE := $(BUILD)/libfat32-write.a
FAT32_MOUNTED_ARCHIVE := $(BUILD)/libfat32-mounted.a
FAT32_MOUNTED_OBJECTS := $(BUILD)/fat32/mounted.o
FAT32_WRITE_OBJECTS := $(BUILD)/fat32/write.o
FAT32_HOST_OBJECTS := $(BUILD)/host/fat32/inspect.o
FAT32_WRITE_HOST_OBJECTS := $(BUILD)/host/fat32/write.o
NPFS_HOST_OBJECTS := $(addprefix $(BUILD)/host/npfs/,host.o journal.o mkfs.o mkfs_source.o check.o fsck.o inspect.o)
NPFS_TOOLS := $(BUILD)/mkfs.npfs $(BUILD)/fsck.npfs $(BUILD)/npfs-inspect
FUSE_AVAILABLE := $(shell $(PKG_CONFIG) --exists fuse3 2>/dev/null && echo yes)
ifeq ($(FUSE_AVAILABLE),yes)
FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3)
FUSE_LIBS := $(shell $(PKG_CONFIG) --libs fuse3)
NPFS_TOOLS += $(BUILD)/npfs-fuse
endif
NPFS_HOST_OBJECTS += $(BUILD)/host/npfs/fuse.o

.DEFAULT_GOAL := all
.PHONY: all clean npfs-fuse fat32-read fat32-write fat32-mounted FORCE

all: $(FORMAT_ARCHIVE) $(NPFS_TOOLS) $(FAT32_ARCHIVE) $(BUILD)/fat32-inspect $(FAT32_WRITE_ARCHIVE) $(BUILD)/fat32-write $(FAT32_MOUNTED_ARCHIVE)

fat32-read: $(FAT32_ARCHIVE)

fat32-write: $(FAT32_ARCHIVE) $(FAT32_WRITE_ARCHIVE)

fat32-mounted: $(FAT32_ARCHIVE) $(FAT32_MOUNTED_ARCHIVE)

$(FAT32_MOUNTED_ARCHIVE): $(FAT32_MOUNTED_OBJECTS)
	$(RM) $@
	$(HOST_AR) rcs $@ $^

$(FAT32_WRITE_ARCHIVE): $(FAT32_WRITE_OBJECTS)
	$(RM) $@
	$(HOST_AR) rcs $@ $^

$(BUILD)/fat32-write: $(FAT32_WRITE_HOST_OBJECTS) $(FAT32_WRITE_ARCHIVE) $(FAT32_ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(FAT32_ARCHIVE): $(FAT32_OBJECTS)
	$(RM) $@
	$(HOST_AR) rcs $@ $^

$(BUILD)/fat32/%.o: $(SOURCE)/fat32/%.c
	@mkdir -p $(@D)
	$(HOST_CC) $(CPPFLAGS) -I$(SOURCE)/include $(CFLAGS) $(FORMAT_FLAGS) $(WARNINGS) -MMD -MP -c $< -o $@

$(FAT32_OBJECTS) $(FAT32_WRITE_OBJECTS) $(FAT32_MOUNTED_OBJECTS): $(BUILD)/fat32/.config

$(BUILD)/fat32/.config: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' "$(HOST_CC)" "$(CPPFLAGS)" "$(CFLAGS)" "$(FORMAT_FLAGS)" "$(WARNINGS)" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

FORCE:

$(BUILD)/fat32-inspect: $(FAT32_HOST_OBJECTS) $(FAT32_ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

ifeq ($(FUSE_AVAILABLE),yes)
npfs-fuse: $(BUILD)/npfs-fuse

$(BUILD)/host/npfs/fuse.o: CPPFLAGS += $(FUSE_CFLAGS)

$(BUILD)/npfs-fuse: $(addprefix $(BUILD)/host/npfs/,fuse.o host.o journal.o) $(FORMAT_ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) $(FUSE_LIBS) -o $@
else
npfs-fuse:
	@echo 'npfs-fuse requires pkg-config and the libfuse3 development package.' >&2
	@exit 1
endif

$(BUILD)/host/%.o: $(SOURCE)/host/%.c
	@mkdir -p $(@D)
	$(HOST_CC) $(CPPFLAGS) -I$(SOURCE)/include $(CFLAGS) -std=gnu23 $(WARNINGS) -MMD -MP -c $< -o $@

$(BUILD)/mkfs.npfs: $(addprefix $(BUILD)/host/npfs/,mkfs.o mkfs_source.o host.o journal.o) $(FORMAT_ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/fsck.npfs: $(addprefix $(BUILD)/host/npfs/,fsck.o check.o host.o journal.o) $(FORMAT_ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/npfs-inspect: $(addprefix $(BUILD)/host/npfs/,inspect.o host.o journal.o) $(FORMAT_ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(FORMAT_ARCHIVE): $(FORMAT_OBJECTS)
	$(RM) $@
	$(HOST_AR) rcs $@ $^

$(BUILD)/format/%.o: $(SOURCE)/format/%.c
	@mkdir -p $(@D)
	$(HOST_CC) $(CPPFLAGS) -I$(SOURCE)/include $(CFLAGS) $(FORMAT_FLAGS) $(WARNINGS) -MMD -MP -c $< -o $@

clean:
	$(RM) $(FAT32_MOUNTED_OBJECTS) $(FAT32_MOUNTED_OBJECTS:.o=.d) $(FAT32_MOUNTED_ARCHIVE)
	$(RM) $(FORMAT_OBJECTS) $(FORMAT_OBJECTS:.o=.d) $(FORMAT_ARCHIVE) $(NPFS_HOST_OBJECTS) $(NPFS_HOST_OBJECTS:.o=.d) $(NPFS_TOOLS) $(BUILD)/npfs-fuse
	$(RM) $(FAT32_OBJECTS) $(FAT32_OBJECTS:.o=.d) $(FAT32_ARCHIVE) $(FAT32_HOST_OBJECTS) $(FAT32_HOST_OBJECTS:.o=.d) $(BUILD)/fat32-inspect
	$(RM) $(FAT32_WRITE_OBJECTS) $(FAT32_WRITE_OBJECTS:.o=.d) $(FAT32_WRITE_ARCHIVE) $(FAT32_WRITE_HOST_OBJECTS) $(FAT32_WRITE_HOST_OBJECTS:.o=.d) $(BUILD)/fat32-write

-include $(FORMAT_OBJECTS:.o=.d) $(NPFS_HOST_OBJECTS:.o=.d)
-include $(FAT32_MOUNTED_OBJECTS:.o=.d)
-include $(FAT32_OBJECTS:.o=.d) $(FAT32_HOST_OBJECTS:.o=.d) $(FAT32_WRITE_OBJECTS:.o=.d) $(FAT32_WRITE_HOST_OBJECTS:.o=.d)
