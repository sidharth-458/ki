#!/bin/sh
SHELL=/bin/sh
#======= USER SETTINGS ========================================================
PROJ=ki
COMPILER=gcc
CC=@${COMPILER}
USERFLAGS= -fno-unroll-loops -fno-exceptions -Oz -Os -flto
#======= DEFAULTS =============================================================
ALL= ${PROJ}
CFLAGS=-fno-delete-null-pointer-checks -fno-strict-overflow\
	-Wformat -Wformat=2 -Wconversion -Wimplicit-fallthrough\
	-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS\
	-fstack-protector-strong -fPIE -Wall -pedantic\
	-pedantic-errors -Wextra -Waggregate-return -Wcast-align\
	-Wcast-qual -Wchar-subscripts -Wcomment -Wconversion\
	-Wfloat-equal -Wformat -Wformat=2  -Wformat-security\
	-Wformat-y2k -Wimplicit -Wimport -Winit-self -Winline -Winvalid-pch\
	-Wlong-long -Wmissing-braces -Wmissing-field-initializers\
	-Wmissing-format-attribute -Wmissing-include-dirs -Wmissing-noreturn\
	-Wpacked -Wparentheses -Wpointer-arith\
	-Wredundant-decls -Wreturn-type -Wsequence-point -Wsign-compare\
	-Wstack-protector -Wstrict-aliasing -Wstrict-aliasing=2 -Wswitch\
	-Wswitch-default -Wswitch-enum -Wtrigraphs -Wuninitialized\
	-Wunknown-pragmas -Wunreachable-code -Wunused -Wunused-function\
	-Wunused-label -Wunused-parameter -Wunused-value -Wunused-variable\
	-Wvariadic-macros -Wvolatile-register-var -Wwrite-strings\
	-Wsign-conversion -Wconversion -Wdouble-promotion -Wnull-dereference\
	-fno-strict-aliasing\
	-Wdisabled-optimization -Wshadow ${USERFLAGS}
#======= ARCHITECHTURE DEPENDENT ==============================================
ARCH=$(shell uname -m)
ifeq (${ARCH},arm64)
	CFLAGS+=-mbranch-protection=standard -fstrict-flex-arrays=3
endif
ifeq (${ARCH},x86_64)
	CFLAGS+=-fcf-protection=full
endif
#======= RULES ================================================================
${PROJ}:${PROJ}.o
	@${CC} *.o
	@strip -s a.out
	@mv a.out ${PROJ}

${PROJ}.o: ${PROJ}.c
#======= PROJECT MGMT =========================================================
.PHONY:	clean
clean:
	rm -f *.o ${PROJ}

