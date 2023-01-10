.POSIX:

CC ?=		cc
LIBS =		-lsndio
OPTFLAGS =	-O3
DBGFLAGS =	-O0 -g
CFLAGS =	-pipe -Wall -Werror -march=native

all: build

build: clean
	${CC} ${DBGFLAGS} ${CFLAGS} -o cbar ${LIBS} cbar.c

opt: clean
	${CC} ${OPTFLAGS} ${CFLAGS} -o cbar ${LIBS} cbar.c

install:
	install -s cbar /home/sdk/.bin/cbar

clean:
	rm -f cbar

debug: build
	egdb -q ./cbar -ex "break main" -ex "run"
