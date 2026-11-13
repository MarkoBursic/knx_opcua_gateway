CC = gcc
CFLAGS = -Wall -O2 -std=gnu99 -D_DEFAULT_SOURCE -D_GNU_SOURCE -Ivendor/eibclient/include
LIBS = -lopen62541 -ljson-c -lpthread -lm -lcrypt vendor/eibclient/lib/libeibclient.a

TARGET = knx_opcua_gateway
OBJS = main.o config_loader.o opc_handler.o opc_security.o knx_handler.o knx_eibd.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
