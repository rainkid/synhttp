# Makefile for synchttp
CC=gcc
CFLAGS=-L/usr/local/libevent-2.0.10-stable/lib/ -levent -L/usr/local/tokyocabinet-1.4.47/lib/ -ltokyocabinet -L/usr/local/curl/lib/ -lcurl -I/usr/local/libevent-2.0.10-stable/include/ -I/usr/local/tokyocabinet-1.4.47/include/ -I/usr/local/curl/include/ -lcrypto -lssl -lz -lbz2 -lrt -lpthread -lm -lc -O2 -g -Bstatic -Bdynamic
synchttp: synchttp.c
	$(CC) -o synchttp synchttp.c lib/tool.c $(CFLAGS)
	@echo ""
	@echo "synchttp build complete."
	@echo ""	

clean: synchttp
	rm -f synchttp

install: synchttp
	install $(INSTALL_FLAGS) -m 4755 -o root synchttp $(DESTDIR)/usr/bin
