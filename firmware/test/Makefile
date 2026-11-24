# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
CXX = c++
.DEFAULT_GOAL := test
ARDUINOJSON ?= $(firstword $(wildcard ../ventanal/.pio/libdeps/*/ArduinoJson/src) ../ventanal/.pio/libdeps/bench/ArduinoJson/src)
SODIUM_CFLAGS ?= $(shell pkg-config --cflags libsodium 2>/dev/null)
SODIUM_LIBS ?= $(shell pkg-config --libs libsodium 2>/dev/null || echo -lsodium)
JPEG_CFLAGS ?= $(shell pkg-config --cflags libjpeg 2>/dev/null)
JPEG_LIBS ?= $(shell pkg-config --libs libjpeg 2>/dev/null)
CPPFLAGS += -DVENTANAL_ALLOW_TEST_KEY=1
# These existing suites exercise unpaired bench transport and test-key reporting.
build/test_lcd43b_sync build/test_net build/test_release: CPPFLAGS += -DVENTANAL_HOST_TEST_FORCE_TEST_KEY=1
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -pedantic

.PHONY: test clean check_standalone_screens check_lcd43b_chrome check_lcd43b_pie check_lcd43b_display_owner check-arduinojson check-hardening

# Validate the checked-in raster against the current server implementation.
PYTHON ?= ../../server/.venv/bin/python
check_lcd43b_chrome:
	$(PYTHON) ../lcd43b/tools/lcars_geometry.py --check
	$(PYTHON) ../../tools/aa_fonts.py --check

check_lcd43b_pie:
	@rc=0; git -C ../.. grep -niE \
	    'esp[-_]dsp|(^|[^[:alnum:]_])(ee[._][a-z]|esp32s3_ee_|__builtin_xtensa|xt_(vec|ivp)|ivp_|ae_|dsp[ms]_|xchal_cp3_(store|load)|(rur|wur)\.(accx|qacc|sar_byte|fft_bit_width|ua_state)|(st|ld)\.qr)' \
	    -- firmware ':!firmware/lcd43b/scanout_dim_s3.S' ':!firmware/test/Makefile' || rc=$$?; \
	    test "$$rc" -eq 1

.PHONY: test-lcd-round4
test-lcd-round4:
	$(PYTHON) test_lcd_round4.py

test: test-lcd-round4 test-review6-wake

test: check_lcd43b_pie check-arduinojson check-hardening check_lcd43b_display_owner check_standalone_screens build/test_lcd43b_wifi_roam

test: build/test_staleness build/test_https_get build/test_lcd43b_installation build/test_portal_form build/test_portal_flow build/test_locate build/test_local_weather build/test_local_solar build/test_resume build/test_resume_store build/test_fcc build/test_local_text build/test_local_art build/test_settings build/test_ota build/test_release build/test_lcd43b_owner build/test_lcd43b_power_session build/test_lcd43b_power check_lcd43b_chrome build/test_lcd43b_settings_page build/test_lcd43b_setup build/test_lcd43b_settings build/test_lcd43b_clock build/test_lcd43b_battery build/test_budget build/test_extras build/test_longpress build/test_solar build/test_battcal_wake build/test_battcal build/test_secure_transport build/test_security build/test_statelost build/test_spares build/test_owner build/test_ponretry_stall build/test_ponretry build/test_boottime build/test_bootconfirm build/test_pwren build/test_stall build/test_logic build/test_cache build/test_cache_abort build/test_net build/test_overlay build/test_wake build/test_lcd43b_gesture build/test_lcd43b_frame build/test_lcd43b_manifest build/test_lcd43b_overlay build/test_lcd43b_panel build/test_lcd43b_diag build/test_lcd43b_screens build/test_lcd43b_touch build/test_lcd43b_sync build/test_lcd43b_persist build/test_lcd43b_display_owner
	./build/test_https_get
	./build/test_lcd43b_installation
	./build/test_portal_form
	./build/test_portal_flow
	./build/test_locate
	./build/test_local_weather
	./build/test_local_solar
	./build/test_resume
	./build/test_staleness
	./build/test_resume_store
	./build/test_fcc
	./build/test_local_text
	./build/test_local_art
	$(PYTHON) ../lcd43b/tools/standalone_assets.py --check
	$(PYTHON) ../lcd43b/tools/standalone_expected.py --check
	python3 ../../tools/gen_tz_posix.py --check
	./build/test_lcd43b_owner
	./build/test_lcd43b_power_session
	./build/test_lcd43b_power
	./build/test_lcd43b_settings_page
	./build/test_lcd43b_setup
	./build/test_lcd43b_settings
	./build/test_lcd43b_clock
	./build/test_lcd43b_battery
	./build/test_settings
	./build/test_ota
	./build/test_release
	./build/test_budget
	./build/test_extras
	./build/test_longpress
	./build/test_solar
	./build/test_battcal_wake
	./build/test_battcal
	./build/test_secure_transport
	./build/test_security
	./build/test_statelost
	./build/test_spares
	./build/test_owner
	./build/test_ponretry_stall
	./build/test_ponretry
	./build/test_boottime
	./build/test_bootconfirm
	./build/test_pwren
	./build/test_stall
	TZ=America/New_York ./build/test_logic
	./build/test_cache
	./build/test_cache_abort
	./build/test_net
	./build/test_net_owner
	./build/test_overlay
	./build/test_wake
	./build/test_lcd43b_gesture
	./build/test_lcd43b_frame
	./build/test_lcd43b_manifest
	./build/test_lcd43b_overlay
	./build/test_lcd43b_panel
	./build/test_lcd43b_diag
	./build/test_lcd43b_screens
	./build/test_lcd43b_touch
	./build/test_lcd43b_sync
	./build/test_lcd43b_sync_owner
	./build/test_lcd43b_wifi_roam
	./build/test_lcd43b_persist
	./build/test_lcd43b_display_owner

build/test_release build/test_net build/test_security build/test_secure_transport: | check-arduinojson

check-arduinojson:
	@test -f "$(ARDUINOJSON)/ArduinoJson.h" || { \
		echo "ArduinoJson headers not found. From the repo root, run:" >&2; \
		echo "  firmware/.venv/bin/pio pkg install -d firmware/ventanal -e bench" >&2; \
		echo "Or pass ARDUINOJSON=/path/to/ArduinoJson/src to make." >&2; \
		exit 1; \
	}

build/test_settings: test_settings.cpp ../ventanal/settings.h ../ventanal/security_config.h ../ventanal/config.example.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_settings.cpp $(LDFLAGS) -o $@

build/test_ota: test_ota.cpp ../common/release_info.h $(wildcard ../common/resume/*.h fakes/*.h) ../common/resume/fs_store.cpp ../common/release.h ../common/ota_state.h ../lcd43b/ota.h ../ventanal/ota.h ../ventanal/logic.cpp ../ventanal/logic.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Ifakes -UNDEBUG test_ota.cpp ../common/resume/fs_store.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_release: test_release.cpp ../common/release_info.h ../common/release.h ../common/release_key.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG $(SODIUM_CFLAGS) -I"$(ARDUINOJSON)" test_release.cpp $(LDFLAGS) $(SODIUM_LIBS) -o $@

build/test_logic: test_logic.cpp ../ventanal/logic.cpp ../ventanal/logic.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_logic.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

clean:
	rm -rf build

.PHONY: test-epaper-cache
test-epaper-cache: build/test_cache build/test_cache_abort
	./build/test_cache
	./build/test_cache_abort

.PHONY: test-epaper-wake
test: test-epaper-wake
test-epaper-wake: build/test_wake
	@result=0; for case in weather_fetched_first_when_due first_frame_uses_remaining_budget \
		later_frames_get_leftover_budget sd_not_probed_when_nothing_due \
		no_card_result_cached_until_cold_boot_or_24h complete_screen_shown_instead_of_waiting \
		displayed_missing_screen_downloads_before_spares; do \
		./build/test_wake $$case || result=1; \
	done; exit $$result

EPAPER_CACHE_STORAGE := ../common/resume/fs_store.cpp ../common/resume/storage.cpp build/ventanal_tinfl.o

# The e-paper firmware compiles the vendored tinfl as C++ (tinfl.cpp); so do the host tests.
build/ventanal_tinfl.o: ../ventanal/tinfl.cpp $(wildcard ../lcd43b/src/miniz/*) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) -std=c++17 -w -c ../ventanal/tinfl.cpp -o $@
build/test_cache build/test_cache_abort build/test_secure_transport: $(EPAPER_CACHE_STORAGE)
build/test_cache build/test_cache_abort build/test_secure_transport build/test_net: ../ventanal/http.h ../ventanal/net.h $(wildcard ../common/resume/*.h)

build/test_cache: test_cache.cpp ../ventanal/cache.cpp ../ventanal/cache.h ../ventanal/config.example.h ../ventanal/http.h ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Wframe-larger-than=2048 -Ifakes test_cache.cpp ../ventanal/cache.cpp ../ventanal/logic.cpp $(EPAPER_CACHE_STORAGE) $(LDFLAGS) -lz -o $@

build/test_cache_abort: test_cache.cpp ../ventanal/cache.cpp ../ventanal/cache.h ../ventanal/config.example.h ../ventanal/http.h ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -DVENTANAL_ABORT_AFTER_BYTES=100000 -Wframe-larger-than=2048 -Ifakes test_cache.cpp ../ventanal/cache.cpp ../ventanal/logic.cpp $(EPAPER_CACHE_STORAGE) $(LDFLAGS) -lz -o $@

build/test_net: test_net.cpp ../ventanal/quiet.h ../ventanal/net.cpp ../ventanal/net.h ../ventanal/config.example.h ../ventanal/http.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes -I"$(ARDUINOJSON)" test_net.cpp ../ventanal/net.cpp $(LDFLAGS) -o $@

build/test_overlay: ../common/staleness.h ../ventanal/shade.h ../ventanal/shade_tables.h test_overlay.cpp ../ventanal/overlay.cpp ../ventanal/overlay.h $(wildcard ../ventanal/fonts/*.h) $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes test_overlay.cpp ../ventanal/overlay.cpp $(LDFLAGS) -o $@

build/test_wake: ../common/staleness.h test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.cpp ../ventanal/logic.cpp ../ventanal/logic.h ../ventanal/net.h ../ventanal/cache.h ../ventanal/overlay.h ../ventanal/config.example.h $(wildcard fakes/*.h) fakes/driver/rtc_io.h fakes/driver/gpio.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_wake.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_stall: test_stall.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.cpp ../ventanal/logic.cpp $(wildcard fakes/*.h) fakes/driver/gpio.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_stall.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_pwren: test_stall.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.cpp ../ventanal/logic.cpp $(wildcard fakes/*.h) fakes/driver/gpio.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -DVENTANAL_PWRENTEST=1 -Ifakes test_stall.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_boottime build/test_bootconfirm: test_boottime.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.cpp ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST $(if $(filter build/test_bootconfirm,$@),-DVENTANAL_BOOT_CONFIRM_MS=7000,) -Ifakes test_boottime.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_ponretry: test_ponretry.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.cpp ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_ponretry.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_ponretry_stall: test_ponretry_stall.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.cpp ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_ponretry_stall.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_owner: test_owner.cpp ../ventanal/owner.h ../ventanal/net.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes test_owner.cpp $(LDFLAGS) -o $@

build/test_spares: test_spares.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.cpp ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_spares.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_statelost: test_statelost.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.cpp ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_statelost.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_security: test_security.cpp ../ventanal/settings.h ../ventanal/security.h ../ventanal/manifest_security.h ../lcd43b/transport.h ../lcd43b/manifest.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes -I"$(ARDUINOJSON)" test_security.cpp $(LDFLAGS) -o $@

# Shared transport headers also invalidate the net/cache builds.
build/test_net build/test_cache build/test_cache_abort build/test_secure_transport: ../ventanal/settings.h ../ventanal/security.h ../ventanal/security_config.h ../ventanal/manifest_security.h

build/test_secure_transport: fixtures/extras-server-manifest.json fixtures/manifest-controls.json test_secure_transport.cpp secure_config.h ../ventanal/security.h ../ventanal/manifest_security.h ../ventanal/net.cpp ../ventanal/cache.cpp ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -DVENTANAL_SECURITY_CONFIG=../test/secure_config.h -Ifakes -I"$(ARDUINOJSON)" test_secure_transport.cpp ../ventanal/net.cpp ../ventanal/cache.cpp ../ventanal/logic.cpp $(EPAPER_CACHE_STORAGE) $(LDFLAGS) -o $@


build/test_battcal: test_battcal.cpp ../ventanal/battery.h ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes test_battcal.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

# Calibration header participates in wake and transport builds.
build/test_solar build/test_net build/test_secure_transport build/test_wake build/test_statelost build/test_spares build/test_stall build/test_pwren build/test_boottime build/test_bootconfirm build/test_ponretry build/test_ponretry_stall: ../ventanal/battery.h

build/test_battcal_wake: test_battcal_wake.cpp test_wake.cpp ../ventanal/ventanal.cpp ../ventanal/battery.h ../ventanal/logic.cpp ../ventanal/owner.h ../ventanal/net.h ../ventanal/logic.h ../ventanal/cache.h ../ventanal/quiet.h ../ventanal/overlay.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_battcal_wake.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_solar: ../ventanal/battery.h ../ventanal/owner.h ../ventanal/cache.h ../ventanal/quiet.h ../ventanal/overlay.h test_solar.cpp test_wake.cpp ../ventanal/ventanal.cpp ../ventanal/logic.cpp ../ventanal/logic.h ../ventanal/net.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_solar.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_longpress: test_longpress.cpp test_wake.cpp ../ventanal/ventanal.cpp ../ventanal/owner.h ../ventanal/logic.h ../ventanal/logic.cpp ../ventanal/battery.h ../ventanal/quiet.h ../ventanal/cache.h ../ventanal/overlay.h ../ventanal/net.h ../ventanal/security_config.h ../ventanal/config.example.h $(wildcard fakes/*.h) fakes/driver/gpio.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_longpress.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_extras: test_extras.cpp test_solar.cpp test_wake.cpp ../ventanal/ventanal.cpp ../ventanal/net.h ../ventanal/logic.h ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_extras.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_budget: test_budget.cpp test_wake.cpp ../ventanal/ventanal.cpp ../ventanal/owner.h ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_budget.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

# lcd43b device profile: host-side units. gesture/frame/manifest/overlay are
# pure C++; panel compiles the production header against the esp_lcd fakes.
build/test_lcd43b_gesture: test_lcd43b_gesture.cpp ../lcd43b/gesture.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_gesture.cpp $(LDFLAGS) -o $@

build/test_lcd43b_panel: test_lcd43b_panel.cpp ../lcd43b/panel.cpp ../lcd43b/panel.h ../lcd43b/scanout.h ../lcd43b/scanout_diag.h fakes/Arduino.h fakes/esp_lcd_panel_ops.h fakes/esp_lcd_panel_rgb.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes -Wframe-larger-than=2048 test_lcd43b_panel.cpp ../lcd43b/panel.cpp $(LDFLAGS) -o $@

build/test_lcd43b_diag: test_lcd43b_diag.cpp ../lcd43b/diag.h ../lcd43b/gesture.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_diag.cpp $(LDFLAGS) -o $@

build/test_lcd43b_screens: test_lcd43b_screens.cpp ../lcd43b/screens.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_screens.cpp $(LDFLAGS) -o $@


build/test_lcd43b_frame: test_lcd43b_frame.cpp ../lcd43b/frame.h ../lcd43b/sha256.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_frame.cpp $(LDFLAGS) -o $@

build/test_lcd43b_manifest: test_lcd43b_manifest.cpp ../lcd43b/manifest.h ../lcd43b/frame.h ../lcd43b/sha256.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I"$(ARDUINOJSON)" test_lcd43b_manifest.cpp $(LDFLAGS) -o $@

build/test_lcd43b_overlay: ../common/staleness.h $(wildcard ../lcd43b/*.h) test_lcd43b_overlay.cpp ../lcd43b/overlay.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_overlay.cpp $(LDFLAGS) -o $@

# GT911 driver + classifier integration: the fake controller (fakes/gt911)
# sits behind a fake TwoWire, so the production Gt911 register protocol and
# the 10-15 ms report cadence are exercised for real.
build/test_lcd43b_touch: test_lcd43b_touch.cpp ../lcd43b/touch.h ../lcd43b/gt911.h ../lcd43b/gesture.h fakes/gt911/Wire.h fakes/Arduino.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes/gt911 -Ifakes test_lcd43b_touch.cpp $(LDFLAGS) -o $@

# The production downloader/refresh engine. AddressSanitizer doubles as an
# overflow tripwire: a read that ever passed the frame end would abort.
MINIZ_DIR = ../lcd43b/src/miniz

build/miniz_tinfl.o: $(MINIZ_DIR)/miniz_tinfl.c $(wildcard $(MINIZ_DIR)/*.h) Makefile
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -fsanitize=address -c $< -o $@

.PHONY: check-libjpeg
check-libjpeg:
	@pkg-config --exists libjpeg || { echo "libjpeg not found by pkg-config" >&2; exit 1; }

build/test_lcd43b_sync: test_lcd43b_sync.cpp fake_tjpgd.cpp fakes/rom/tjpgd.h ../lcd43b/host_jpeg.cpp ../lcd43b/frame_stream.h ../lcd43b/sync.cpp ../lcd43b/sync.h ../lcd43b/log.h ../lcd43b/persist.h ../lcd43b/manifest.h ../lcd43b/frame.h ../lcd43b/sha256.h ../lcd43b/screens.h $(wildcard fakes/*.h) $(wildcard ../common/resume/*.h) ../common/resume/fs_store.cpp ../common/resume/storage.cpp build/miniz_tinfl.o Makefile | check-libjpeg
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -fsanitize=address -DVENTANAL_HOST_TEST -Ifakes -I"$(ARDUINOJSON)" $(JPEG_CFLAGS) test_lcd43b_sync.cpp fake_tjpgd.cpp ../lcd43b/host_jpeg.cpp ../lcd43b/sync.cpp ../common/resume/fs_store.cpp ../common/resume/storage.cpp build/miniz_tinfl.o $(LDFLAGS) -fsanitize=address -lz $(JPEG_LIBS) -o $@

# Persisted state (quiet hours, publish shape, cache ids, selection).
build/test_lcd43b_persist: ../lcd43b/clock.h test_lcd43b_persist.cpp ../lcd43b/persist.h ../lcd43b/manifest.h ../lcd43b/screens.h ../lcd43b/frame.h ../lcd43b/sha256.h fakes/Preferences.h fakes/LittleFS.h fakes/Arduino.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes -I"$(ARDUINOJSON)" test_lcd43b_persist.cpp $(LDFLAGS) -o $@

build/test_lcd43b_clock: test_lcd43b_clock.cpp $(wildcard ../lcd43b/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_clock.cpp $(LDFLAGS) -o $@

build/test_lcd43b_battery: ../ventanal/logic.cpp ../ventanal/logic.h test_lcd43b_battery.cpp $(wildcard ../lcd43b/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_battery.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_lcd43b_settings: test_lcd43b_settings.cpp $(wildcard ../lcd43b/*.h) $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes -I"$(ARDUINOJSON)" test_lcd43b_settings.cpp $(LDFLAGS) -o $@

build/test_lcd43b_screens build/test_lcd43b_sync build/test_lcd43b_persist build/test_lcd43b_panel: ../lcd43b/settings.h

build/test_lcd43b_settings_page: fixtures/lcd43b-settings-chrome.rle test_lcd43b_settings_page.cpp ../common/call_state.h $(wildcard ../lcd43b/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_settings_page.cpp $(LDFLAGS) -o $@

build/test_lcd43b_power: test_lcd43b_power.cpp $(wildcard ../lcd43b/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_power.cpp $(LDFLAGS) -o $@

build/test_lcd43b_power_session: test_lcd43b_power_session.cpp ../lcd43b/power_session.h ../lcd43b/power.h Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_power_session.cpp $(LDFLAGS) -o $@

build/test_lcd43b_owner: test_lcd43b_owner.cpp ../lcd43b/owner.h ../ventanal/owner.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes test_lcd43b_owner.cpp $(LDFLAGS) -o $@

build/test_lcd43b_wifi_roam: test_lcd43b_wifi_roam.cpp ../lcd43b/wifi_roam.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_wifi_roam.cpp $(LDFLAGS) -o $@

build/test_lcd43b_sync: ../lcd43b/inflate.h ../lcd43b/deferred_save.h ../lcd43b/wifi_roam.h
build/test_lcd43b_persist: ../lcd43b/deferred_save.h
build/test_lcd43b_owner: ../lcd43b/frame.h ../lcd43b/sha256.h

build/test_budget build/test_extras build/test_longpress build/test_solar build/test_battcal_wake build/test_statelost build/test_spares build/test_ponretry_stall build/test_ponretry build/test_boottime build/test_bootconfirm build/test_pwren build/test_stall build/test_wake: fakes/freertos/FreeRTOS.h fakes/freertos/task.h

SECURITY_HEADERS = ../ventanal/security_protocol.h ../ventanal/manifest_signature.h
build/test_security build/test_secure_transport build/test_lcd43b_sync: $(SECURITY_HEADERS)
build/test_lcd43b_sync: ../lcd43b/build_config.h ../lcd43b/host_request.h ../lcd43b/host_transport.h ../lcd43b/installation.h ../lcd43b/transport.h
build/test_security build/test_lcd43b_manifest build/test_lcd43b_sync build/test_lcd43b_persist: ../common/ota_state.h

build/test_portal_form: test_portal_form.cpp ../common/setup_portal/form.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_portal_form.cpp $(LDFLAGS) -o $@
build/test_portal_flow: test_portal_flow.cpp fakes/portal_net.h $(wildcard ../common/setup_portal/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_portal_flow.cpp $(LDFLAGS) -o $@
build/test_locate: test_locate.cpp ../common/setup_portal/locate.h ../common/setup_portal/location.h ../common/tz_posix.h Makefile | check-arduinojson
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I"$(ARDUINOJSON)" test_locate.cpp $(LDFLAGS) -o $@
build/test_lcd43b_setup: test_lcd43b_setup.cpp ../common/fcc.h ../common/call_state.h $(wildcard ../lcd43b/*.h) $(wildcard ../common/setup_portal/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_lcd43b_setup.cpp $(LDFLAGS) -o $@
build/test_local_weather: test_local_weather.cpp $(wildcard ../lcd43b/local/*.h) $(wildcard fixtures/standalone/*.json) Makefile | check-arduinojson
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I"$(ARDUINOJSON)" test_local_weather.cpp $(LDFLAGS) -o $@
build/test_local_solar: test_local_solar.cpp $(wildcard ../lcd43b/local/*.h) $(wildcard fixtures/standalone/*) Makefile | check-arduinojson
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I"$(ARDUINOJSON)" test_local_solar.cpp $(LDFLAGS) -o $@
build/test_resume: test_resume.cpp ../common/resume/resume.h ../common/resume/partial_store.h ../lcd43b/sha256.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_resume.cpp $(LDFLAGS) -o $@
build/test_fcc: test_fcc.cpp ../common/fcc.h ../common/call_state.h Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG $< $(LDFLAGS) -o $@

build/test_local_text: test_local_text.cpp $(wildcard ../lcd43b/local/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I../lcd43b test_local_text.cpp $(LDFLAGS) -o $@
# The board's standalone screens, held to the server's pixels.
build/render_local_screens: render_local_screens.cpp $(wildcard ../lcd43b/local/*.h) $(wildcard fixtures/standalone/*) Makefile | check-arduinojson
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O2 -I"$(ARDUINOJSON)" render_local_screens.cpp $(LDFLAGS) -o $@
check_standalone_screens: build/render_local_screens
	mkdir -p build/standalone
	$(PYTHON) ../lcd43b/tools/standalone_compare.py build/standalone --inputs
	./build/render_local_screens build/standalone
	$(PYTHON) ../lcd43b/tools/standalone_compare.py build/standalone
build/test_local_art: test_local_art.cpp $(wildcard ../lcd43b/local/*.h) ../common/resume/resume.h ../common/resume/partial_store.h Makefile | check-arduinojson
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I"$(ARDUINOJSON)" test_local_art.cpp $(LDFLAGS) -o $@
build/test_lcd43b_installation: test_lcd43b_installation.cpp ../lcd43b/installation.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_lcd43b_installation.cpp $(LDFLAGS) -o $@
build/test_budget build/test_extras build/test_longpress build/test_solar build/test_battcal_wake build/test_statelost build/test_spares build/test_ponretry_stall build/test_ponretry build/test_boottime build/test_bootconfirm build/test_pwren build/test_stall build/test_wake: fakes/freertos/FreeRTOS.h fakes/freertos/task.h ../ventanal/settings.h ../ventanal/config.example.h ../ventanal/security_config.h

build/test_cache build/test_cache_abort build/test_net build/test_secure_transport build/test_owner build/test_budget build/test_extras build/test_longpress build/test_solar build/test_battcal_wake build/test_statelost build/test_spares build/test_ponretry_stall build/test_ponretry build/test_boottime build/test_bootconfirm build/test_pwren build/test_stall build/test_wake: fakes/freertos/FreeRTOS.h fakes/freertos/task.h ../ventanal/settings.h ../ventanal/config.example.h ../ventanal/security_config.h ../common/installation.h

build/test_cache build/test_cache_abort build/test_net build/test_secure_transport build/test_owner build/test_budget build/test_extras build/test_longpress build/test_solar build/test_battcal_wake build/test_statelost build/test_spares build/test_ponretry_stall build/test_ponretry build/test_boottime build/test_bootconfirm build/test_pwren build/test_stall build/test_wake: ../ventanal/ota.h ../common/release_key.h ../common/ota_state.h

build/test_settings build/test_security build/test_net build/test_cache build/test_cache_abort build/test_secure_transport: ../common/installation.h

build/test_lcd43b_installation build/test_lcd43b_sync: ../common/installation.h
build/test_lcd43b_sync: ../common/release_key.h

check-hardening:
	CXX="$(CXX)" ./check_hardening.sh

build/test_lcd43b_display_owner: test_lcd43b_display_owner.cpp ../lcd43b/display_owner.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_display_owner.cpp $(LDFLAGS) -o $@

# Only the display owner's adapter may switch the backlight (see display_owner.h).
check_lcd43b_display_owner:
	@rc=0; git -C ../.. grep -nE 'setBacklight\(' -- firmware/lcd43b \
	    ':!firmware/lcd43b/app_display.h' ':!firmware/lcd43b/ch422g.h' \
	    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' \
	    | grep -vE '^firmware/lcd43b/app_buffers.h:[0-9]+:bool setBacklight\(bool on\);' && rc=1; \
	    test "$$rc" -eq 0 || { echo "Direct backlight calls bypass the display owner" >&2; exit 1; }


build/test_https_get: test_https_get.cpp ../common/https_get.cpp ../common/https_get.h ../lcd43b/public_transport.h ../common/resume/resume.h ../common/resume/partial_store.h fakes/Arduino.h fakes/esp_http_client.h fakes/esp_crt_bundle.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_FIRMWARE_VERSION='"test"' -Ifakes test_https_get.cpp ../common/https_get.cpp $(LDFLAGS) -o $@

build/test_resume_store: test_resume_store.cpp ../common/release_info.h $(wildcard ../common/resume/*.h ../common/resume/*.cpp fakes/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Ifakes -UNDEBUG test_resume_store.cpp $(wildcard ../common/resume/*.cpp) $(LDFLAGS) -o $@

build/test_staleness: test_staleness.cpp ../common/staleness.h Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_staleness.cpp $(LDFLAGS) -o $@

.PHONY: test-lcd-storage test-resume-store
test-lcd-storage: build/test_lcd_storage
	./build/test_lcd_storage

test-resume-store: build/test_resume_store
	./build/test_resume_store

test: test-lcd-storage

build/test_lcd_storage: test_lcd_storage.cpp ../lcd43b/local/art_state.h $(wildcard ../lcd43b/download_*.h ../common/resume/*.h fakes/*.h) ../common/resume/fs_store.cpp Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Ifakes -UNDEBUG test_lcd_storage.cpp ../common/resume/fs_store.cpp $(LDFLAGS) -o $@

.PHONY: test-lcd-round4-integration
test-lcd-round4-integration: test-lcd-round4 test-lcd-storage test-resume-store build/test_lcd43b_sync build/test_ota build/test_lcd43b_settings_page
	./build/test_lcd43b_sync
	./build/test_ota
	./build/test_lcd43b_settings_page

.PHONY: test-secure-transport
test-secure-transport: build/test_secure_transport
	./build/test_secure_transport

.PHONY: test-review6-wake
test-review6-wake: build/test_wake
	./build/test_wake invalid_cached_frame_falls_back_and_redownloads
	./build/test_wake zlib_frame_not_started_without_inflate_reserve
	./build/test_wake zlib_frame_gets_remaining_less_reserve
	./build/test_wake failed_scheduled_screen_is_not_retried

# Exercise production owner-key decisions independently of the bench suite.
build/test_lcd43b_sync_owner: test_lcd43b_sync.cpp fake_tjpgd.cpp ../lcd43b/sync.cpp ../common/release_key.h build/miniz_tinfl.o Makefile | check-libjpeg
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -fsanitize=address -DVENTANAL_HOST_TEST -DVENTANAL_OWNER_KEY_TEST -Ifakes -I"$(ARDUINOJSON)" $(JPEG_CFLAGS) test_lcd43b_sync.cpp fake_tjpgd.cpp ../lcd43b/host_jpeg.cpp ../lcd43b/sync.cpp ../common/resume/fs_store.cpp ../common/resume/storage.cpp build/miniz_tinfl.o $(LDFLAGS) -fsanitize=address -lz $(JPEG_LIBS) -o $@

build/test_net_owner: test_net.cpp ../ventanal/net.cpp ../common/release_key.h Makefile | check-arduinojson
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -DVENTANAL_OWNER_KEY_TEST -Ifakes -I"$(ARDUINOJSON)" test_net.cpp ../ventanal/net.cpp $(LDFLAGS) -o $@

test: build/test_lcd43b_sync_owner build/test_net_owner
