stats_SOURCES := modules/stats/stats.c contrib/ccan/json/json.c
stats_DEPEND := $(libkresolve)
stats_LIBS := $(libkresolve_TARGET) $(libkresolve_LIBS)
$(call make_c_module,stats)
