#define FUSE_USE_VERSION 34
#define _FILE_OFFSET_BITS 64
#define BLOCK_SIZE 512			// RedSea Block Size
#define ISO_9660_SECTOR_SIZE 2048
#define UNIX_CDATE_SECONDS 62167132800 	// seconds to subtract from CDate seconds for unix time
					// 719527*84000
#define FUSE_CAP_HANDLE_KILLPRIV 0

#include <fuse.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

/* RedSea FUSE driver
 * for the TempleOS RedSea filesystem
 * as of right now read only
 */

struct redsea_file {
	unsigned long long int seek_to;		// seek to here from parent block to get entry
	unsigned char name[38]; 		// file names can be 37 chars + null
	unsigned long long int size;
	unsigned long long int block;
	unsigned long long int mod_date;
	struct redsea_directory* parent;
};
struct redsea_directory {
	unsigned long long int seek_to;		// seek to here from parent block to get entry
	unsigned char name[38];
	unsigned long long int size;
	unsigned long long int block;
	unsigned long long int mod_date;
	unsigned long long int num_children;
	unsigned char** children;
	struct redsea_directory* parent;
	
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
FILE* image;			// image is going to be global. makes it easier
unsigned long long int free_space_pointer = 0;				// just point to the next free available location to expand files
									// really naive implementation but should work.

//Converts TempleOS CDate (Christ date?) format to unix time.
long long int cdate_to_unix(unsigned long long int cdate) {
	unsigned int lower = cdate & 0xFFFFFFFF;
	unsigned int upper = (cdate>>32) & 0xFFFFFFFF;
       	unsigned long long int cdate_upper_seconds = upper*86400LL;
	long int cdate_lower_seconds = lower/49710;
	long long int unix_seconds = cdate_upper_seconds+cdate_lower_seconds-UNIX_CDATE_SECONDS;
	return unix_seconds;
}
//Converts unix time to TempleOS CDate.
unsigned long long int unix_to_cdate(long long int unix_time) {
	unsigned long long int cdate;
	unsigned int lower = (unix_time % 84000LL)*49710;
	unsigned int upper = (unix_time + UNIX_CDATE_SECONDS) / 86400;
	cdate = (unsigned long long int) upper << 32 | lower;
	return cdate;
}

bool is_directory(const char* path) {
	for (int i = 0; i < directory_count; i++) {
		if (strcmp (path, directory_paths[i]) == 0) return true;
	}
	return false;
}

unsigned long long int directory_position(const char* path) {
	for (unsigned long long int i=0; i<directory_count; i++) {
		if (strcmp (path, directory_paths[i]) == 0) return i;
	}
	return -1;
}

unsigned long long int file_position(const char* path) {
	for (unsigned long long int i = 0; i < file_count; i++) {
		if (strcmp (path, file_paths[i]) == 0) return i;
	}
	return -1;
}

// double directory array
void expand_directory_array() {
	max_directory_count *= 2;
	directory_paths = realloc(directory_paths, sizeof(char*)*max_directory_count);
	directory_structs = realloc(directory_structs, sizeof(struct redsea_directory*)*max_directory_count);
}
// double file array
void expand_file_array() {
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
						// but will this will always be there. I should probably make this 
						// look for the actual RedSea signature. :/
	if (buf != tos_string) return false;
	return true;
}

/*
 * Reads all the files and child directories of a given redsea directory
 * First called in main.
 */
void redsea_read_files(struct redsea_directory* directory, unsigned char *path_so_far) {
	
	unsigned long long int block = directory->block;
	unsigned long long int size = directory->size;
	unsigned char *directory_name = directory->name; 
	unsigned long long int num_children = directory->num_children;


	//strcat(path_so_far, "/");
	uint16_t filetype = 0;		// 0x0810 for directories, 0x0820 for files, 0x0c20 for compressed files
	unsigned char name[38];
	unsigned long long int file_block;
	unsigned long long int file_size;
	unsigned long long int timestamp;
	int subdirec = -1;
	struct redsea_directory* subdirectories[size/64]; 	// directory size / 64 is max entries in a directory
	unsigned char subdirectory_paths[size/64][512];
	unsigned char path[strlen(path_so_far)+40];
	fseek(image, (block*BLOCK_SIZE), SEEK_SET);
	for (int i = 0; i < size/64; i++ ) {
		fread(&filetype, 2, 1, image);
		if (filetype == 0) {
			break;				// end of directory
		}
		printf("0x%04x ", filetype);
		// handle deleted files
		// this implementation is INCORRECT and will not work with all potential filetypes. I need to fix that
		// these are attribute flags!
		if (filetype == 0x0910 || filetype == 0x0920 || filetype == 0x0d20 || filetype == 0x0d00 || filetype == 0x0900) {
			printf("DELETED FILE\n");
			fseek(image, 62, SEEK_CUR);
			continue;
		}
		if (filetype == 0x0810) printf("Directory ");
		else if (filetype == 0x0820 || filetype == 0x0800) printf("File ");
		else if (filetype == 0x0c20 || filetype == 0x0c00) printf("Compressed File ");
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
				break;
			}
			name[i] = current;
		}
		fread(&file_block, 8, 1, image);
		
