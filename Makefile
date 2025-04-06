CC ?= /bin/gcc
LIBDIR ?= /usr/local/lib64
SONAME ?= qalloc.so

build:
	$(CC) -pg -g -shared -o $(SONAME) -fPIC qalloc.c

debug:
	$(CC) -g -DDEBUG -shared -o $(SONAME) -fPIC qalloc.c

install:
	install -m 755 $(SONAME) $(LIBDIR)/
