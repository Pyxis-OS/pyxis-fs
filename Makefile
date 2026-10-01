ifndef SOURCE
SOURCE := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
endif
BUILD ?= $(SOURCE)/build
HOST_CC ?= cc
HOST_AR ?= ar
CFLAGS ?= -O2 -g3
WARNINGS := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror
CORE_FLAGS := -std=gnu23 -ffreestanding -fno-builtin -fno-stack-protector
SOURCES := mutate file writer writer_access admit plan edit canonical base block record tree platform build pool access check check_walk check_reconcile
OBJECTS := $(addprefix $(BUILD)/core/,$(addsuffix .o,$(SOURCES)))
ARCHIVE := $(BUILD)/libpyxis-fs.a
HOST_OBJECTS := $(addprefix $(BUILD)/host/,write.o host.o gpt.o source.o mkpyxisfs.o inspect.o inspect_options.o inspect_objects.o inspect_check.o extract.o)
TOOLS := $(BUILD)/mkpyxisfs $(BUILD)/pyxisfs-inspect $(BUILD)/pyxisfs-write

.DEFAULT_GOAL := all
.PHONY: all clean check check-extended

all: $(ARCHIVE) $(TOOLS)

TEST_SOURCES := recovery_workload extended_tests namespace_failure_tests namespace_tests file_workloads file_tests publication_tests live_tests failure failure_tests admit_tests main support baseline_tests build_tests plan_tests codec_tests edit_tests
TEST_HOST_OBJECTS := $(addprefix $(BUILD)/host/,source.o host.o gpt.o)
TEST_OBJECTS := $(addprefix $(BUILD)/tests/,$(addsuffix .o,$(TEST_SOURCES)))
UNITY_OBJECT := $(BUILD)/tests/unity.o
TEST_RUNNER := $(BUILD)/pyxis-fs-tests

check: $(TEST_RUNNER)
	$(TEST_RUNNER) --suite pr

check-extended: $(TEST_RUNNER)
	$(TEST_RUNNER) --suite extended --seed 1

$(TEST_RUNNER): $(TEST_OBJECTS) $(UNITY_OBJECT) $(TEST_HOST_OBJECTS) $(ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/tests/%.o: $(SOURCE)/tests/%.c
	@mkdir -p $(@D)
	$(HOST_CC) $(CPPFLAGS) -I$(SOURCE)/include -I$(SOURCE)/core -I$(SOURCE)/third_party/unity $(CFLAGS) -std=gnu23 $(WARNINGS) -MMD -MP -c $< -o $@

# Preserve upstream formatting and compile the host-only dependency separately.
$(UNITY_OBJECT): $(SOURCE)/third_party/unity/unity.c
	@mkdir -p $(@D)
	$(HOST_CC) $(CPPFLAGS) -I$(SOURCE)/third_party/unity $(CFLAGS) -std=gnu23 -MMD -MP -c $< -o $@


$(BUILD)/pyxisfs-write: $(addprefix $(BUILD)/host/,write.o host.o gpt.o) $(ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/mkpyxisfs: $(addprefix $(BUILD)/host/,mkpyxisfs.o source.o host.o gpt.o) $(ARCHIVE)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/pyxisfs-inspect: $(addprefix $(BUILD)/host/,inspect.o inspect_options.o inspect_objects.o inspect_check.o extract.o host.o gpt.o) $(ARCHIVE)
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
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) $(ARCHIVE) $(HOST_OBJECTS) $(HOST_OBJECTS:.o=.d) $(TOOLS) $(TEST_OBJECTS) $(TEST_OBJECTS:.o=.d) $(UNITY_OBJECT) $(UNITY_OBJECT:.o=.d) $(TEST_RUNNER)

-include $(OBJECTS:.o=.d) $(HOST_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(UNITY_OBJECT:.o=.d)
