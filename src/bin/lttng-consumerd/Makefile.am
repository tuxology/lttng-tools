AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src

lttnglibexec_PROGRAMS = lttng-consumerd

lttng_consumerd_SOURCES = lttng-consumerd.c lttng-consumerd.h

lttng_consumerd_LDADD = \
	   $(top_builddir)/src/common/libconsumer.la \
	   $(top_builddir)/src/common/sessiond-comm/libsessiond-comm.la \
	   $(top_builddir)/src/common/libcommon.la \
	   -lrt

if HAVE_LIBLTTNG_UST_CTL
lttng_consumerd_LDADD += -llttng-ust-ctl
endif
