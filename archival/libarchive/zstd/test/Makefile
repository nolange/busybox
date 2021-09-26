all: test_zstd

CFLAGS = -O2 -iquote../../../../include  -iquote. -includeautoconf.h

decompress_unzstd.o: ../../decompress_unzstd.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_zstd: test_zstd.c decompress_unzstd.o
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f test_zstd decompress_unzstd.o

.PHONY: clean
