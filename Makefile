CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -g -pthread -Iinclude -MMD -MP
LDFLAGS = -pthread
SRC     = $(wildcard src/*.c)
OBJ     = $(SRC:src/%.c=build/%.o)
TARGET  = mycp

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET)

-include $(OBJ:.o=.d)

.PHONY: all clean