		fread(&file_size, 8, 1, image);
		strcpy(path, path_so_far);
		strcat(path, "/");
		strcat(path, name);
		fread(&timestamp, 8, 1, image);						   			
		printf("%s BLOCK %#lx SIZE %#lx TIMESTAMP %#016lx \n", path, file_block, file_size, timestamp);
		if (filetype == 0x0810) {
			if (strcmp(directory_name, name) != 0 && strcmp("..", name) != 0) {
				subdirec++;

				strcpy(subdirectory_paths[subdirec], path);

				// global directory array
				if (directory_count+1 >= max_directory_count) {
					expand_directory_array();
				}
				directory_paths[directory_count] = malloc(strlen(path));
				strcpy(directory_paths[directory_count], path);	
				struct redsea_directory* directory_entry = malloc(sizeof(struct redsea_directory));
				strcpy(directory_entry->name, name);
				directory_entry -> seek_to = i*64;
				directory_entry -> size = file_size;
				directory_entry -> block = file_block;
				directory_entry -> mod_date = timestamp;
				directory_entry -> children = malloc(sizeof(char*)*(file_size/64));
				directory_entry -> num_children = 0;
				directory_entry -> parent = directory;
				directory_structs[directory_count] = directory_entry;
				subdirectories[subdirec] = directory_entry;
				
				directory_count++;
			}
		}
		else {
			if (file_count+1 >= max_file_count) {
				expand_file_array();
			}
			file_paths[file_count] = malloc(strlen(path)+1);
			strcpy(file_paths[file_count], path);
			// create file entry struct after inserting path
			struct redsea_file* file_entry = malloc(sizeof(struct redsea_file));
			strcpy(file_entry->name, name);
			file_entry -> seek_to = i*64;
			file_entry -> size = file_size;
			file_entry -> block = file_block;
			file_entry -> mod_date = timestamp;
			file_entry -> parent = directory;
			file_structs[file_count] = file_entry;

			file_count++;
		}
		if (strcmp(directory_name, name) != 0 && strcmp("..", name) != 0) {
			directory -> children[num_children] = malloc(sizeof(char)*strlen(name)+1);
			strcpy(directory -> children[num_children], name);
			num_children++;
		}
		// set the free space pointer, eventually to the end of the image.
		if ((file_block*BLOCK_SIZE + file_size + BLOCK_SIZE-1) / BLOCK_SIZE > free_space_pointer) {
			free_space_pointer = (file_block*BLOCK_SIZE+file_size+BLOCK_SIZE-1)/BLOCK_SIZE+2;
		}
	}
	directory->num_children = num_children;
	for (int i = 0; i<=subdirec; i++) {

		struct redsea_directory* subdir = subdirectories[i];
		unsigned char* sub_path = subdirectory_paths[i];
		redsea_read_files(subdir, sub_path);
	}

}

/* Gets the contents of a given file
 * 
 */

unsigned char* redsea_file_content(struct redsea_file* rs_file, size_t size, off_t offset) {
	rewind(image);
	unsigned char* content = malloc(size);
	fseek(image, (rs_file->block)*BLOCK_SIZE+offset, SEEK_SET);
	fread(content, 1, size, image);
	return content;
}

int redsea_remove_common(struct redsea_directory* parent, unsigned long long int seek_to, unsigned char* name) {

	unsigned long long int loc = -1;			//file loc in parent->children
	for (int i = 0; i<parent->num_children; i++) {
		if (strcmp(parent->children[i], name) == 0) {
			loc = i;
			break;
		}
	}
	if (loc == -1) return -1;
	
	// remove file from parent children list.
	// I could also remove it from the global file list but there's no
	// real need to other than for memory concerns. (so I probably should)
	for (unsigned long long int i = loc; i<parent->num_children; i++) {
		parent->children[i]=parent->children[i+1];
	}
	parent->num_children--;

	rewind(image);
	fseek(image, (parent->block*BLOCK_SIZE), SEEK_SET);
	uint16_t filetype;
	fseek(image, seek_to, SEEK_CUR);
	fread(&filetype, 2, 1, image);
	fseek(image, -2, SEEK_CUR);
	filetype += 0x100;					// mark file as deleted
	unsigned char buf[2];
	buf[0] = filetype & 0xff;
	buf[1] = (filetype >> 8) & 0xff;
	fwrite(buf, 2, 1, image);

	return 0;
}

