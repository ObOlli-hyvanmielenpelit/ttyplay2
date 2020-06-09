CC = gcc
CFLAGS = -g -O0 -DSVR4
VERSION = 1.0.8
#For curses (ttyplay only): 
CFLAGS += -DUSE_CURSES
LDFLAGS = -lm
LIBS = -lcurses

TARGET = ttytime2 ttyplay2

DIST =	ttyrec.h io.c io.h ttytime2.c\
	README Makefile ttytime2.1

all: $(TARGET)

ttyplay2: ttyplay2.o io.o
	$(CC) $(CFLAGS) -o ttyplay2 ttyplay2.o io.o $(LIBS)

ttytime2: ttytime2.o io.o
	$(CC) $(CFLAGS) -o ttytime2 ttytime2.o $(LDFLAGS) io.o 

clean:
	rm -f *.o $(TARGET) ttyplay2 ttytime2 *~

dist:
	rm -rf ttyrec-$(VERSION)
	rm -f ttyrec-$(VERSION).tar.gz

	mkdir ttyrec-$(VERSION)
	cp $(DIST) ttyrec-$(VERSION)
	tar zcf ttyrec-$(VERSION).tar.gz  ttyrec-$(VERSION)
	rm -rf ttyrec-$(VERSION)
