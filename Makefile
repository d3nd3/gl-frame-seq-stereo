CC=gcc
CFLAGS=-fPIC -Wall -O2 -shared
SRC=main.c

all: libstereo32.so libstereo64.so

libstereo32.so: $(SRC)
	$(CC) $(CFLAGS) -m32 -o $@ $< -ldl -lGL

libstereo64.so: $(SRC)
	$(CC) $(CFLAGS) -m64 -o $@ $< -ldl -lGL

clean:
	rm -f libstereo32.so libstereo64.so 