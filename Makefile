CFLAGS = -std=c99 -g -O3 -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -lm
PREFIX = /usr/local

JACKPIFM_SRC=\
        src/outputter.o \
	src/preemp.o \
	src/rds.o \
	src/stereo.o \
	src/main.o

all: jackpifm


# Compilation
%.o: %.c
	$(CC) $(CFLAGS) $(pkg-config --cflags jack) -c -o $@ $<

# Linking
jackpifm: $(JACKPIFM_SRC)
	$(CC) $^ $(LDFLAGS) $(pkg-config --libs jack) -o $@

# Housekeeping
clean:
	$(RM) src/*.o
	$(RM) jackpifm
install:
	install -m755 -d $(DESTDIR)$(PREFIX)/bin
	install -m755 jackpifm $(DESTDIR)$(PREFIX)/bin

