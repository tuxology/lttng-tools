AM_CFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src -I$(top_srcdir)/tests/utils/ -I$(srcdir)
AM_LDFLAGS =

if LTTNG_TOOLS_BUILD_WITH_LIBDL
AM_LDFLAGS += -ldl
endif
if LTTNG_TOOLS_BUILD_WITH_LIBC_DL
AM_LDFLAGS += -lc
endif

LIBTAP=$(top_builddir)/tests/utils/tap/libtap.la

LIBCOMMON=$(top_builddir)/src/common/libcommon.la
LIBSESSIOND_COMM=$(top_builddir)/src/common/sessiond-comm/libsessiond-comm.la
LIBHASHTABLE=$(top_builddir)/src/common/hashtable/libhashtable.la

# Define test programs
noinst_PROGRAMS = test_uri test_session	test_kernel_data test_utils_parse_size_suffix

if HAVE_LIBLTTNG_UST_CTL
noinst_PROGRAMS += test_ust_data
endif

# URI unit tests
test_uri_SOURCES = test_uri.c
test_uri_LDADD = $(LIBTAP) $(LIBCOMMON) $(LIBHASHTABLE)

# Session unit test
SESSIONS=$(top_srcdir)/src/bin/lttng-sessiond/session.o	\
	 $(top_srcdir)/src/bin/lttng-sessiond/consumer.o \
	 $(top_srcdir)/src/bin/lttng-sessiond/health.o \
	 $(top_srcdir)/src/common/uri.o \
	 $(top_srcdir)/src/common/utils.o \
	 $(top_srcdir)/src/common/error.o

test_session_SOURCES = test_session.c
test_session_LDADD = $(LIBTAP) $(LIBCOMMON) $(LIBSESSIOND_COMM) $(LIBHASHTABLE) \
					 -lrt
test_session_LDADD += $(SESSIONS)

# UST data structures unit test
if HAVE_LIBLTTNG_UST_CTL
UST_DATA_TRACE=$(top_srcdir)/src/bin/lttng-sessiond/trace-ust.o \
	       $(top_srcdir)/src/bin/lttng-sessiond/consumer.o \
		   $(top_srcdir)/src/bin/lttng-sessiond/buffer-registry.o \
		   $(top_srcdir)/src/bin/lttng-sessiond/ust-registry.o \
		   $(top_srcdir)/src/bin/lttng-sessiond/ust-metadata.o \
		   $(top_srcdir)/src/bin/lttng-sessiond/ust-app.o \
		   $(top_srcdir)/src/bin/lttng-sessiond/ust-consumer.o \
		   $(top_srcdir)/src/bin/lttng-sessiond/fd-limit.o \
		   $(top_srcdir)/src/bin/lttng-sessiond/health.o \
		   $(top_srcdir)/src/bin/lttng-sessiond/session.o \
		   $(top_srcdir)/src/common/uri.o \
		   $(top_srcdir)/src/common/utils.o

test_ust_data_SOURCES = test_ust_data.c
test_ust_data_LDADD = $(LIBTAP) $(LIBCOMMON) $(LIBSESSIOND_COMM) $(LIBHASHTABLE) \
					  -lrt -llttng-ust-ctl
test_ust_data_LDADD += $(UST_DATA_TRACE)
endif

# Kernel data structures unit test
KERN_DATA_TRACE=$(top_srcdir)/src/bin/lttng-sessiond/trace-kernel.o	\
		$(top_srcdir)/src/bin/lttng-sessiond/consumer.o	\
		$(top_srcdir)/src/bin/lttng-sessiond/health.o \
		$(top_srcdir)/src/common/uri.o \
		$(top_srcdir)/src/common/utils.o

test_kernel_data_SOURCES = test_kernel_data.c
test_kernel_data_LDADD = $(LIBTAP) $(LIBCOMMON) $(LIBSESSIOND_COMM) $(LIBHASHTABLE) \
						 -lrt
test_kernel_data_LDADD += $(KERN_DATA_TRACE)

# parse_size_suffix unit test
UTILS_PARSE_SIZE_SUFFIX=$(top_srcdir)/src/common/utils.o \
		$(top_srcdir)/src/common/runas.o

test_utils_parse_size_suffix_SOURCES = test_utils_parse_size_suffix.c
test_utils_parse_size_suffix_LDADD = $(LIBTAP) $(LIBHASHTABLE) $(LIBCOMMON)
test_utils_parse_size_suffix_LDADD += $(UTILS_PARSE_SIZE_SUFFIX)
