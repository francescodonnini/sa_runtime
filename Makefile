CC      ?= gcc
HOSTCC  ?= $(CC)
BUILD   := build
TARGET  := $(BUILD)/libruntime.so

# local.mk definisce UAPI_DIR (directory che contiene uapi.h).
# Si può anche passare da riga di comando: make UAPI_DIR=...
-include local.mk

# I target che non compilano nulla non hanno bisogno di uapi.h
NO_UAPI_GOALS := clean format format-check
ifneq ($(if $(MAKECMDGOALS),$(filter-out $(NO_UAPI_GOALS),$(MAKECMDGOALS)),all),)
  ifeq ($(strip $(UAPI_DIR)),)
    $(error UAPI_DIR non impostato: crea local.mk con "UAPI_DIR := <directory di uapi.h>")
  endif
  ifeq ($(wildcard $(UAPI_DIR)/uapi.h),)
    $(error $(UAPI_DIR)/uapi.h not found, check or insert UAPI_DIR in local.mk)
  endif
endif

GEN_SRC    := gen_asm_consts.c
GEN_BIN    := $(BUILD)/gen_asm_consts
ASM_CONSTS := $(BUILD)/asm_consts.h

# Entry point dell'eseguibile instrumentato: va linkato nel programma,
# non dentro libruntime.so (salta a _start, che sta nell'eseguibile)
ENTRY_SRC  := stckcpy.S
ENTRY_OBJ  := $(BUILD)/stckcpy.o

SRCS       := $(filter-out $(GEN_SRC),$(wildcard *.c))
OBJS       := $(SRCS:%.c=$(BUILD)/%.o)

CFLAGS     += -std=gnu17 -g -O2 -fPIC -MMD -MP \
              -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
              -Wstrict-prototypes -Wmissing-prototypes -Wvla \
              -Werror=implicit-function-declaration
CFLAGS     += -D_GNU_SOURCE -I$(UAPI_DIR)
ASFLAGS    += -MMD -MP -I$(BUILD)
LDFLAGS    += -shared
LDLIBS     += -ldl -lpthread

all: $(TARGET) $(ENTRY_OBJ)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# L'assembly include asm_consts.h, che va generato prima
$(ENTRY_OBJ): $(ENTRY_SRC) $(ASM_CONSTS) | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

# Il generatore gira sulla macchina di build, quindi usa HOSTCC
$(GEN_BIN): $(GEN_SRC) $(UAPI_DIR)/uapi.h | $(BUILD)
	$(HOSTCC) -std=gnu17 -Wall -Wextra -I$(UAPI_DIR) $< -o $@

# Se il generatore fallisce (ioctl e struct incoerenti) l'header non viene creato
$(ASM_CONSTS): $(GEN_BIN)
	$< > $@.tmp && mv $@.tmp $@

$(BUILD):
	mkdir -p $@

compdb: clean
	bear -- $(MAKE)

format:
	clang-format -i $(SRCS) $(GEN_SRC) $(wildcard *.h)

format-check:
	clang-format --dry-run --Werror $(SRCS) $(GEN_SRC) $(wildcard *.h)

lint:
	clang-tidy -p . $(SRCS)
	cppcheck --enable=warning,performance,portability --quiet \
		--suppress=missingIncludeSystem -I$(UAPI_DIR) $(SRCS)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(ENTRY_OBJ:.o=.d)

.PHONY: all compdb format format-check lint clean