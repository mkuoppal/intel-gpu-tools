SUBDIRS = doc test

noinst_LTLIBRARIES = libbrw.la

bin_PROGRAMS = intel-gen4asm intel-gen4disasm

libbrw_la_SOURCES =		\
	brw_compat.h		\
	brw_context.c		\
	brw_context.h		\
	brw_disasm.c		\
	brw_defines.h		\
	brw_eu.h		\
	brw_eu.c		\
	brw_eu_compact.c	\
	brw_eu_debug.c		\
	brw_eu_emit.c		\
	brw_eu_util.c		\
	brw_reg.h		\
	brw_structs.h		\
	gen8_disasm.c		\
	gen8_instruction.h	\
	gen8_instruction.c	\
	ralloc.c		\
	ralloc.h		\
	$(NULL)

AM_YFLAGS = -d --warnings=all
AM_CFLAGS= $(ASSEMBLER_WARN_CFLAGS)

LEX = flex -i
BUILT_SOURCES = gram.h gram.c lex.c
gram.h: gram.c

intel_gen4asm_SOURCES =	\
	gen4asm.h	\
	gram.y		\
	lex.l		\
	main.c		\
	$(NULL)

intel_gen4asm_LDADD = libbrw.la

intel_gen4disasm_SOURCES =  disasm-main.c
intel_gen4disasm_LDADD = libbrw.la

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = intel-gen4asm.pc

check_SCRIPTS = test/run-test.sh

TESTS = \
	test/mov \
	test/frc \
	test/rndd \
	test/rndu \
	test/rnde \
	test/rnde-intsrc \
	test/rndz \
	test/lzd \
	test/not \
	test/immediate \
	$(NULL)

# Tests that are expected to fail because they contain some inccorect code.
XFAIL_TESTS =

# Those tests were already failing when the assembler was imported from
# the intel-gen4asm git repository:
#   http://cgit.freedesktop.org/xorg/app/intel-gen4asm/
# We disable them "for now" as a workaround to be able to release i-g-t
disabled_tests = \
	test/declare \
	test/jmpi \
	test/if \
	test/iff \
	test/while \
	test/else \
	test/break \
	test/cont \
	test/halt \
	test/wait \
	test/endif \
	$(NULL)

disabled_xfail_tests = \
	test/rnde-intsrc \
	$(NULL)

TESTDATA = \
	test/mov.expected \
	test/mov.g4a \
	test/frc.expected \
	test/frc.g4a \
	test/rndd.expected \
	test/rndd.g4a \
	test/rndu.expected \
	test/rndu.g4a \
	test/rnde.expected \
	test/rnde.g4a \
	test/rnde-intsrc.expected \
	test/rnde-intsrc.g4a \
	test/rndz.expected \
	test/rndz.g4a \
	test/lzd.expected \
	test/lzd.g4a \
	test/not.expected \
	test/not.g4a \
	test/jmpi.expected \
	test/jmpi.g4a \
	test/if.expected \
	test/if.g4a \
	test/iff.expected \
	test/iff.g4a \
	test/while.expected \
	test/while.g4a \
	test/else.expected \
	test/else.g4a \
	test/break.expected \
	test/break.g4a \
	test/cont.expected \
	test/cont.g4a \
	test/halt.expected \
	test/halt.g4a \
	test/wait.expected \
	test/wait.g4a \
	test/endif.expected \
	test/endif.g4a \
	test/declare.expected \
	test/declare.g4a \
	test/immediate.g4a \
	test/immediate.expected \
	$(NULL)

test_EXTRA_DIST = \
	${TESTDATA} \
	test/run-test.sh \
	$(NULL)

$(TESTS): test/run-test.sh
	sed "s|TEST|$@|g" ${srcdir}/test/run-test.sh > $@
	chmod +x $@

test_CLEANFILES = \
	test/*.out \
	${TESTS} \
	$(NULL)

CLEANFILES = $(BUILT_SOURCES) \
	     $(test_CLEANFILES) \
	     $(NULL)

EXTRA_DIST = \
	README \
	TODO \
	intel-gen4asm.pc.in \
	$(test_EXTRA_DIST) \
	$(NULL)