void move_file_to_end(struct redsea_file* file) {
	unsigned long long int new_block = free_space_pointer;
	unsigned long long int old_block = file -> block;
	unsigned long long int size = file -> size;
	unsigned char* buf = malloc(size);
	rewind(image);
	fseek(image, old_block*BLOCK_SIZE, SEEK_SET);
	fread(buf, 1, size, image);
	fseek(image, new_block*BLOCK_SIZE, SEEK_SET);
	fwrite(buf, 1, size, image);
	file -> block = new_block;
	rewind(image);
	fseek(image, file->parent->block*BLOCK_SIZE + file->seek_to, SEEK_SET);
	fseek(image, 40, SEEK_CUR);				// find block
	unsigned char nb_char[8] = {new_block & 0xff, (new_block >> 8) & 0xff, (new_block >> 16) & 0xff, (new_block>>24) & 0xff,
		(new_block>>32) & 0xff, (new_block >> 40) & 0xff, (new_block >> 48) & 0xff, (new_block >> 56) & 0xff};
	fwrite(nb_char, 8, 1, image);
	// should be always true?
	if ((new_block*BLOCK_SIZE + size + BLOCK_SIZE-1) / BLOCK_SIZE > free_space_pointer) {
			free_space_pointer = (new_block*BLOCK_SIZE+size+BLOCK_SIZE-1)/BLOCK_SIZE+1;
	}	
}

// could probably merge a lot of this functions functionality with the above function
// without too many issues

void move_directory_to_end(struct redsea_directory* directory) {
	unsigned long long int new_block = free_space_pointer;
	unsigned long long int old_block = directory -> block;
	unsigned long long int size = directory -> size;
	unsigned char* buf = malloc(size);
	rewind(image);
	fseek(image, old_block*BLOCK_SIZE, SEEK_SET);
	fread(buf, 1, size, image);
	fseek(image, new_block*BLOCK_SIZE, SEEK_SET);
	fwrite(buf, 1, size, image);
	directory -> block = new_block;
	rewind(image);
	fseek(image, directory->parent->block*BLOCK_SIZE + directory->seek_to, SEEK_SET);
	fseek(image, 40, SEEK_CUR);				// find block
	unsigned char nb_char[8] = {new_block & 0xff, (new_block >> 8) & 0xff, (new_block >> 16) & 0xff, (new_block>>24) & 0xff,
		(new_block>>32) & 0xff, (new_block >> 40) & 0xff, (new_block >> 48) & 0xff, (new_block >> 56) & 0xff};
	fwrite(nb_char, 8, 1, image);

	rewind(image); // SEEK_SET just set the pointer. I don't know why I use
		       // rewind so much.
	fseek(image, new_block*BLOCK_SIZE, SEEK_SET);
       	fseek(image, 40, SEEK_CUR);
	fwrite(nb_char, 8, 1, image);
	for (int i = 0; i < directory -> num_children; i++) {
		rewind(image);
		fseek(image, directory->block*BLOCK_SIZE, SEEK_SET);
		uint16_t filetype;
		fseek(image, 128 + i*64, SEEK_CUR);
		fread(&filetype, 2, 1, image);
		if ((filetype >> 5) & 1 == 1) {		// If file is a directory
			fseek(image, 38, SEEK_CUR);
			unsigned long long int subdir_block;
			fread(&subdir_block, 8, 1, image);
			rewind(image);
			fseek(image, subdir_block*BLOCK_SIZE, SEEK_SET);
			fseek(image, 104, SEEK_CUR);
			fwrite(nb_char, 8, 1, image);
		}

	}
	// should be always true? 
	if ((new_block*BLOCK_SIZE + size + BLOCK_SIZE-1) / BLOCK_SIZE > free_space_pointer) {
			free_space_pointer = (new_block*BLOCK_SIZE+size+BLOCK_SIZE-1)/BLOCK_SIZE+1;
	}	
}

