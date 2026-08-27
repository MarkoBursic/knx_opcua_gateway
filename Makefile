CC = gcc
CFLAGS = -Wall -O2 -std=gnu99 -D_DEFAULT_SOURCE -D_GNU_SOURCE
LIBS = -lopen62541 -ljson-c -lpthread

TARGET = knx_opcua_gateway
OBJS = main.o config_loader.o opc_handler.o knx_handler.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)


