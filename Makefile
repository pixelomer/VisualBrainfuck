CC = clang
CFLAGS = -arch x86_64 -Wno-unused-command-line-argument
LDFLAGS = -lncurses
ifeq ($(shell uname),Darwin)
LDFLAGS += -lSystem -mmacosx-version-min=10.8
CFLAGS += -mmacosx-version-min=10.8
endif

all: brainfuck

clean:
	rm -rf out

brainfuck: out/main.o
	$(CC) $(LDFLAGS) -o brainfuck out/main.o

out/main.o: main.c out
	$(CC) $(CFLAGS) -c -o out/main.o main.c

out:
	mkdir out