void write_file(struct redsea_file* file, const char* buffer, size_t size, off_t offset) {
	unsigned long long int end_after = (((size+offset)-file->size) + file->block*BLOCK_SIZE + file->size + BLOCK_SIZE-1) / BLOCK_SIZE;
	unsigned long long int end_before = (file->size + file->block*BLOCK_SIZE + BLOCK_SIZE-1) / BLOCK_SIZE;
	if (end_after > end_before && (end_before < free_space_pointer-1)) {
		printf("MOVED TO END OF IMAGE !!! \n");
		move_file_to_end(file);
	}
	else printf("NOT MOVED TO END OF IMAGE !!! \n");
	printf("%#x %#x\n", (((size + offset)-file->size) + file->block*BLOCK_SIZE + file->size + BLOCK_SIZE-1) / BLOCK_SIZE, free_space_pointer);
	unsigned long long int block = file -> block;
	rewind(image);
	fseek(image, block*BLOCK_SIZE, SEEK_SET);
	fseek(image, offset, SEEK_CUR);
	// pad last RS block
	fwrite(buffer, size, 1, image);
	file->size += (size+offset)-file->size;			// set file size
	unsigned char size_char[8] = {file->size & 0xff, (file->size >> 8) & 0xff, (file->size >> 16) & 0xff, (file->size>>24) & 0xff,
		(file->size>>32) & 0xff, (file->size >> 40) & 0xff, (file->size >> 48) & 0xff, (file->size >> 56) & 0xff};
	rewind(image);
	fseek(image, file->parent->block*BLOCK_SIZE, SEEK_SET);
	fseek(image, file->seek_to+48, SEEK_CUR);
	fwrite(size_char, 8, 1, image);
	fseek(image, 0, SEEK_END);

	if ((block*BLOCK_SIZE + file->size + BLOCK_SIZE-1) / BLOCK_SIZE > free_space_pointer) {
		free_space_pointer = (block*BLOCK_SIZE+file->size-BLOCK_SIZE-1)/BLOCK_SIZE+1;
	}	

}

/*Rewrite the redsea boot area.
 *Called upon filesystem destruction
 */

void rewrite_redsea_boot() {
	fseek(image, 0, SEEK_END);
	unsigned long long int end = ftell(image);
	unsigned int end_sector = end / ISO_9660_SECTOR_SIZE;			// this number is only 32 bit which puts a
										// limitation on ISO.C files with redsea, which can
										// theoretically support much larger filesystems.
										// You should never have a redsea fs that large though.
										// That's not what RedSea is for
	unsigned long long int end_block = end / BLOCK_SIZE - 0x58;		// minus 0x58 for start block
	printf("EOF SECTOR: %#x\n", end);
	
	// ISO 9660 is both little AND big endian 
	unsigned char ISO_9660_buffer[8] = {end_sector & 0xff, (end_sector >> 8) & 0xff, (end_sector >> 16) & 0xff, 
		(end_sector >> 24) & 0xff, (end_sector >> 24) & 0xff, (end_sector >> 16) & 0xff, (end_sector >> 8) & 0xff, end_sector & 0xff};
	rewind(image);
	fseek(image, 0x8050, SEEK_SET);
	fwrite(ISO_9660_buffer, 8, 1, image);
	fseek(image, 0xff8, SEEK_CUR);		// to 0x9050
	fwrite(ISO_9660_buffer, 8, 1, image);
	unsigned char rs_buffer[8] = {end_block & 0xff, (end_block >> 8) & 0xff, (end_block >> 16) & 0xff, 
		(end_block >> 24) & 0xff, (end_block >> 32) & 0xff, (end_block >> 40) & 0xff, (end_block >> 48) & 0xff, (end_block >> 56) & 0xff};
	fseek(image, 0x1fa8, SEEK_CUR);		// to 0xB000
	fseek(image, 16, SEEK_CUR);
	fwrite(rs_buffer, 8, 1, image);
}

/*
 * Function for use in both unlink and rmdir
 */

