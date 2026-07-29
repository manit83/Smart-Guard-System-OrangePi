CC ?= gcc
PKG_CONFIG ?= pkg-config
PYTHON ?= python3

BUILD_DIR := build
WEB_TARGET := $(BUILD_DIR)/smart-guard-web
COMMAND_TARGET := $(BUILD_DIR)/smart-guard-command
NOTIFIER_TARGET := $(BUILD_DIR)/smart-guard-notifier
WATCHDOG_TARGET := $(BUILD_DIR)/smart-guard-watchdog
THERMAL_TARGET := $(BUILD_DIR)/smart-guard-thermal-manager

CPPFLAGS := -Iinclude
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L

CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null)
MOSQUITTO_CFLAGS := $(shell $(PKG_CONFIG) --cflags libmosquitto 2>/dev/null)
MOSQUITTO_LIBS := $(shell $(PKG_CONFIG) --libs libmosquitto 2>/dev/null)
SQLITE_CFLAGS := $(shell $(PKG_CONFIG) --cflags sqlite3 2>/dev/null)
SQLITE_LIBS := $(shell $(PKG_CONFIG) --libs sqlite3 2>/dev/null)

WEB_OBJECTS := \
	$(BUILD_DIR)/web_server.o \
	$(BUILD_DIR)/telemetry.o \
	$(BUILD_DIR)/history_db.o \
	$(BUILD_DIR)/guard_state.o \
	$(BUILD_DIR)/mongoose.o

NOTIFIER_OBJECTS := \
	$(BUILD_DIR)/notifier.o \
	$(BUILD_DIR)/core.o \
	$(BUILD_DIR)/email_sender.o \
	$(BUILD_DIR)/mqtt_client.o \
	$(BUILD_DIR)/guard_state.o

WATCHDOG_OBJECTS := \
	$(BUILD_DIR)/watchdog.o \
	$(BUILD_DIR)/core.o \
	$(BUILD_DIR)/email_sender.o \
	$(BUILD_DIR)/history_db.o

THERMAL_OBJECTS := \
	$(BUILD_DIR)/thermal_manager.o \
	$(BUILD_DIR)/thermal_policy.o \
	$(BUILD_DIR)/core.o \
	$(BUILD_DIR)/email_sender.o

.PHONY: all clean check check-deps check-offline scripts-check syntax-offline

all: check-deps \
	$(WEB_TARGET) \
	$(COMMAND_TARGET) \
	$(NOTIFIER_TARGET) \
	$(WATCHDOG_TARGET) \
	$(THERMAL_TARGET)

check-deps:
	@$(PKG_CONFIG) --exists libcurl || { \
		echo "Missing dependency: libcurl4-openssl-dev"; exit 1; \
	}
	@$(PKG_CONFIG) --exists libmosquitto || { \
		echo "Missing dependency: libmosquitto-dev"; exit 1; \
	}
	@$(PKG_CONFIG) --exists sqlite3 || { \
		echo "Missing dependency: libsqlite3-dev"; exit 1; \
	}

$(WEB_TARGET): $(WEB_OBJECTS)
	$(CC) $^ -lssl -lcrypto $(SQLITE_LIBS) -o $@

$(COMMAND_TARGET): $(BUILD_DIR)/command_runner.o
	$(CC) $^ -o $@

$(NOTIFIER_TARGET): $(NOTIFIER_OBJECTS)
	$(CC) $^ $(CURL_LIBS) $(MOSQUITTO_LIBS) -o $@

$(WATCHDOG_TARGET): $(WATCHDOG_OBJECTS)
	$(CC) $^ $(CURL_LIBS) $(SQLITE_LIBS) -o $@

$(THERMAL_TARGET): $(THERMAL_OBJECTS)
	$(CC) $^ $(CURL_LIBS) -o $@

$(BUILD_DIR)/email_sender.o: src/email_sender.c include/email_sender.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CURL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/mqtt_client.o: src/mqtt_client.c include/mqtt_client.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MOSQUITTO_CFLAGS) -c $< -o $@

$(BUILD_DIR)/history_db.o: src/history_db.c include/history_db.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SQLITE_CFLAGS) -c $< -o $@

$(BUILD_DIR)/mongoose.o: src/mongoose.c include/mongoose.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -Iinclude -DMG_TLS=MG_TLS_OPENSSL -DMG_ENABLE_DIRLIST=0 \
		-O2 -std=gnu11 -D_DEFAULT_SOURCE -c $< -o $@

$(BUILD_DIR)/web_server.o: src/web_server.c include/mongoose.h \
		include/telemetry.h include/guard_state.h include/history_db.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -DMG_TLS=MG_TLS_OPENSSL -DMG_ENABLE_DIRLIST=0 \
		$(CFLAGS) $(SQLITE_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test-core: tests/test_core.c src/core.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test-guard-state: tests/test_guard_state.c src/guard_state.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test-history-db: tests/test_history_db.c src/history_db.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -Itests/stubs $(CPPFLAGS) $(CFLAGS) $^ -lsqlite3 -o $@

$(BUILD_DIR)/test-telemetry: tests/test_telemetry.c src/telemetry.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test-thermal-policy: \
		tests/test_thermal_policy.c src/thermal_policy.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

syntax-offline:
	$(CC) -Itests/stubs $(CPPFLAGS) $(CFLAGS) -fsyntax-only \
		src/core.c src/command_runner.c src/email_sender.c \
		src/mqtt_client.c src/notifier.c \
		src/guard_state.c src/history_db.c src/watchdog.c \
		src/thermal_manager.c src/thermal_policy.c
	$(CC) -Itests/stubs $(CPPFLAGS) $(CFLAGS) \
		-DMG_TLS=MG_TLS_OPENSSL -DMG_ENABLE_DIRLIST=0 -fsyntax-only \
		src/web_server.c src/telemetry.c

scripts-check:
	bash -n scripts/install.sh
	bash -n scripts/set_secret.sh
	bash -n scripts/camera_probe.sh
	bash -n scripts/camera_ready.sh
	bash -n scripts/generate_tls_cert.sh
	bash -n scripts/vision_start.sh
	bash -n pc/setup_mosquitto_pc.sh
	bash -n pc/monitor_mqtt.sh
	bash -n tests/test_final_layout.sh
	bash -n tests/test_api.sh
	bash -n tests/test_camera.sh
	bash -n tests/test_vision.sh
	@if command -v node >/dev/null 2>&1; then \
		node --check web/dashboard.js; \
	fi

check-offline: syntax-offline scripts-check \
		$(BUILD_DIR)/test-core \
		$(BUILD_DIR)/test-guard-state \
		$(BUILD_DIR)/test-telemetry \
		$(BUILD_DIR)/test-thermal-policy
	./$(BUILD_DIR)/test-core
	./$(BUILD_DIR)/test-guard-state
	./$(BUILD_DIR)/test-telemetry
	./$(BUILD_DIR)/test-thermal-policy
	$(PYTHON) tests/test_vision_unit.py
	./tests/test_final_layout.sh

check: check-deps check-offline all $(BUILD_DIR)/test-history-db
	./$(BUILD_DIR)/test-history-db

clean:
	rm -rf $(BUILD_DIR)
