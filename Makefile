CFLAGS      = -O0 -ggdb3
CPPFLAGS    = `pkg-config --cflags glib-2.0` -D_GNU_SOURCE
LDLIBS      = `pkg-config --libs glib-2.0`

all: limits runlimit

limits: limits.o proc.o flags.o rlim.o
runlimit: runlimit.o rlim.o flags.o

clean:
	rm -f limits runlimit *.o