static int fuse_rs_file_attributes(const char *path, struct stat *st) {
	if (strcmp(path, "/") == 0 || is_directory(path)) {
		unsigned long long int did = directory_position(path);			// did is Directory ID
		if (did == -1) return -1;
		st->st_size = directory_structs[did]->size;
		st->st_mtime = cdate_to_unix(directory_structs[did]->mod_date);
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
		
	}
	else {
		unsigned long long int fid = file_position(path);			// fid is File ID
		if (fid == -1) return -1;
		st->st_size = file_structs[fid]->size;
		st->st_mtime = cdate_to_unix(file_structs[fid]->mod_date);
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
	unsigned long long int did = directory_position(path);				// did is Directory ID
	unsigned long long int num_children = directory_structs[did]->num_children;
	if (did==-1) return -1;
	for (int j = 0; j<num_children; j++) {
		filler(buffer, directory_structs[did]->children[j], NULL, 0);
	}
	return 0;
}

static int fuse_rs_read_file(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	unsigned long long int fid = file_position(path);
	if (fid == -1) return -1;
	unsigned char* file_contents;

	file_contents = redsea_file_content(file_structs[fid], size, offset);
	memcpy(buffer, file_contents, size);
	free(file_contents);

	return size;	
}

static int fuse_rs_unlink_file(const char* path) {
	unsigned long long int fid = file_position(path);
	if (fid == -1) {
		if (directory_position(path) != 0) errno = EISDIR;
		return -1;
	}
	struct redsea_file* file = file_structs[fid];
	unsigned char* name = file -> name;
	struct redsea_directory* parent = file -> parent;
	unsigned long long int seek_to = file -> seek_to;	
	return redsea_remove_common(parent, seek_to, name);
}
/* Delete directory:
 * I am pretty sure this deletion is incomplete according to
 * the RedSea specifications as "." and ".." are never marked
 * as deleted. TempleOS still recognizes it though, so it
 * should be fine.
 */
static int fuse_rs_rmdir(const char* path) {
	unsigned long long int did = directory_position(path);
	if (did == -1) {
		errno = ENOTDIR;
		return -1;
	}
	struct redsea_directory* directory = directory_structs[did];
	if (directory->num_children != 0) {
		errno = ENOTEMPTY;
		return -1;
	}
	unsigned char* name = directory -> name;
	struct redsea_directory* parent = directory -> parent;
	unsigned long long int seek_to = directory -> seek_to;
	return redsea_remove_common(parent, seek_to, name);
}

// Both of the below functions just return 0. Unneccesary for redsea
static int fuse_rs_open_file(const char* path, struct fuse_file_info* fi) {
	return 0;
}

static int fuse_rs_open_dir(const char* path, struct fuse_file_info* fi) {
	return 0;
}

static int fuse_rs_write(const char* path, const char* buffer, size_t size, off_t offset, struct fuse_file_info* fi) {
	unsigned long long int fid = file_position(path);
	if (fid == -1) return -1;
	write_file(file_structs[fid], buffer, size, offset);
	return size;
}

static int fuse_rs_truncate(const char* path, off_t offset) {	

	// we are just going to ignore truncate ;)
	return offset;
}

static void fuse_rs_destroy() {
	rewind(image);
	fseek(image, 0, SEEK_END);
	int padding = 2048-ftell(image)%2048;
	printf("END PADDING: %#x \n", padding);
	if (padding != 2048 && padding != 0) {
		unsigned char* buf = calloc(padding, 1);
		fwrite(buf, 1, padding, image);
	}
	rewrite_redsea_boot();
}

static struct fuse_operations redsea_ops = {
	.getattr = fuse_rs_file_attributes,
	.readdir = fuse_rs_read_directory,
	.read = fuse_rs_read_file,					
	.unlink = fuse_rs_unlink_file,
	.rmdir = fuse_rs_rmdir,
	.write = fuse_rs_write,
	.truncate = fuse_rs_truncate,
	.open = fuse_rs_open_file,				// open and opendir should just return 0 afaik. no need
	.opendir = fuse_rs_open_dir,				// for stuff like that in RedSea
	.destroy = fuse_rs_destroy,
	
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
		devfile = argv[i];
		printf("%s\n", devfile);
		memcpy(&argv[i], &argv[i+1], (argc-i) * sizeof(argv[0]));
		argc--;
	
		image = fopen(devfile, "rb+");				// open disk image
		unsigned int bcp = boot_catalog_pointer(image);
		if (redsea_identity_check(bcp, image)) printf("good\n");
		else printf("bad. not redsea?\n");
		printf("%#x\n", bcp);
		unsigned int rdb = root_directory_block(0x58, image);	// first param should always be 0x58, but maybe I don't know the specification well enough
		fseek(image, (rdb*BLOCK_SIZE)+48, SEEK_SET);
		unsigned long long int size;
		fread(&size, 8, 1, image);				// get root directory size before reading. probably inefficient	
		printf("size: %#lx\n", size);
		struct redsea_directory* root_directory = malloc(sizeof(struct redsea_directory));
		strcpy(root_directory->name, ".");
		root_directory->seek_to = 0;
		root_directory->size = size;
		root_directory->block = rdb;
		root_directory->mod_date = 0;
		root_directory->num_children = 0;
		root_directory->children = malloc(sizeof(char*)*(size/64));
		root_directory->parent = NULL;
		directory_paths[0] = "/";
		directory_structs[0] = root_directory;

		free_space_pointer = rdb;

		//redsea_read_files(rdb, size, image, ".", "");
		redsea_read_files(root_directory, "");
	}

	return fuse_main(argc, argv, &redsea_ops, NULL);
	
}
