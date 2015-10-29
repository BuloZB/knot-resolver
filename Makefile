include config.mk
include platform.mk

# Targets
all: info lib modules daemon
install: lib-install modules-install daemon-install
check: all tests
clean: lib-clean modules-clean daemon-clean tests-clean doc-clean
doc: doc-html
.PHONY: all install check clean doc

# Options
ifdef COVERAGE
BUILD_CFLAGS += --coverage
endif

# Dependencies
$(eval $(call find_lib,libknot,2.0))
$(eval $(call find_lib,libuv,1.0))
$(eval $(call find_alt,lua,luajit))
$(eval $(call find_lib,cmocka))
$(eval $(call find_bin,doxygen))
$(eval $(call find_bin,sphinx-build))
$(eval $(call find_bin,python))
$(eval $(call find_lib,libmemcached,1.0))
$(eval $(call find_lib,hiredis))
$(eval $(call find_lib,socket_wrapper))
$(eval $(call find_lib,libdnssec))
$(eval $(call find_gopkg,geoip,github.com/abh/geoip))
# Find Go compiler version
E :=
GO_VERSION := $(subst $(E) $(E),,$(subst go,,$(wordlist 1,2,$(subst ., ,$(word 3,$(shell $(GO) version))))))
$(eval $(call find_ver,go,$(GO_VERSION),15))

# Work around luajit on OS X
ifeq ($(PLATFORM), Darwin)
ifneq (,$(findstring luajit, $(lua_LIBS)))
	lua_LIBS += -pagezero_size 10000 -image_base 100000000
endif
endif

BUILD_CFLAGS += $(libknot_CFLAGS) $(libuv_CFLAGS) $(cmocka_CFLAGS) $(python_CFLAGS) $(lua_CFLAGS) $(libdnssec_CFLAGS)

# Sub-targets
include help.mk
include lib/lib.mk
include daemon/daemon.mk
include modules/modules.mk
include tests/tests.mk
include doc/doc.mk
