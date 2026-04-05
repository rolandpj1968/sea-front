CC       = cc
CFLAGS   = -std=c11 -Wall -Wextra -Wpedantic -g
BUILDDIR = build

HDR      = src/sea-front.h

SRCS     = src/main.c src/util.c src/lex/tokenize.c src/lex/unicode.c
OBJS     = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRCS))

TEST_SRCS = src/util.c src/lex/tokenize.c src/lex/unicode.c
TEST_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(TEST_SRCS))

TARGET      = $(BUILDDIR)/sea-front
TEST_TARGET = $(BUILDDIR)/test_lex

# Vendored mcpp preprocessor
MCPP_SRCS = main.c directive.c eval.c expand.c support.c system.c mbchar.c
MCPP_OBJS = $(patsubst %.c,$(BUILDDIR)/mcpp/%.o,$(MCPP_SRCS))
MCPP       = $(BUILDDIR)/mcpp-bin

all: $(TARGET) $(MCPP)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(MCPP): $(MCPP_OBJS)
	$(CC) -o $@ $^

$(BUILDDIR)/mcpp/%.o: vendor/mcpp/%.c | $(BUILDDIR)/mcpp
	$(CC) -O2 -w -c -o $@ $<

$(TEST_TARGET): $(BUILDDIR)/test_lex.o $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILDDIR)/test_lex.o: tests/test_lex.c $(HDR) | $(BUILDDIR)
	$(CC) $(CFLAGS) -I src -c -o $@ $<

$(BUILDDIR)/%.o: src/%.c $(HDR) | $(BUILDDIR)/lex
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR) $(BUILDDIR)/lex $(BUILDDIR)/mcpp:
	mkdir -p $@

test: $(TEST_TARGET) $(TARGET) $(MCPP)
	./$(TEST_TARGET)
	@if [ -x tests/test.sh ]; then ./tests/test.sh $(TARGET); fi
	@if [ -x tests/test_clang.sh ]; then ./tests/test_clang.sh $(TARGET) $(MCPP); fi

clean:
	rm -rf $(BUILDDIR)

.PHONY: all test clean
