# -- Do not modify, will get replaced during testing --

SOURCES = sfs.c diskio.c
HEADERS = sfs.h diskio.h
META = Makefile check.py README.rst .gitignore \
	   mkfs.sfs mkfs.sfs.amd64 mkfs.sfs.arm64 \
	   fsck.sfs fsck.sfs.amd64 fsck.sfs.arm64

CFLAGS = -Og -ggdb -std=gnu99 -Wall -Wextra -fsanitize=address \
		 -fno-omit-frame-pointer -D_FILE_OFFSET_BITS=64
LDFLAGS = -lfuse -fsanitize=address

DOCKERIMG = vusec/vu-os-fs-check

.PHONY: all tarball clean check

all: sfs

tarball: fs.tar.gz

fs.tar.gz: $(SOURCES) $(HEADERS) $(META)
	tar czf $@ $^

check:
	@./check.py

docker-update:
	docker pull $(DOCKERIMG)

docker-check: fs.tar.gz
	mkdir -p docker_mnt
	cp $^ docker_mnt/
	docker run --privileged -i -t --rm -v "`pwd`/docker_mnt:/submission" $(DOCKERIMG) /test

sfs: $(SOURCES:.c=.o)
	$(CC) -o $@ $^ $(LDFLAGS)

$(SOURCES:.c=.o): $(HEADERS)

clean:
	rm -f sfs *.o
