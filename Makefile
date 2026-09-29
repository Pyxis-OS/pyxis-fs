ifndef SOURCE
SOURCE := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
endif
BUILD ?= $(SOURCE)/build
HOST_CC ?= cc
HOST_AR ?= ar
CFLAGS ?= -O2 -g3
WARNINGS := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror
CORE_FLAGS := -std=gnu23 -ffreestanding -fno-builtin -fno-stack-protector
SOURCES := base block record tree platform
OBJECTS := $(addprefix $(BUILD)/core/,$(addsuffix .o,$(SOURCES)))
ARCHIVE := $(BUILD)/libpyxis-fs.a

.DEFAULT_GOAL := all
.PHONY: all clean

all: $(ARCHIVE)

$(ARCHIVE): $(OBJECTS)
	$(RM) $@
	$(HOST_AR) rcs $@ $(OBJECTS)

$(BUILD)/core/%.o: $(SOURCE)/core/%.c
	@mkdir -p $(@D)
	$(HOST_CC) $(CPPFLAGS) -I$(SOURCE)/include -I$(SOURCE)/core $(CFLAGS) $(CORE_FLAGS) $(WARNINGS) -MMD -MP -c $< -o $@

clean:
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) $(ARCHIVE)

-include $(OBJECTS:.o=.d)
