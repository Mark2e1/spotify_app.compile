
CFLAGS ?= -Wall -Wextra -O2
LDLIBS = -lcurl -lcjson -lssl -lcrypto -lws2_32
OBJS = main.o auth.o player.o util.o app.res
TARGET = SpotifyController.exe

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c config.h util.h auth.h player.h
	$(CC) $(CFLAGS) -c $< -o $@

app.res: app.rc app.ico
	windres app.rc -O coff -o app.res

.PHONY: clean
clean:
	rm -f $(TARGET) main.o auth.o player.o util.o app.res
