redseabuild:
	gcc -I/usr/include/fuse FuseRedSea.c -lfuse -o redsea
debug:
	gcc -Wall -g -O0 -I/usr/include/fuse FuseRedSea.c -lfuse -o redsea

all: redseabuild
