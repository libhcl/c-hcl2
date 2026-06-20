PREFIX ?= /usr/local
CFLAGS ?= -O2 -Wall -Wextra -pedantic -I.
LDLIBS ?= -lm

SANITIZE ?=
LLVM_PROFDATA ?= llvm-profdata
LLVM_COV ?= llvm-cov
TEST_CFLAGS := -I. -O0 -g -Wall -Wextra -pedantic -DHCL2_FAULT_INJECT
ifneq ($(strip $(SANITIZE)),)
TEST_CFLAGS += -fsanitize=$(SANITIZE)
endif
COVER_CFLAGS = $(TEST_CFLAGS) -fprofile-instr-generate -fcoverage-mapping

SRCS := value.c lexer.c parser.c eval.c body.c convert.c json.c json_body.c $(wildcard builtin/*.c)
OBJS := $(SRCS:.c=.o)

all: libhcl2.a

%.o: %.c hcl2.h hcl2_internal.h builtin/builtin.h
	$(CC) $(CFLAGS) -c $< -o $@

libhcl2.a: $(OBJS)
	$(AR) rcs $@ $^

.PHONY: fmt
fmt:
	clang-format -i $(SRCS) hcl2.h hcl2_internal.h builtin/builtin.h test/*.c

.PHONY: test
test:
	$(CC) $(TEST_CFLAGS) $(SRCS) test/hcl2_test.c $(LDLIBS) -o test/hcl2_test
	./test/hcl2_test

.PHONY: cover
cover:
	rm -f *.profraw *.profdata
	$(CC) $(COVER_CFLAGS) $(SRCS) test/hcl2_test.c $(LDLIBS) -o test/hcl2_test
	LLVM_PROFILE_FILE=hcl2.profraw ./test/hcl2_test >/dev/null
	$(LLVM_PROFDATA) merge -sparse hcl2.profraw -o hcl2.profdata
	$(LLVM_COV) report ./test/hcl2_test -instr-profile=hcl2.profdata $(SRCS)

# Deterministic fuzzer (fixed seed -> reproducible). Best run under ASan:
#   make fuzz SANITIZE=address          # ITERS iterations (default 200000)
#   make fuzz ITERS=5000000 SANITIZE=address
ITERS ?= 200000
.PHONY: fuzz
fuzz:
	$(CC) $(TEST_CFLAGS) $(SRCS) test/fuzz.c $(LDLIBS) -o test/fuzz
	./test/fuzz $(ITERS)

install: libhcl2.a
	install -d "$(DESTDIR)$(PREFIX)/lib" "$(DESTDIR)$(PREFIX)/include"
	install -m644 libhcl2.a "$(DESTDIR)$(PREFIX)/lib/"
	install -m644 hcl2.h "$(DESTDIR)$(PREFIX)/include/"

.PHONY: clean
clean:
	rm -f *.o builtin/*.o *.a test/hcl2_test test/fuzz *.profraw *.profdata
	rm -rf *.dSYM test/*.dSYM
