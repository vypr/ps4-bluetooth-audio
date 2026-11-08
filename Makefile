# PS4 Bluetooth Audio V14 — payload-only reproducible build.
# `make payload`, `make test`, `make verify`; no PKG target in this source snapshot.
CLANG ?= clang
LLD ?= ld.lld
PYTHON ?= python3

BUILD := build
OBJ := $(BUILD)/bta-app
PAYLOAD := $(BUILD)/BTA-Audio.bin
RELEASE := BTA-Audio-FW14-V14-HDMI-UNLOCK.bin

V7_SPACEBUDS_RECOVER ?= 1
V8_FREEPODS_GAIN_PCT ?= 130
V8_PACE_CATCHUP ?= 0
PAYLOAD_FLAGS := -DBTA_V7_SPACEBUDS_RECOVER=$(V7_SPACEBUDS_RECOVER) -DBTA_V8_FREEPODS_GAIN_PCT=$(V8_FREEPODS_GAIN_PCT) -DBTA_V8_PACE_CATCHUP=$(V8_PACE_CATCHUP) --target=x86_64-pc-freebsd12-elf -O2 -fPIE -ffreestanding -fno-builtin \
 -fno-stack-protector -mno-red-zone -fno-asynchronous-unwind-tables \
 -Wall -Wextra -Werror -I $(OBJ)
APP_HEADERS := $(wildcard app/*.h)
PAYLOAD_OBJS := $(addprefix $(OBJ)/,launcher.o worker.o bt_patch.o bt_devices.o fw14_slots.o)

.PHONY: all payload test verify clean
all: payload
payload: $(PAYLOAD)

$(OBJ):
	mkdir -p $@

$(OBJ)/web_page.h: app/web.html scripts/embed.py | $(OBJ)
	$(PYTHON) scripts/embed.py $< $@ web_page

$(OBJ)/launcher.o $(OBJ)/worker.o: $(OBJ)/%.o: app/%.c $(APP_HEADERS) $(OBJ)/web_page.h
	$(CLANG) $(PAYLOAD_FLAGS) -c $< -o $@

$(OBJ)/bt_devices.o $(OBJ)/bt_patch.o $(OBJ)/fw14_slots.o: $(OBJ)/%.o: app/%.c $(APP_HEADERS) | $(OBJ)
	$(CLANG) $(PAYLOAD_FLAGS) -mgeneral-regs-only -c $< -o $@

$(OBJ)/BTA-Audio.debug.elf: $(PAYLOAD_OBJS) app/payload.ld
	$(LLD) -T app/payload.ld $(PAYLOAD_OBJS) -o $@

$(PAYLOAD): $(PAYLOAD_OBJS) app/payload.ld $(OBJ)/BTA-Audio.debug.elf
	mkdir -p $(BUILD)
	$(LLD) --oformat=binary -T app/payload.ld $(PAYLOAD_OBJS) -o $@

test:
	bash tools/test-v14-core.sh

verify: payload
	sha256sum -c SHA256SUMS
	cmp $(PAYLOAD) $(RELEASE)
	@echo 'V14 payload matches release binary byte for byte.'

clean:
	rm -rf $(BUILD)
