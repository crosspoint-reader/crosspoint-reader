all: fontconvert

CC     = gcc
CFLAGS = -Wall -I/usr/local/include/freetype2 -I/usr/include/freetype2 -I/usr/include -I/opt/homebrew/include/freetype2
LIBS   = -L/opt/homebrew/lib -lfreetype

fontconvert: main.c ../Group5/Group5.h
	$(CC) $(CFLAGS) main.c $(LIBS) -o fontconvert
	strip fontconvert

clean:
	rm -f fontconvert
