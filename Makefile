CFLAGS = -fPIC -std=c99 -g -O3 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags jack samplerate)
LDFLAGS = -pthread -lm -lrt $(shell pkg-config --libs jack samplerate)
PREFIX = /usr/local

JACKPIFM_SRC=\
	src/controller.o \
	src/outputter.o \
	src/preemp.o \
	src/rds.o \
	src/resamp.o \
	src/stereo.o \
	\
	src/main.o

all: jackpifm


# Compilation
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Linking
jackpifm: $(JACKPIFM_SRC)
	$(CC) $^ $(LDFLAGS) -o $@

# Housekeeping
clean:
	$(RM) src/*.o
	$(RM) jackpifm
install:
	install -m755 -d $(DESTDIR)$(PREFIX)/bin
	install -m755 jackpifm $(DESTDIR)$(PREFIX)/bin
