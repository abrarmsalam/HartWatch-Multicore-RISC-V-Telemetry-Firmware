# HartWatch firmware build.
#
#   make            build build/hartwatch.elf
#   make run        boot it in QEMU (console on this terminal; exits by itself)
#   make test       host unit tests, then `run` with a timeout and a pass/fail verdict
#   make host-test  only the host unit tests
#   make stress     hammer the rings with RISC-V code on all harts (fw/probe/ring_stress.c)
#   make test-drops boot a build that overflows its rings and check the drops are accounted for
#   make debug      boot QEMU frozen, waiting for GDB on port 1234 (use `make gdb` in a 2nd terminal)
#   make gdb        attach GDB to a running `make debug`
#   make dtb        dump QEMU's hardware description (device tree) to build/sifive_u.dts
#   make disasm     disassemble the firmware to build/hartwatch.lst
#   make probe      run the counter probe (fw/probe); add QEMU_TIMING= to run without -icount
#   make clean
#
# If your cross-compiler has a different prefix (e.g. Homebrew's riscv64-elf-gcc):
#   make CROSS=riscv64-elf-
#
# Counter backend is chosen at build time. Give another backend its own build directory:
#   make HPM_BACKEND=sifive BUILD=build/sifive

CROSS ?= riscv64-unknown-elf-
CC    := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
SIZE  := $(CROSS)size

QEMU  ?= qemu-system-riscv64
GDB   ?= $(shell command -v gdb-multiarch >/dev/null 2>&1 && echo gdb-multiarch || echo $(CROSS)gdb)

BUILD := build
HPM_BACKEND ?= qemu
SAMPLE_CHECKSUM ?= 1
TEST_TIMEOUT ?= 120
STRESS_TIMEOUT ?= 600
HOSTCC ?= cc
ELF   := $(BUILD)/hartwatch.elf

# rv64imac works on BOTH core types: the E51 is rv64imac, the U54s are rv64imafdc.
# -mcmodel=medany : code lives at 0x80000000, outside the +/-2 GiB "medlow" window.
# -mno-relax      : we never initialise the gp register, so forbid gp-relative linker relaxation.
ARCH   := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany -mno-relax
CFLAGS := $(ARCH) -std=gnu11 -O2 -g -Wall -Wextra \
          -ffreestanding -fno-pic -fno-stack-protector -ffunction-sections -fdata-sections \
          -Ifw/src -DSAMPLE_CHECKSUM=$(SAMPLE_CHECKSUM) $(FW_DEFS)
ASFLAGS := $(ARCH) -g -Ifw
LDFLAGS := $(ARCH) -nostdlib -nostartfiles -static -Wl,--gc-sections -Wl,--no-relax -Wl,--no-warn-rwx-segments \
           -Wl,-T,fw/linker.ld -Wl,-Map,$(BUILD)/hartwatch.map

