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
	unsigned long long int mod_date;
};
struct redsea_directory {
	unsigned char name[39];
	unsigned long long int size;
	unsigned long long int block;
	unsigned long long int mod_date;
	unsigned long long int num_children;
	unsigned char** children;
	
};


// maybe not best practice to have this all global, but there's no reason my 
// functions *shouldn't* be able to access this. TempleOS is like a motorcyle
// if you lean too far you'll crash. Don't do that
int max_directory_count = 20;
int max_file_count = 20;
char** directory_paths;		// malloced in main
char** file_paths;		// 20 max default, will be expanded if exceeded
struct redsea_directory** directory_structs;
struct redsea_file** file_structs;
int directory_count = 1;
int file_count = 0;

bool is_directory(const char *path) {
	for (int i = 0; i < directory_count; i++) {
		if (strcmp (path, directory_paths[i]) == 0) return true;
	}
	return false;
}
// double directory array
void expand_directories() {
	max_directory_count *= 2;
	directory_paths = realloc(directory_paths, sizeof(char*)*max_directory_count);
	directory_structs = realloc(directory_structs, sizeof(struct redsea_directory*)*max_directory_count);
}
// double file array
void expand_files() {
	max_file_count *= 2;
	file_paths = realloc(file_paths, sizeof(char*)*max_file_count);
	file_structs = realloc(file_structs, sizeof(struct redsea_file*)*max_file_count);
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
						// but will always be there. I should probably make this look for the
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
//void redsea_read_files(unsigned long long int block, unsigned long long int size, FILE* image, unsigned char *directory, unsigned char *path_so_far) {
void redsea_read_files(struct redsea_directory* directory, FILE* image, unsigned char *path_so_far) {
	
	unsigned long long int block = directory->block;
	unsigned long long int size = directory->size;
	unsigned char *directory_name = directory->name; 
	unsigned long long int num_children = directory->num_children;


	//strcat(path_so_far, "/");
	uint16_t filetype = 0;		// 0x0810 for directories, 0x0820 for files, 0x0c20 for compressed files
	unsigned char name[39];
	unsigned long long int file_block;
	unsigned long long int file_size;
	unsigned long long int timestamp;
	int seek_count = 0;
	int subdirec = -1;
	struct redsea_directory* subdirectories[size/64]; 	// directory size / 64 is max entries in a directory
	unsigned char subdirectory_paths[size/64][512];
	//unsigned long long int subdirectory_blocks[size/64];
	//unsigned long long int subdirectory_sizes[size/64];
	unsigned char path[strlen(path_so_far)+40];
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
		strcpy(path, path_so_far);
		strcat(path, "/");
		strcat(path, name);
		fread(&timestamp, 8, 1, image);	// TODO: This is the date timestamp. Follows format of 0xFFFFFFFF 0xFFFFFFFF
					   	// with upper 32 bits being days since Christ's birth on January 2nd 1BC
					   	// and lower 32 bits being the time of day divided by 4 billion

		printf("%s BLOCK %x SIZE %x TIMESTAMP %x \n", path, file_block, file_size, timestamp);
		seek_count+=8;
		if (filetype == 0x0810) {
			if (strcmp(directory_name, name) != 0 && strcmp("..", name) != 0) {
				subdirec++;

				strcpy(subdirectory_paths[subdirec], path);

				// global directory array
				if (directory_count+1 >= max_directory_count) {
					expand_directories();
				}
				directory_paths[directory_count] = malloc(strlen(path));
				strcpy(directory_paths[directory_count], path);	
				struct redsea_directory* directory_entry = malloc(sizeof(struct redsea_directory));
				strcpy(directory_entry->name, name);
				directory_entry -> size = file_size;
				directory_entry -> block = file_block;
				directory_entry -> mod_date = timestamp;
				directory_entry -> children = malloc(sizeof(char*)*(file_size/64));
				directory_entry -> num_children = 0;
				directory_structs[directory_count] = directory_entry;
				subdirectories[subdirec] = directory_entry;
				
				directory_count++;
			}
		}
		else {
			if (file_count+1 >= max_file_count) {
				expand_files();
			}
			file_paths[file_count] = malloc(strlen(path)+1);
			strcpy(file_paths[file_count], path);
			printf("%s\n", file_paths[file_count]);
			// create file entry struct after inserting path
			struct redsea_file* file_entry = malloc(sizeof(struct redsea_file));
			strcpy(file_entry->name, name);	
			file_entry -> size = file_size;
			file_entry -> block = file_block;
			file_entry -> mod_date = timestamp;
			file_structs[file_count] = file_entry;

			file_count++;
		}
		if (strcmp(directory_name, name) != 0 && strcmp("..", name) != 0) {
			directory -> children[num_children] = malloc(sizeof(char)*strlen(name)+1);
			strcpy(directory -> children[num_children], name);
			num_children++;
		}
	}
	directory->num_children = num_children;
	for (int i = 0; i<=subdirec; i++) {

		struct redsea_directory* subdir = subdirectories[i];
		unsigned char* sub_path = subdirectory_paths[i];
		redsea_read_files(subdir, image, sub_path);
		printf("EOD\n");
	}

}

