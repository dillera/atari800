CC = gcc
CFLAGS = -Wall -g -I.. -DUSE_FUJINET -DDEBUG_FUJINET
LDFLAGS = -lm

# Object files
OBJS = netsio.o ../fujinet_netsio.o

# Target
all: netsio_test

netsio_test: $(OBJS) netsio_test.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

netsio.o: netsio.c netsio.h
	$(CC) $(CFLAGS) -c -o $@ $<

../fujinet_netsio.o: ../fujinet_netsio.c ../fujinet_netsio.h
	$(CC) $(CFLAGS) -c -o $@ $<

netsio_test.o: netsio_test.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o ../fujinet_netsio.o netsio_test

# Create a simple test program
netsio_test.c:
	echo '#include <stdio.h>' > $@
	echo '#include <stdlib.h>' >> $@
	echo '#include <string.h>' >> $@
	echo '#include "netsio.h"' >> $@
	echo '' >> $@
	echo 'int main(int argc, char *argv[]) {' >> $@
	echo '    printf("NetSIO Test Program\\n");' >> $@
	echo '    printf("Initializing NetSIO...\\n");' >> $@
	echo '    if (NetSIO_Initialize("localhost", 9997) != NETSIO_STATUS_OK) {' >> $@
	echo '        printf("Failed to initialize NetSIO\\n");' >> $@
	echo '        return 1;' >> $@
	echo '    }' >> $@
	echo '    printf("NetSIO initialized successfully\\n");' >> $@
	echo '    printf("Sending device connected message...\\n");' >> $@
	echo '    NetSIO_SendDeviceConnected();' >> $@
	echo '    printf("Waiting for incoming messages (press Ctrl+C to exit)...\\n");' >> $@
	echo '    while (1) {' >> $@
	echo '        NetSIO_HandleIncomingMessages();' >> $@
	echo '    }' >> $@
	echo '    NetSIO_Shutdown();' >> $@
	echo '    return 0;' >> $@
	echo '}' >> $@

.PHONY: all clean
