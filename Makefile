.POSIX:

CONFIGFILE = config.mk
include $(CONFIGFILE)

OBJ =\
	sshexec.o

HDR =

all: sshexec
$(OBJ): $(HDR)

.c.o:
	$(CC) -c -o $@ $< $(CFLAGS) $(CPPFLAGS)

sshexec: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

install: sshexec
	mkdir -p -- "$(DESTDIR)$(PREFIX)/bin"
	mkdir -p -- "$(DESTDIR)$(MANPREFIX)/man1/"
	cp -- sshexec "$(DESTDIR)$(PREFIX)/bin/"
	test ! -d "$(DESTDIR)$(PREFIX)/bin/sshcd"
	ln -sf -- sshexec "$(DESTDIR)$(PREFIX)/bin/sshcd"
	cp -- sshexec.1 "$(DESTDIR)$(MANPREFIX)/man1/"
	cp -- sshcd.1 "$(DESTDIR)$(MANPREFIX)/man1/"

uninstall:
	-rm -f -- "$(DESTDIR)$(PREFIX)/bin/sshexec"
	-rm -f -- "$(DESTDIR)$(PREFIX)/bin/sshcd"
	-rm -f -- "$(DESTDIR)$(MANPREFIX)/man1/sshexec.1"
	-rm -f -- "$(DESTDIR)$(MANPREFIX)/man1/sshcd.1"

clean:
	-rm -f -- *.o *.a *.lo *.su *.so *.so.* *.gch *.gcov *.gcno *.gcda
	-rm -f -- sshexec

.SUFFIXES:
.SUFFIXES: .o .c

.PHONY: all install uninstall clean
