redseabuild:
	gcc -I/usr/include/fuse -lfuse FuseRedSea.c -o redsea
debug:
	gcc -Wall -g -O0 -I/usr/include/fuse -lfuse FuseRedSea.c -o redsea

all: redseabuild
