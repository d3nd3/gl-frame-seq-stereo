CC=gcc
CFLAGS=-fPIC -Wall -O2 -shared -w
SRC=main.c nvstusb.c usb_libusb.c emitter.c

all: libstereo32.so libstereo64.so

libstereo32.so: $(SRC)
	$(CC) $(CFLAGS) -m32 -o $@ $(SRC) -lX11 -lXxf86vm -ldl -lGL -lusb-1.0 && \
		echo "[INFO] Built $@ successfully." || \
		echo "[ERROR] Failed to build $@."

libstereo64.so: $(SRC)
	$(CC) $(CFLAGS) -m64 -o $@ $(SRC) -lX11 -lXxf86vm -ldl -lGL -lusb-1.0 && \
		echo "[INFO] Built $@ successfully." || \
		echo "[ERROR] Failed to build $@."

clean:
	rm -f libstereo32.so libstereo64.so 