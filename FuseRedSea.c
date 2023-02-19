#define FUSE_USE_VERSION 34
#define _FILE_OFFSET_BITS 64
#define BLOCK_SIZE 512			// RedSea Block Size
#define ISO_9660_SECTOR_SIZE 2048
#include <fuse.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* RedSea FUSE driver
 * for the TempleOS RedSea filesystem
 *
 */

struct redsea_file {
	unsigned char name[39]; // file names can be 38 chars
	unsigned long long int size;
	unsigned long long int block;
};
struct redsea_directory {
	unsigned char name[39];
	unsigned long long int block;
	unsigned long long int size;
	int entries;
	unsigned char* children[];

};

void read_directory(int block, FILE* image) {
	unsigned char* buf;
	fseek(image, block, SEEK_SET);
	fseek(image, 0x47, SEEK_CUR);
	fread(buf, 1, 1, image);
	rewind(image);
}

unsigned int boot_catalog_pointer(FILE* image) {
	unsigned int buf;			// 4 byte
	fseek(image, 0x8800, SEEK_SET);
	fseek(image, 0x47, SEEK_CUR);
	fread(&buf, 4, 1, image);
	rewind(image);
	return buf;
}

/*
 * find block for root directory in allocation bitmap (I think?)
 * block is always 0x58 as far as i know, but kept as a parameter just in case
 * root directory block is given at 0x18, presumably next 7 bytes as well
 */
unsigned long long int root_directory_block(unsigned int block, FILE* image) {
	unsigned long long int root_block;
	fseek(image, (block*BLOCK_SIZE), SEEK_SET);
	fseek(image, 0x18, SEEK_CUR);
	fread(&root_block, 8, 1, image);
	printf("0x%x\n", root_block);
	return root_block;
}

/*
 *Check boot catalog sector for 0x54 0x65 0x64 0x70 block indicating TOS
 */
bool redsea_identity_check(unsigned int boot_catalog, FILE* image) {
	unsigned int buf;
	fseek(image, (boot_catalog*ISO_9660_SECTOR_SIZE+4), SEEK_SET);
	fread(&buf, 4, 1, image);
	unsigned int tos_string = 0x706d6554;	// check for TempleOS fs signature. not the actual signature location
						// always be there. I should probably make this look for the
						// actual RedSea signature
	if (buf != tos_string) return false;
	return true;
}

/*
 * Reads all the files and child directories of a given redsea directory
 * Called in main as redsea_read_files(rdb, image, ".");
 * *directory is the name of the directory being searched.
 * block is the directory's starting block
 * image is the image file being read.
 * size is the size of the directory
 */
void redsea_read_files(unsigned long long int block, unsigned long long int size, FILE* image, unsigned char *directory) {
	uint16_t filetype = 0;		// 0x0810 for directories, 0x0820 for files, 0x0c20 for compressed files
	unsigned char name[39];
	unsigned long long int file_block;
	unsigned long long int file_size;
	int seek_count = 0;
	int subdirec = -1;
	unsigned char subdirectories[size/64][39]; 	// directory size / 64 is max entries in a directory
	unsigned long long int subdirectory_blocks[size/64];
	unsigned long long int subdirectory_sizes[size/64];
	fseek(image, (block*BLOCK_SIZE), SEEK_SET);
	for ( ;; ) {
		fread(&filetype, 2, 1, image);
		seek_count+=2;
		if (filetype == 0) {
			//printf("%d\n", seek_count);
			//printf("%x\n", filetype);
			//printf("directory end\n");
			break;				// end of directory
		}
		printf("0x%04x ", filetype);
		if (filetype == 0x0810) printf("Directory ");
		else if (filetype == 0x0820) printf("File ");
		else if (filetype == 0x0c20) printf("Compressed File ");
		for (int i = 0; i < 38; i++) {
			unsigned char current;
			fread(&current, 1, 1, image);
			if (current == 0 || i == 37) {	// find end of file name
				if (current != 0) {
					name[i] = current;
					i++; 
				}
				name[i] = '\0';		// terminate file name
				fseek(image, 37-i, SEEK_CUR);
				seek_count+= (37-i);
				break;
			}
			name[i] = current;
		}
		fread(&file_block, 8, 1, image);
		seek_count+=8;
		//printf("%x\n", file_block);
		//printf("%s\n", name);
		
		fread(&file_size, 8, 1, image);
		seek_count+=8;
		printf("%s / %s BLOCK %x SIZE %x\n", directory, name, file_block, file_size);
		fseek(image, 8, SEEK_CUR);
		seek_count+=8;
		if (filetype == 0x0810) {
			if (strcmp(directory, name) != 0 && strcmp("..", name) != 0) {
				subdirec++; 
				memcpy(subdirectories[subdirec], name, strlen(name)+1);
				subdirectory_blocks[subdirec] = file_block;
				subdirectory_sizes[subdirec] = file_size;
			}
		}	
	}
	for (int i = 0; i<subdirec; i++) {
		unsigned long long int sub_block = subdirectory_blocks[i];
		unsigned char* sub_name = subdirectories[i];
		unsigned long long int sub_size = subdirectory_sizes[i];
		redsea_read_files(sub_block, sub_size, image, sub_name);
	}

}

static int fuse_rs_file_attributes(const char *path, struct stat *st) {
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

static int fuse_rs_read_directory(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	filler(buffer, ".", NULL, 0);
	filler(buffer, "..", NULL, 0);
	//filler(buffer, "/test", NULL, 0);
	filler(buffer, "test.txt", NULL, 0);
	return 0;
}

static struct fuse_operations redsea_ops = {
	.getattr = fuse_rs_file_attributes,
	.readdir = fuse_rs_read_directory
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
	unsigned int bcp = boot_catalog_pointer(image);
	if (redsea_identity_check(bcp, image)) printf("good\n");
	else printf("bad\n");
	printf("0x%x\n", bcp);
	printf("0x%x\n", 0x8800);
	unsigned int rdb = root_directory_block(0x58, image);	// first param should always be 0x58, but maybe I don't know the specification well enough
	fseek(image, (rdb*BLOCK_SIZE)+48, SEEK_SET);
	unsigned long long int size;
	fread(&size, 8, 1, image);				// get root directory size before reading. probably inefficient	
	printf("size: %x\n", size);
	redsea_read_files(rdb, size, image, ".");

	return fuse_main(argc, argv, &redsea_ops, NULL);
	
}
