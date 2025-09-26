CC=gcc
CFLAGS=-fPIC -Wall -O2 -shared -w
SRC=main.c nvstusb.c usb_libusb.c emitter.c
LIBS64=-lX11 -lXxf86vm -lXrandr -ldl -lGL -lGLU -lusb-1.0 -lpthread
LIBS32=-lX11 -lXxf86vm -ldl -lGL -lusb-1.0 -lpthread

all: libstereo32.so libstereo64.so

libstereo32.so: $(SRC)
	$(CC) $(CFLAGS) -m32 -o $@ $(SRC) $(LIBS32) && \
		echo "[INFO] Built $@ successfully." || \
		echo "[ERROR] Failed to build $@."

libstereo64.so: $(SRC)
	$(CC) $(CFLAGS) -m64 -o $@ $(SRC) $(LIBS64) && \
		echo "[INFO] Built $@ successfully." || \
		echo "[ERROR] Failed to build $@."

clean:
	rm -f libstereo32.so libstereo64.so 

demo: demo.c
	$(CC) -m64 -O2 -o demo demo.c -lX11 -lGL -lGLU -lm