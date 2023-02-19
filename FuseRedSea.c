#define FUSE_USE_VERSION 34
#define _FILE_OFFSET_BITS 64
#define BLOCK_SIZE 512 // RedSea Block Size
#include <fuse.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

/* RedSea FUSE driver
 * for the TempleOS RedSea filesystem
 *
 */

struct redsea_file {
	unsigned char name[46];
	int size;
	int block;
};
struct redsea_directory {
	unsigned char name[46];
	int block;
	int size;
	int entries;
	unsigned char* children[];

};

static int redsea_file_attributes(const char *path, struct stat *st) {
	if (!strcmp(path, "/")) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
		st -> st_size = 512;
	}
	else {
		st-> st_mode = S_IFREG | 0644;
		st-> st_nlink = 2;
		st-> st_size = 512;
	}
	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_size = 512;
	return 0;
}

void read_directory(int block, FILE* image) {
	unsigned char* buf;
	fseek(image, block, SEEK_SET);
	//fread(buf, 1, 1, image);
	fseek(image, 0x47, SEEK_CUR);
	fread(buf, 1, 1, image);
	printf("\n%x\n", *buf);
	
}

static int redsea_read_directory(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	filler(buffer, ".", NULL, 0);
	filler(buffer, "..", NULL, 0);
	filler(buffer, "test.txt", NULL, 0);
	return 0;
}

static struct fuse_operations redsea_ops = {
	.getattr = redsea_file_attributes,
	.readdir = redsea_read_directory
};

char *devfile = NULL;

int main(int argc, char **argv) {
	int i;
	for (i = 1; i<argc && argv[i][0] == '-'; i++);
	if (i < argc) {
		devfile = realpath(argv[i], NULL);
		printf("%s\n", devfile);
		memcpy(&argv[i], &argv[i+1], (argc-i) * sizeof(argv[0]));
		argc--;
	}

	FILE* image = fopen(devfile, "rb");		// open disk image
	fseek(image, 0x8800, SEEK_SET);			// seek to 0x8800
	rewind(image);
	read_directory(0x8800, image);
	read_directory(0x8801, image);
	printf("%x", 0x8800);

	return fuse_main(argc, argv, &redsea_ops, NULL);
	
}
