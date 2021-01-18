CFLAGS = -fPIC -Wall -lzip `pkg-config libnautilus-extension --cflags libxml-2.0 libzip`
AM_LDFLAGS = -lzip `pkg-config libnautilus-extension --libs libxml-2.0 libzip`

all: fb2-extension.o
	gcc -shared fb2-extension.o -o fb2-extension.so $(AM_LDFLAGS)

fb2-extension.o:
	gcc -c fb2-extension.c -o fb2-extension.o $(CFLAGS)

install:
	cp fb2-extension.so /usr/lib/nautilus/extensions-3.0
	
uninstall:
	rm -f /usr/lib/nautilus/extensions-3.0/fb2-extension.so
	
replace:
	rm -f /usr/lib/nautilus/extensions-3.0/fb2-extension.so
	cp fb2-extension.so /usr/lib/nautilus/extensions-3.0

clean:
	rm -f *.so
	rm -f *.o

debug:
	nautilus -q && nautilus --browser
