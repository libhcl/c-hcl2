PREFIX ?= /usr/local
CFLAGS ?= -O2 -Wall -Wextra -pedantic
LDLIBS ?= -lm

SANITIZE ?=
LLVM_PROFDATA ?= llvm-profdata
LLVM_COV ?= llvm-cov
TEST_CFLAGS := -I. -O0 -g -Wall -Wextra -pedantic
ifneq ($(strip $(SANITIZE)),)
TEST_CFLAGS += -fsanitize=$(SANITIZE)
endif
COVER_CFLAGS = $(TEST_CFLAGS) -fprofile-instr-generate -fcoverage-mapping

all: libhcl2.a

hcl2.o: hcl2.c hcl2.h
	$(CC) $(CFLAGS) -c hcl2.c -o $@

libhcl2.a: hcl2.o
	$(AR) rcs $@ $^

.PHONY: fmt
fmt:
	clang-format -i hcl2.c hcl2.h test/*.c

.PHONY: test
test:
	$(CC) $(TEST_CFLAGS) hcl2.c test/hcl2_test.c $(LDLIBS) -o test/hcl2_test
	./test/hcl2_test

.PHONY: cover
cover:
	rm -f *.profraw *.profdata
	$(CC) $(COVER_CFLAGS) hcl2.c test/hcl2_test.c $(LDLIBS) -o test/hcl2_test
	LLVM_PROFILE_FILE=hcl2.profraw ./test/hcl2_test >/dev/null
	$(LLVM_PROFDATA) merge -sparse hcl2.profraw -o hcl2.profdata
	$(LLVM_COV) report ./test/hcl2_test -instr-profile=hcl2.profdata hcl2.c

install: libhcl2.a
	install -d "$(DESTDIR)$(PREFIX)/lib" "$(DESTDIR)$(PREFIX)/include"
	install -m644 libhcl2.a "$(DESTDIR)$(PREFIX)/lib/"
	install -m644 hcl2.h "$(DESTDIR)$(PREFIX)/include/"

.PHONY: clean
clean:
	rm -f *.o *.a test/hcl2_test *.profraw *.profdata
	rm -rf *.dSYM test/*.dSYM
