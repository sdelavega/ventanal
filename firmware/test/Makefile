# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
CXX = c++
.DEFAULT_GOAL := test
ARDUINOJSON ?= $(HOME)/Library/Arduino15/ventanal-sketchbook/libraries/ArduinoJson/src
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -pedantic

.PHONY: test clean check_lcd43b_chrome

# Validate the checked-in raster against the current server implementation.
PYTHON ?= ../../server/.venv/bin/python
check_lcd43b_chrome:
	$(PYTHON) ../lcd43b/tools/lcars_geometry.py --check
	$(PYTHON) ../../tools/aa_fonts.py --check

test: build/test_lcd43b_power_session build/test_lcd43b_power check_lcd43b_chrome build/test_lcd43b_settings_page build/test_lcd43b_settings build/test_lcd43b_clock build/test_lcd43b_battery build/test_budget build/test_extras build/test_longpress build/test_solar build/test_battcal_wake build/test_battcal build/test_secure_transport build/test_security build/test_statelost build/test_spares build/test_owner build/test_ponretry_stall build/test_ponretry build/test_boottime build/test_bootconfirm build/test_pwren build/test_stall build/test_logic build/test_cache build/test_cache_abort build/test_net build/test_overlay build/test_wake build/test_lcd43b_gesture build/test_lcd43b_frame build/test_lcd43b_manifest build/test_lcd43b_overlay build/test_lcd43b_panel build/test_lcd43b_diag build/test_lcd43b_screens build/test_lcd43b_touch build/test_lcd43b_sync build/test_lcd43b_persist
	./build/test_lcd43b_power_session
	./build/test_lcd43b_power
	./build/test_lcd43b_settings_page
	./build/test_lcd43b_settings
	./build/test_lcd43b_clock
	./build/test_lcd43b_battery
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
	./build/test_lcd43b_persist

build/test_logic: test_logic.cpp ../ventanal/logic.cpp ../ventanal/logic.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_logic.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

clean:
	rm -rf build

build/test_cache: test_cache.cpp ../ventanal/cache.cpp ../ventanal/cache.h ../ventanal/config.example.h ../ventanal/http.h ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Wframe-larger-than=2048 -Ifakes test_cache.cpp ../ventanal/cache.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_cache_abort: test_cache.cpp ../ventanal/cache.cpp ../ventanal/cache.h ../ventanal/config.example.h ../ventanal/http.h ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -DVENTANAL_ABORT_AFTER_BYTES=100000 -Wframe-larger-than=2048 -Ifakes test_cache.cpp ../ventanal/cache.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_net: test_net.cpp ../ventanal/quiet.h ../ventanal/net.cpp ../ventanal/net.h ../ventanal/config.example.h ../ventanal/http.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes -I"$(ARDUINOJSON)" test_net.cpp ../ventanal/net.cpp $(LDFLAGS) -o $@

