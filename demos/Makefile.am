include Makefile.sources

bin_PROGRAMS = $(demos_prog_list)

if HAVE_LIBDRM_INTEL
	bin_PROGRAMS += $(LIBDRM_INTEL_BIN)
endif

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/lib
AM_CFLAGS = $(DRM_CFLAGS) $(PCIACCESS_CFLAGS) $(CWARNFLAGS) \
	    $(CAIRO_CFLAGS) $(LIBUNWIND_CFLAGS) $(WERROR_CFLAGS)
LDADD = $(top_builddir)/lib/libintel_tools.la
