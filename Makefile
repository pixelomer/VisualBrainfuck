CC = clang
CFLAGS = 
LDFLAGS = -lncurses
ifeq ($(shell uname),Darwin)
LDFLAGS += -lSystem -macosx_version_min 10.8 -arch x86_64
CFLAGS += -mmacosx-version-min=10.8 -arch x86_64
else
CFLAGS += -arch i386
LDFLAGS += -arch i386
endif

all: brainfuck

clean:
	rm -rf out

brainfuck: out/main.o
	ld $(LDFLAGS) -o brainfuck out/main.o

out/main.o: main.c out
	clang $(CFLAGS) -c -o out/main.o main.c

out:
	mkdir out