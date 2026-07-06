CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
TARGET  = doip_server
SIMULATOR = dtc_simulator
TEST_A  = test_appA
TEST_B  = test_appB
OBJS    = doip_main.o doip_server.o uds.o ota_manager.o dtc_store.o aes_cmac.o

all: $(TARGET) $(SIMULATOR) $(TEST_A) $(TEST_B)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

doip_main.o: doip_main.c doip_server.h doip.h
	$(CC) $(CFLAGS) -c doip_main.c

doip_server.o: doip_server.c doip_server.h doip.h uds.h
	$(CC) $(CFLAGS) -c doip_server.c

uds.o: uds.c uds.h ota_manager.h aes_cmac.h
	$(CC) $(CFLAGS) -c uds.c

ota_manager.o: ota_manager.c ota_manager.h
	$(CC) $(CFLAGS) -c ota_manager.c

dtc_store.o: dtc_store.c dtc_store.h
	$(CC) $(CFLAGS) -c dtc_store.c

aes_cmac.o: aes_cmac.c aes_cmac.h
	$(CC) $(CFLAGS) -c aes_cmac.c

$(SIMULATOR): dtc_simulator.c dtc_store.o dtc_store.h
	$(CC) $(CFLAGS) -o $(SIMULATOR) dtc_simulator.c dtc_store.o

$(TEST_A): test_appA.c
	$(CC) $(CFLAGS) -o $(TEST_A) test_appA.c

$(TEST_B): test_appB.c
	$(CC) $(CFLAGS) -o $(TEST_B) test_appB.c

clean:
	rm -f $(TARGET) $(SIMULATOR) $(TEST_A) $(TEST_B) $(OBJS)

# 首次部署：将 doip_server 安装到 slot_a
install: $(TARGET)
	mkdir -p data/ota/slot_a
	cp $(TARGET) data/ota/slot_a/firmware.bin
	chmod +x data/ota/slot_a/firmware.bin
	echo "已安装到 data/ota/slot_a/"

run: install
	./$(TARGET)

.PHONY: all clean run