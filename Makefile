# Makefile for synhttp
CC=gcc
CFLAGS=-L/usr/local/libevent-2.0.10-stable/lib/ -levent -L/usr/local/tokyocabinet-1.4.47/lib/ -ltokyocabinet -L/usr/local/curl/lib/ -lcurl -I/usr/local/libevent-2.0.10-stable/include/ -I/usr/local/tokyocabinet-1.4.47/include/ -I/usr/local/curl/include/ -lcrypto -lssl -lz -lbz2 -lrt -lpthread -lm -lc -O2 -g

synhttp: synhttp.c
	$(CC) -o synhttp synhttp.c $(CFLAGS)
	@echo ""
	@echo "synhttp build complete."
	@echo ""	

clean: synhttp
	rm -f synhttp

install: synhttp
	install $(INSTALL_FLAGS) -m 4755 -o root synhttp $(DESTDIR)/usr/bin
