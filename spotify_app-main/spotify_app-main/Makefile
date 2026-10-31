CFLAGS ?= -Wall -Wextra -Wno-unused-parameter -O2
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)
# -mwindows hides the black console window. Use "make GUIFLAG=" to get it back for debugging.
GUIFLAG ?= -mwindows
LDLIBS = -lcurl -lcjson -lssl -lcrypto -lws2_32 $(GTK_LIBS)
OBJS = main.o auth.o player.o util.o ui.o app.res
TARGET = SpotifyController.exe

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(GUIFLAG) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c config.h util.h auth.h player.h ui.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -c $< -o $@

app.res: app.rc app.ico
	windres app.rc -O coff -o app.res

.PHONY: clean
clean:
	rm -f $(TARGET) main.o auth.o player.o util.o ui.o app.res
