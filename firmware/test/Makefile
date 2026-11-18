# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
CXX = c++
ARDUINOJSON ?= $(HOME)/Library/Arduino15/ventanal-sketchbook/libraries/ArduinoJson/src
SODIUM_CFLAGS ?= $(shell pkg-config --cflags libsodium 2>/dev/null)
SODIUM_LIBS ?= $(shell pkg-config --libs libsodium 2>/dev/null || echo -lsodium)
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -pedantic

.PHONY: test clean
test: build/test_ota build/test_release build/test_budget build/test_extras build/test_longpress build/test_solar build/test_battcal_wake build/test_battcal build/test_secure_transport build/test_security build/test_statelost build/test_spares build/test_owner build/test_ponretry_stall build/test_ponretry build/test_boottime build/test_bootconfirm build/test_pwren build/test_stall build/test_logic build/test_cache build/test_cache_abort build/test_net build/test_overlay build/test_wake
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
	./build/test_overlay
	./build/test_wake

build/test_ota: test_ota.cpp ../ventanal/ota.h ../ventanal/logic.cpp ../ventanal/logic.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG test_ota.cpp ../ventanal/logic.cpp $(LDFLAGS) -o $@

build/test_release: test_release.cpp ../common/release.h ../common/release_key.h Makefile
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG $(SODIUM_CFLAGS) -I"$(ARDUINOJSON)" test_release.cpp $(LDFLAGS) $(SODIUM_LIBS) -o $@

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

build/test_secure_transport: fixtures/extras-server-manifest.json fixtures/manifest-controls.json test_secure_transport.cpp secure_config.h ../ventanal/security.h ../ventanal/manifest_security.h ../ventanal/net.cpp ../ventanal/cache.cpp ../ventanal/logic.cpp $(wildcard fakes/*.h) Makefile
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

build/test_budget build/test_extras build/test_longpress build/test_solar build/test_battcal_wake build/test_statelost build/test_spares build/test_ponretry_stall build/test_ponretry build/test_boottime build/test_bootconfirm build/test_pwren build/test_stall build/test_wake: fakes/freertos/FreeRTOS.h fakes/freertos/task.h

build/test_cache build/test_cache_abort build/test_net build/test_secure_transport build/test_owner build/test_budget build/test_extras build/test_longpress build/test_solar build/test_battcal_wake build/test_statelost build/test_spares build/test_ponretry_stall build/test_ponretry build/test_boottime build/test_bootconfirm build/test_pwren build/test_stall build/test_wake: ../ventanal/ota.h
