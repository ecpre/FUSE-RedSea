redseabuild:
	gcc -I/usr/include/fuse -lfuse FuseRedSea.c -o redsea

all: redseabuild
