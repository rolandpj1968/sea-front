CC       = cc
CFLAGS   = -std=c11 -Wall -Wextra -Wpedantic -g
BUILDDIR = build

HDR      = src/sea-front.h
PARSE_HDR = src/parse/parse.h

SRCS     = src/main.c src/util.c src/arena.c \
           src/lex/tokenize.c src/lex/unicode.c \
           src/parse/parser.c src/parse/expr.c src/parse/stmt.c \
           src/parse/decl.c src/parse/type.c src/parse/lookup.c \
           src/parse/ast_dump.c \
           src/sema/sema.c \
           src/codegen/emit_c.c \
           src/codegen/mangle.c
OBJS     = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRCS))

# Lexer test (doesn't need parse objects)
LEX_TEST_SRCS = src/util.c src/arena.c src/lex/tokenize.c src/lex/unicode.c
LEX_TEST_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(LEX_TEST_SRCS))
LEX_TEST_TARGET = $(BUILDDIR)/test_lex

# Vendored mcpp preprocessor
MCPP_SRCS = main.c directive.c eval.c expand.c support.c system.c mbchar.c
MCPP_OBJS = $(patsubst %.c,$(BUILDDIR)/mcpp/%.o,$(MCPP_SRCS))
MCPP       = $(BUILDDIR)/mcpp-bin

TARGET   = $(BUILDDIR)/sea-front

all: $(TARGET) $(MCPP)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(MCPP): $(MCPP_OBJS)
	$(CC) -o $@ $^

$(BUILDDIR)/mcpp/%.o: vendor/mcpp/%.c | $(BUILDDIR)/mcpp
	$(CC) -O2 -w -c -o $@ $<

# Lexer tests
$(LEX_TEST_TARGET): $(BUILDDIR)/test_lex.o $(LEX_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILDDIR)/test_lex.o: tests/test_lex.c $(HDR) | $(BUILDDIR)
	$(CC) $(CFLAGS) -I src -c -o $@ $<

# Pattern rules for source compilation
$(BUILDDIR)/%.o: src/%.c $(HDR) $(PARSE_HDR) | $(BUILDDIR)/lex $(BUILDDIR)/parse $(BUILDDIR)/sema $(BUILDDIR)/codegen
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR) $(BUILDDIR)/lex $(BUILDDIR)/parse $(BUILDDIR)/sema $(BUILDDIR)/codegen $(BUILDDIR)/mcpp:
	mkdir -p $@

# Core tests — must all pass (gated).
test: $(LEX_TEST_TARGET) $(TARGET) $(MCPP)
	./$(LEX_TEST_TARGET)
	@if [ -x tests/test.sh ]; then ./tests/test.sh $(TARGET); fi
	@if [ -x tests/test_parse.sh ]; then ./tests/test_parse.sh $(TARGET); fi
	@if [ -x tests/test_emit_c.sh ]; then ./tests/test_emit_c.sh $(TARGET); fi
	@if [ -x tests/test_multi_tu.sh ]; then ./tests/test_multi_tu.sh $(TARGET); fi
	@if [ -x tests/test_libstdcxx_headers.sh ] && [ -d /usr/include/c++/13 ]; then \
	    ./tests/test_libstdcxx_headers.sh $(TARGET) $(MCPP); \
	fi

# Smoke tests against GCC/Clang test suites — not gated (has expected failures).
# Tracks progress toward the ultimate bootstrap goal.
# Non-fatal: reports pass/fail counts but always exits 0.
test-smoke: $(TARGET) $(MCPP)
	@echo "=== Lexer smoke ==="
	-@./tests/test_clang.sh $(TARGET) $(MCPP) || true
	@echo ""
	@echo "=== Parser smoke ==="
	-@./tests/test_clang_parse.sh $(TARGET) $(MCPP) || true

clean:
	rm -rf $(BUILDDIR)

.PHONY: all test test-smoke clean