build/test_overlay: ../ventanal/shade.h ../ventanal/shade_tables.h test_overlay.cpp ../ventanal/overlay.cpp ../ventanal/overlay.h $(wildcard ../ventanal/fonts/*.h) $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes test_overlay.cpp ../ventanal/overlay.cpp $(LDFLAGS) -o $@

build/test_wake: test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.ino ../ventanal/logic.cpp ../ventanal/logic.h ../ventanal/net.h ../ventanal/cache.h ../ventanal/overlay.h ../ventanal/config.example.h $(wildcard fakes/*.h) fakes/driver/rtc_io.h fakes/driver/gpio.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_wake.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_stall: test_stall.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.ino ../ventanal/logic.cpp $(wildcard fakes/*.h) fakes/driver/gpio.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_stall.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_pwren: test_stall.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.ino ../ventanal/logic.cpp $(wildcard fakes/*.h) fakes/driver/gpio.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -DVENTANAL_PWRENTEST=1 -Ifakes test_stall.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_boottime build/test_bootconfirm: test_boottime.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.ino ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST $(if $(filter build/test_bootconfirm,$@),-DVENTANAL_BOOT_CONFIRM_MS=7000,) -Ifakes test_boottime.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_ponretry: test_ponretry.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.ino ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_ponretry.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_ponretry_stall: test_ponretry_stall.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.ino ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_ponretry_stall.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_owner: test_owner.cpp ../ventanal/owner.h ../ventanal/net.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes test_owner.cpp $(LDFLAGS) -o $@

build/test_spares: test_spares.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.ino ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_spares.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_statelost: test_statelost.cpp test_wake.cpp ../ventanal/owner.h ../ventanal/quiet.h ../ventanal/ventanal.ino ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_statelost.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_security: test_security.cpp ../ventanal/security.h ../ventanal/manifest_security.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes -I"$(ARDUINOJSON)" test_security.cpp $(LDFLAGS) -o $@

# Shared transport headers also invalidate the net/cache builds.
build/test_net build/test_cache build/test_cache_abort: ../ventanal/security.h ../ventanal/security_config.h ../ventanal/manifest_security.h

build/test_secure_transport: fixtures/extras-server-manifest.json test_secure_transport.cpp secure_config.h ../ventanal/security.h ../ventanal/manifest_security.h ../ventanal/net.cpp ../ventanal/cache.cpp ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -DVENTANAL_SECURITY_CONFIG=../test/secure_config.h -Ifakes -I"$(ARDUINOJSON)" test_secure_transport.cpp ../ventanal/net.cpp ../ventanal/cache.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@


build/test_battcal: test_battcal.cpp ../ventanal/battery.h ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes test_battcal.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

# Calibration header participates in wake and transport builds.
build/test_solar build/test_net build/test_secure_transport build/test_wake build/test_statelost build/test_spares build/test_stall build/test_pwren build/test_boottime build/test_bootconfirm build/test_ponretry build/test_ponretry_stall: ../ventanal/battery.h

build/test_battcal_wake: test_battcal_wake.cpp test_wake.cpp ../ventanal/ventanal.ino ../ventanal/battery.h ../ventanal/logic.cpp ../ventanal/owner.h ../ventanal/net.h ../ventanal/logic.h ../ventanal/cache.h ../ventanal/quiet.h ../ventanal/overlay.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_battcal_wake.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_solar: ../ventanal/battery.h ../ventanal/owner.h ../ventanal/cache.h ../ventanal/quiet.h ../ventanal/overlay.h test_solar.cpp test_wake.cpp ../ventanal/ventanal.ino ../ventanal/logic.cpp ../ventanal/logic.h ../ventanal/net.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_solar.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_longpress: test_longpress.cpp test_wake.cpp ../ventanal/ventanal.ino ../ventanal/owner.h ../ventanal/logic.h ../ventanal/logic.cpp ../ventanal/battery.h ../ventanal/quiet.h ../ventanal/cache.h ../ventanal/overlay.h ../ventanal/net.h ../ventanal/security_config.h ../ventanal/config.example.h $(wildcard fakes/*.h) fakes/driver/gpio.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_longpress.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_extras: test_extras.cpp test_solar.cpp test_wake.cpp ../ventanal/ventanal.ino ../ventanal/net.h ../ventanal/logic.h ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_extras.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_budget: test_budget.cpp test_wake.cpp ../ventanal/ventanal.ino ../ventanal/owner.h ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DVENTANAL_HOST_TEST -Ifakes test_budget.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

# lcd43b device profile: host-side units. gesture/frame/manifest/overlay are
# pure C++; panel compiles the production header against the esp_lcd fakes.
build/test_lcd43b_gesture: test_lcd43b_gesture.cpp ../lcd43b/gesture.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_gesture.cpp $(LDFLAGS) -o $@

build/test_lcd43b_panel: test_lcd43b_panel.cpp ../lcd43b/panel.h fakes/Arduino.h fakes/esp_lcd_panel_ops.h fakes/esp_lcd_panel_rgb.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes -Wframe-larger-than=2048 test_lcd43b_panel.cpp $(LDFLAGS) -o $@

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

build/test_lcd43b_overlay: $(wildcard ../lcd43b/*.h) test_lcd43b_overlay.cpp ../lcd43b/overlay.h ../lcd43b/gfxfont.h ../ventanal/fonts/Antonio-Regular.h ../ventanal/fonts/Inter-Medium.h Makefile
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
build/test_lcd43b_sync: test_lcd43b_sync.cpp ../lcd43b/sync.cpp ../lcd43b/sync.h ../lcd43b/log.h ../lcd43b/persist.h ../lcd43b/manifest.h ../lcd43b/frame.h ../lcd43b/sha256.h ../lcd43b/screens.h $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -fsanitize=address -DVENTANAL_HOST_TEST -Ifakes -I"$(ARDUINOJSON)" test_lcd43b_sync.cpp ../lcd43b/sync.cpp $(LDFLAGS) -fsanitize=address -o $@

# Persisted state (quiet hours, publish shape, cache ids, selection).
build/test_lcd43b_persist: test_lcd43b_persist.cpp ../lcd43b/persist.h ../lcd43b/manifest.h ../lcd43b/screens.h ../lcd43b/frame.h ../lcd43b/sha256.h fakes/Preferences.h fakes/LittleFS.h fakes/Arduino.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes -I"$(ARDUINOJSON)" test_lcd43b_persist.cpp $(LDFLAGS) -o $@

build/test_lcd43b_clock: test_lcd43b_clock.cpp $(wildcard ../lcd43b/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_clock.cpp $(LDFLAGS) -o $@

build/test_lcd43b_battery: test_lcd43b_battery.cpp $(wildcard ../lcd43b/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_battery.cpp $(LDFLAGS) -o $@

build/test_lcd43b_settings: test_lcd43b_settings.cpp $(wildcard ../lcd43b/*.h) $(wildcard fakes/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -Ifakes -I"$(ARDUINOJSON)" test_lcd43b_settings.cpp $(LDFLAGS) -o $@

build/test_lcd43b_screens build/test_lcd43b_sync build/test_lcd43b_persist build/test_lcd43b_panel: ../lcd43b/settings.h

build/test_lcd43b_settings_page: fixtures/lcd43b-settings-chrome.rle test_lcd43b_settings_page.cpp $(wildcard ../lcd43b/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_settings_page.cpp $(LDFLAGS) -o $@

build/test_lcd43b_power: test_lcd43b_power.cpp $(wildcard ../lcd43b/*.h) Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_power.cpp $(LDFLAGS) -o $@

build/test_lcd43b_power_session: test_lcd43b_power_session.cpp ../lcd43b/power_session.h ../lcd43b/power.h Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_lcd43b_power_session.cpp $(LDFLAGS) -o $@
