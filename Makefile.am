AM_CFLAGS = -I$(srcdir)/include
ACLOCAL_AMFLAGS = -I config

bin_PROGRAMS = \
	performance/multi-node/rdm_mbw_mr \
	performance/multi-node/rdm_pingpong \
	performance/multi-node/rdm_one_sided \
	performance/multi-node/random_access \
	performance/single-node/lock_tests \
	performance/single-node/builtin_atomic_tests

if HAVE_DRC
bin_PROGRAMS += performance/multi-node/auth_key_test
endif

dist_bin_SCRIPTS = \
	scripts/run_cray_tests.py

noinst_LTLIBRARIES = libcraytests.la
libcraytests_la_SOURCES = \
	common/ct_utils.c \
	common/ct_print_utils.c

################################################################################
# Multi-node sources
################################################################################
performance_multi_node_rdm_mbw_mr_SOURCES = \
	performance/multi-node/rdm_mbw_mr.c
performance_multi_node_rdm_mbw_mr_LDADD = libcraytests.la

performance_multi_node_rdm_pingpong_SOURCES = \
	performance/multi-node/rdm_pingpong.c
performance_multi_node_rdm_pingpong_LDADD = libcraytests.la

performance_multi_node_rdm_one_sided_SOURCES = \
	performance/multi-node/rdm_one_sided.c
performance_multi_node_rdm_one_sided_LDADD = libcraytests.la

performance_multi_node_random_access_SOURCES = \
        performance/multi-node/random_access.c
performance_multi_node_random_access_LDADD = libcraytests.la

if HAVE_DRC
performance_multi_node_auth_key_test_SOURCES = \
        performance/multi-node/auth_key_test.c
performance_multi_node_auth_key_test_LDADD = libcraytests.la
endif

################################################################################
# Single-node sources
################################################################################
performance_single_node_lock_tests_SOURCES = \
	performance/single-node/lock_tests.c
performance_single_node_lock_tests_LDADD = libcraytests.la

performance_single_node_builtin_atomic_tests_SOURCES = \
	performance/single-node/builtin_atomic_tests.c
performance_single_node_builtin_atomic_tests_LDADD = libcraytests.la

EXTRA_DIST = \
	ct_utils.h \
	ct_tbarrier.h \
	ct_print_utils.h

if HAVE_PMI
AM_CFLAGS += -lpmi

if HAVE_CRAY_PMI
AM_CFLAGS += -DCRAY_PMI_COLL
endif
endif

if HAVE_PROFILE
AM_CFLAGS += -lprofiler

if HAVE_CPUPROFILE
AM_CFLAGS += -DCPUPROFILE_ENABLED
endif
endif
# test:
#	./scripts/run_cray_tests.py
