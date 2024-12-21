EXECBIN  = httpserver
SOURCES  = $(wildcard *.c)
HEADERS  = $(wildcard *.h)
OBJECTS  = $(SOURCES:%.c=%.o)
LIBRARY  = asgn4_helper_funcs.a

CC       = clang
FORMAT   = clang-format
CFLAGS   = -Wall -Wpedantic -Werror -Wextra -DDEBUG -g

.PHONY: all clean nuke format

all: $(EXECBIN)

$(EXECBIN): $(OBJECTS) $(LIBRARY)
        @if [ ! -f $(LIBRARY) ]; then echo "Missing library: $(LIBRARY)"; exit 1; fi
        $(CC) -o $@ $^

%.o : %.c
        $(CC) $(CFLAGS) -c $<

clean:
        rm -f $(EXECBIN) $(OBJECTS)

nuke: clean
        rm -rf .format

format:
        $(FORMAT) -i $(SOURCES) $(HEADERS)