C_SRCS := $(wildcard fw/src/*.c)
OBJS   := $(BUILD)/boot.o $(BUILD)/workloads.o $(BUILD)/hpm_$(HPM_BACKEND).o \
          $(patsubst fw/src/%.c,$(BUILD)/%.o,$(C_SRCS))

# QEMU: 1 E51 + 4 U54 = 5 harts, 256 MiB RAM, no firmware (we ARE the firmware),
# and treat a reset request as "exit".
#
# -icount shift=0,sleep=off : deterministic virtual time (1 instruction = 1 ns). Without it,
# QEMU ties timer interrupts to the host's real clock and tick spacing jitters by milliseconds
# depending on how busy your computer is. Use `make run QEMU_TIMING=` to turn it off.
QEMU_TIMING ?= -icount shift=0,sleep=off
QEMU_EXTRA  ?=
SMP ?= 5
# Serial port 0 (UART0) is the console; serial port 1 (UART1) is the telemetry stream.
TELEMETRY  := $(BUILD)/telemetry.bin
QEMU_COMMON := -machine sifive_u -smp $(SMP) -m 256M -nographic -bios none -no-reboot \
               -serial mon:stdio -serial file:$(TELEMETRY) $(QEMU_EXTRA)
QEMU_BASE  := $(QEMU_COMMON) $(QEMU_TIMING)
QEMU_FLAGS := $(QEMU_BASE) -kernel $(ELF)

.PHONY: all run test test-fw test-drops host-test debug gdb dtb disasm size clean probe tickgap stress
all: $(ELF) size

$(ELF): $(OBJS) fw/linker.ld
	$(CC) $(LDFLAGS) $(OBJS) -lgcc -o $@

$(BUILD)/boot.o: fw/boot.S fw/src/config.h
	@mkdir -p $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/workloads.o: fw/src/workloads.S
	@mkdir -p $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: fw/src/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/hpm_%.o: fw/src/hpm/hpm_%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# The counter probe is its own image: it shares boot and console code with the firmware,
# but brings its own hart0_main, hart_main and trap_handler.
PROBE_ELF  := $(BUILD)/probe.elf
PROBE_OBJS := $(BUILD)/boot.o $(BUILD)/kprintf.o $(BUILD)/power.o $(BUILD)/uart.o \
              $(BUILD)/probe_main.o $(BUILD)/workloads.o

$(PROBE_ELF): $(PROBE_OBJS) fw/linker.ld
	$(CC) $(LDFLAGS) $(PROBE_OBJS) -lgcc -o $@

$(BUILD)/probe_main.o: fw/probe/probe.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Ring stress test. Runs WITHOUT -icount by default: that is the mode where harts really run
# in parallel. STRESS_TIMING="-icount shift=0,sleep=off" runs it in turn-taking mode instead.
STRESS_ELF := $(BUILD)/stress.elf
STRESS_TIMING ?=
STRESS_OBJS := $(BUILD)/boot.o $(BUILD)/kprintf.o $(BUILD)/power.o $(BUILD)/uart.o \
               $(BUILD)/sample.o $(BUILD)/stress_main.o

$(BUILD)/stress_main.o: fw/probe/ring_stress.c fw/src/ring.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(STRESS_ELF): $(STRESS_OBJS) fw/linker.ld
	$(CC) $(LDFLAGS) $(STRESS_OBJS) -lgcc -o $@

stress: $(STRESS_ELF)
	@EXPECT="STRESS PASSED" TEST_TIMEOUT=$(STRESS_TIMEOUT) scripts/run_test.sh $(STRESS_ELF) \
	    "$(QEMU) $(QEMU_COMMON) $(STRESS_TIMING) -kernel $(STRESS_ELF)" $(BUILD)

# Tick-gap experiment: GAP="-DGAP_BUSY=0x1e ..." picks the variant (see fw/probe/tickgap.c).
GAP ?=
TICKGAP_ELF := $(BUILD)/tickgap.elf

tickgap: $(BUILD)/boot.o $(BUILD)/workloads.o $(BUILD)/hpm_$(HPM_BACKEND).o $(BUILD)/kprintf.o \
         $(BUILD)/power.o $(BUILD)/uart.o $(BUILD)/timer.o $(BUILD)/trap.o $(BUILD)/sample.o
	@$(CC) $(CFLAGS) $(GAP) -c fw/probe/tickgap.c -o $(BUILD)/tickgap.o
	@$(CC) $(LDFLAGS) $^ $(BUILD)/tickgap.o -lgcc -o $(TICKGAP_ELF)
	@EXPECT="GAP DONE" TEST_TIMEOUT=120 scripts/run_test.sh $(TICKGAP_ELF) "$(QEMU) $(QEMU_BASE) -kernel $(TICKGAP_ELF)" $(BUILD)

-include $(OBJS:.o=.d) $(BUILD)/probe_main.d

size: $(ELF)
	@$(SIZE) $(ELF)

run: $(ELF)
	$(QEMU) $(QEMU_FLAGS)

# A build whose rings are too small for its drain period, so samples really are dropped.
# Proves the drop path end to end, including the counts the decoder reads from frames.
test-drops:
	@$(MAKE) --no-print-directory test BUILD=build/drops TEST_TIMEOUT=$(TEST_TIMEOUT) \
	    FW_DEFS="-DRING_SLOTS=32 -DDRAIN_PERIOD_US=100000 -DEXPECT_DROPS=1" \
	    CHECK_FLAGS="--attempts 2000 --allow-drops"

# Extra -D flags for firmware builds, e.g. FW_DEFS="-DSAMPLE_HZ=100 -DTEST_TICKS=200".
# Use a separate BUILD= for each, because make does not notice a changed flag.
FW_DEFS ?=
CHECK_FLAGS ?= --attempts 2000

# Host tests run under ASan and UBSan, so memory errors fail them too.
HOST_FLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
              -fno-sanitize-recover=all -Ifw/src
HOST_SRCS  := fw/tests/test_sample.c fw/tests/hpm_fake.c fw/src/sample.c fw/src/sampler.c

host-test: $(ELF)
	@mkdir -p $(BUILD)
	@$(HOSTCC) $(HOST_FLAGS) -DSAMPLE_CHECKSUM=1 $(HOST_SRCS) -o $(BUILD)/test_sample
	@$(HOSTCC) $(HOST_FLAGS) -DSAMPLE_CHECKSUM=0 $(HOST_SRCS) -o $(BUILD)/test_sample_nochk
	@$(HOSTCC) $(HOST_FLAGS) fw/tests/test_ring.c -o $(BUILD)/test_ring
	@$(HOSTCC) $(HOST_FLAGS) fw/tests/test_wire.c fw/src/wire.c fw/src/sample.c -o $(BUILD)/test_wire
	@$(HOSTCC) $(HOST_FLAGS) fw/tests/gen_vectors.c fw/src/wire.c fw/src/sample.c -o $(BUILD)/gen_vectors
	@$(BUILD)/test_sample
	@$(BUILD)/test_sample_nochk
	@$(BUILD)/test_ring
	@$(BUILD)/test_wire
	@$(BUILD)/gen_vectors $(BUILD)/wire_vectors.bin $(BUILD)/wire_vectors.txt
	@HW_BUILD=$(BUILD) python3 -m pytest -q -p no:cacheprovider --ignore=host/test_db.py host
	@$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=thread -pthread -Ifw/src \
	    fw/tests/test_ring_tsan.c fw/src/sample.c -o $(BUILD)/test_ring_tsan
	@$(BUILD)/test_ring_tsan 2000000 64

test: host-test test-fw

# The firmware half of `make test`: boot, then check the telemetry stream.
test-fw: $(ELF)
	@TEST_TIMEOUT=$(TEST_TIMEOUT) scripts/run_test.sh $(ELF) "$(QEMU) $(QEMU_FLAGS)" $(BUILD)
	@python3 host/check_stream.py $(TELEMETRY) $(CHECK_FLAGS)

probe: $(PROBE_ELF)
	@EXPECT="PROBE DONE" TEST_TIMEOUT=120 scripts/run_test.sh $(PROBE_ELF) "$(QEMU) $(QEMU_BASE) -kernel $(PROBE_ELF)" $(BUILD)

debug: $(ELF)
	@echo ">>> QEMU is paused. In another terminal run:  make gdb"
	$(QEMU) $(QEMU_FLAGS) -S -gdb tcp::1234

gdb: $(ELF)
	$(GDB) -q $(ELF) -ex "set architecture riscv:rv64" -ex "target remote :1234"

dtb:
	@mkdir -p $(BUILD)
	$(QEMU) -machine sifive_u,dumpdtb=$(BUILD)/sifive_u.dtb -smp 5 -m 256M -nographic
	dtc -I dtb -O dts $(BUILD)/sifive_u.dtb -o $(BUILD)/sifive_u.dts
	@echo "wrote $(BUILD)/sifive_u.dts"

disasm: $(ELF)
	$(OBJDUMP) -d -S $(ELF) > $(BUILD)/hartwatch.lst
	@echo "wrote $(BUILD)/hartwatch.lst"

clean:
	rm -rf $(BUILD)