static int fuse_rs_file_attributes(const char *path, struct stat *st) {
	if (strcmp(path, "/") == 0 || is_directory(path)) {
		for (int i = 0; i < directory_count; i++) {
			if (strcmp(path, directory_paths[i]) == 0) {
				st->st_size = directory_structs[i]->size;
			}
		}
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
	}
	else {
		for (int i = 0; i<file_count; i++) {
			if (strcmp(path, file_paths[i]) == 0) {
				st->st_size = file_structs[i]->size;
			}	
		}
		st-> st_mode = S_IFREG | 0644;
		st-> st_nlink = 2;
	}
	//st->st_size = 512;
	st->st_uid = getuid();
	st->st_gid = getgid();
	return 0;
}

static int fuse_rs_read_directory(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	filler(buffer, ".", NULL, 0);
	filler(buffer, "..", NULL, 0);
	for (int i = 0; i < directory_count; i++) {
		if (strcmp(path, directory_paths[i]) == 0) {
			unsigned long long int num_children = directory_structs[i]->num_children;
			for (int j = 0; j<num_children; j++) {
				filler(buffer, directory_structs[i]->children[j], NULL, 0);
			}
		}
	}
	return 0;
}

static struct fuse_operations redsea_ops = {
	.getattr = fuse_rs_file_attributes,
	.readdir = fuse_rs_read_directory
};

char *devfile = NULL;

int main(int argc, char **argv) {
	directory_paths = malloc(sizeof(char*)*max_directory_count);
	directory_structs = malloc(sizeof(struct redsea_directory*)*max_directory_count);
	file_paths = malloc(sizeof(char*)*max_file_count);
	file_structs = malloc(sizeof(struct redsea_file*)*max_directory_count);
	int i;
	for (i = 1; i<argc && argv[i][0] == '-'; i++);
	if (i < argc) {
		devfile = realpath(argv[i], NULL);
		printf("%s\n", devfile);
		memcpy(&argv[i], &argv[i+1], (argc-i) * sizeof(argv[0]));
		argc--;
	}

	FILE* image = fopen(devfile, "rb");		// open disk image
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
	struct redsea_directory* root_directory = malloc(sizeof(struct redsea_directory));
	strcpy(root_directory->name, ".");
	root_directory->size = size;
	root_directory->block = rdb;
	root_directory->mod_date = 0;
	root_directory->num_children = 0;
	root_directory->children = malloc(sizeof(char*)*(size/64));
	directory_paths[0] = "/";
	directory_structs[0] = root_directory;

	//redsea_read_files(rdb, size, image, ".", "");
	redsea_read_files(root_directory, image, "");

	return fuse_main(argc, argv, &redsea_ops, NULL);
	
}
