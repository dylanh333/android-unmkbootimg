.DEFAULT_GOAL=all
CC=gcc -std=c99 -I include -I-
SRC=$(wildcard src/*.c)
OBJ=$(SRC:src/%.c=build/%.o)
####


# Specify all target binaries here
BIN=bin/unmkbootimg

#Specify additional dependencies here


####
all: $(BIN)

$(BIN): bin/%: build/%.o
	$(CC) -o $@ $<

$(OBJ): build/%.o: src/%.c
	mkdir -p build bin
	$(CC) -c -o $@ $<

.PHONY: clean
clean:
	rm -f bin/* build/*
