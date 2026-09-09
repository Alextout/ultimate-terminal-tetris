# ultimate-terminal-tetris

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= $(HOME)

tetris: tetris.c
	$(CC) $(CFLAGS) -o $@ $<

# everything that can be checked without a person watching
check: tetris
	./tetris --selftest
	tools/screen_check.py ./tetris
	tools/softdrop_check.py ./tetris

# make it a command: needs $(PREFIX)/bin on your PATH
install: tetris
	mkdir -p $(PREFIX)/bin
	ln -sf $(CURDIR)/tetris $(PREFIX)/bin/tetris
	@echo "linked $(PREFIX)/bin/tetris -> $(CURDIR)/tetris"

uninstall:
	rm -f $(PREFIX)/bin/tetris

clean:
	rm -f tetris

.PHONY: check install uninstall clean
