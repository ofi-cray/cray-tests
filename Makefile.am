AM_CFLAGS = -I$(srcdir)/include
ACLOCAL_AMFLAGS = -I config

bin_PROGRAMS = \
	performance/single-node/gnix_vector_tests

# dist_bin_SCRIPTS = \
# 	scripts/dummy.sh

# performance_single_node_dummy_SOURCES = \
# 	performance/single-node/dummy.c

EXTRA_DIST = \
	ft_utils.h \
	ft_tbarrier.h

if HAVE_PMI
AM_CFLAGS += -lpmi

if HAVE_CRAY_PMI
AM_CFLAGS += -DCRAY_PMI_COLL
endif

endif

test:
	./scripts/dummy.sh
