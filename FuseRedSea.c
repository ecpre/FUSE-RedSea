#define FUSE_USE_VERSION 29
#define _FILE_OFFSET_BITS 64
#define BLOCK_SIZE 512			// RedSea Block Size
#define ISO_9660_SECTOR_SIZE 2048
#define UNIX_CDATE_SECONDS 62167132800 	// seconds to subtract from CDate seconds for unix time
					// 719527*84000

#include <fuse.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

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
				directory_paths[directory_count] = malloc(strlen(path)+1);
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
		if (file_block != 0xFFFFFFFFFFFFFFFF) {					// templeos puts empty files at 0xFFFFFFFFFFFFFFFF
											// but nothing actually gets created there
											// (it would be rather difficult to find a 
											// 16 EiB disk anways)
			
			// below really should probably be > and not >= but it makes it slightly easier to deal with 0
			// size files. I should have just gone the proper TOS implementation route in doing this.
			// but I didn't
			if ((file_block*BLOCK_SIZE + file_size + BLOCK_SIZE-1) / BLOCK_SIZE >= free_space_pointer) {
				free_space_pointer = (file_block*BLOCK_SIZE+file_size+BLOCK_SIZE-1)/BLOCK_SIZE+1;
			}
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
	free(buf);	

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
	free(buf);

	unsigned char* blank = calloc(BLOCK_SIZE, 1);
	fwrite(blank, BLOCK_SIZE, 1, image);
	free(blank);

	// add more space for directory
	size += BLOCK_SIZE;
	directory->size = size;
	directory->children = realloc(directory->children, sizeof(char*)*size/64);

	rewind(image);
	if (strcmp(directory->name, ".") != 0) {
		fseek(image, directory->parent->block*BLOCK_SIZE + directory->seek_to, SEEK_SET);
		fseek(image, 40, SEEK_CUR);				// find block
	}
	unsigned char nb_char[8] = {new_block & 0xff, (new_block >> 8) & 0xff, (new_block >> 16) & 0xff, (new_block>>24) & 0xff,
		(new_block>>32) & 0xff, (new_block >> 40) & 0xff, (new_block >> 48) & 0xff, (new_block >> 56) & 0xff};
	unsigned char size_char[8] = {size & 0xff, (size >> 8) & 0xff, (size >> 16) & 0xff, (size >> 24) & 0xff, (size >> 32) & 0xff,
		(size >> 40) & 0xff, (size >> 48) & 0xff, (size >> 56) & 0xff};
	if (strcmp(directory->name, ".") != 0) {
		fwrite(nb_char, 8, 1, image);
		fwrite(size_char, 8, 1, image);
	}

	rewind(image); // SEEK_SET just set the pointer. I don't know why I use
		       // rewind so much.
	fseek(image, new_block*BLOCK_SIZE, SEEK_SET);
       	fseek(image, 40, SEEK_CUR);
	fwrite(nb_char, 8, 1, image);
	fwrite(size_char, 8, 1, image);
	for (int i = 0; i < directory -> num_children; i++) {
		rewind(image);
		fseek(image, directory->block*BLOCK_SIZE, SEEK_SET);
		uint16_t filetype;
		fseek(image, 128 + i*64, SEEK_CUR);
		fread(&filetype, 2, 1, image);
		if ((filetype >> 4) & 1 == 1) {		// If file is a directory
			fseek(image, 38, SEEK_CUR);
			unsigned long long int subdir_block;
			fread(&subdir_block, 8, 1, image);
			rewind(image);
			fseek(image, subdir_block*BLOCK_SIZE, SEEK_SET);
			fseek(image, 104, SEEK_CUR);
			fwrite(nb_char, 8, 1, image);
			unsigned char* blank = calloc(8, 1);
			fwrite(blank, 8, 1, image);
			free(blank);
		}

	}

	/*Root directory pointer also seems to follow both endian. poses a problem for resizing
	 *root directory past block 0xFFFFFFFF 
	 */
	unsigned char rdb_char[8] = {nb_char[0], nb_char[1], nb_char[2], nb_char[3], nb_char[3], nb_char[2], nb_char[1], nb_char[0]};
	printf("%s !!!!!!! \n", directory->name);
	if (strcmp(directory->name, ".") == 0) {
		printf("enter rdb rewrite!!!! \n");
		rewind(image);
		fseek(image, 0x8098, SEEK_SET);
		fwrite(rdb_char, 8, 1, image);
		fseek(image, 0xff8, SEEK_CUR);
		fwrite(rdb_char, 8, 1, image);
		fseek(image, 0x1f78, SEEK_CUR);
		fwrite(nb_char, 8, 1, image);
		rewind(image);
		fseek(image, new_block*BLOCK_SIZE, SEEK_SET);
		fseek(image, 104, SEEK_CUR);
		fwrite(nb_char, 8, 1, image);
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
	fwrite(buffer, size, 1, image);

	// is this check unnecessary after properly implementing truncate? I think so? I'll leave it in.
	if ((size+offset) > file->size) {
		file->size += (size+offset)-file->size;			// set file size
	}
	long long int unix_time = time(NULL);
	unsigned long long int CDate = unix_to_cdate(unix_time);

	file->mod_date = CDate;

	unsigned char size_char[8] = {file->size & 0xff, (file->size >> 8) & 0xff, (file->size >> 16) & 0xff, (file->size>>24) & 0xff,
		(file->size>>32) & 0xff, (file->size >> 40) & 0xff, (file->size >> 48) & 0xff, (file->size >> 56) & 0xff};
	unsigned char cdate_char[8] = {CDate & 0xff, (CDate >> 8) & 0xff, (CDate >> 16) & 0xff, (CDate >> 24) & 0xff, (CDate >> 32) & 0xff,
		(CDate >> 40) & 0xff, (CDate >> 48) & 0xff, (CDate >> 56) & 0xff};
	rewind(image);
	fseek(image, file->parent->block*BLOCK_SIZE, SEEK_SET);
	fseek(image, file->seek_to+48, SEEK_CUR);
	fwrite(size_char, 8, 1, image);
	fwrite(cdate_char, 8, 1, image);
	fseek(image, 0, SEEK_END);
	
	if ((block*BLOCK_SIZE + file->size + BLOCK_SIZE-1) / BLOCK_SIZE >= free_space_pointer) {
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

unsigned long long int find_free_dir_entry(struct redsea_directory* directory) {
	unsigned long long int free_pos = directory->block*BLOCK_SIZE;
	rewind(image);
	fseek(image, free_pos, SEEK_SET);
	fseek(image, 128, SEEK_CUR);
	for (int i = 0; i < directory->num_children; i++) {
		uint16_t filetype;
		fread(&filetype, 2, 1, image);
		if ((filetype >> 8) & 1 == 1) {			// if file is deleted it is free
			fseek(image, -2, SEEK_CUR);
			break;
		}
		fseek(image, 62, SEEK_CUR);
	}
	free_pos = ftell(image);
	return free_pos;
}

unsigned long long int add_entry_to_dir(struct redsea_directory* directory, uint16_t attributes, unsigned char* name, unsigned long long int block, unsigned long long int size, unsigned long long int timestamp) {
	unsigned long long int next_free = find_free_dir_entry(directory);

	// I should really just write a function to do this work for me..	
	unsigned char att_buf[2] = {attributes & 0xff, (attributes >> 8) & 0xff};
	unsigned char block_buf[8] = {block & 0xff, (block >> 8) & 0xff, (block >> 16) & 0xff, (block >> 24) & 0xff, (block >> 32) & 0xff, (block >> 40) & 0xff,
		(block >> 48) & 0xff, (block >> 56) & 0xff};
	unsigned char size_buf[8] = {size & 0xff, (size >> 8) & 0xff, (size >> 16) & 0xff, (size >> 24) & 0xff, (size >> 32) & 0xff, (size >> 40) & 0xff,
		(size >> 48) & 0xff, (size  >> 56) & 0xff};
	unsigned char timestamp_buf[8] = {timestamp & 0xff, (timestamp >> 8) & 0xff, (timestamp >> 16) & 0xff, (timestamp >> 24) & 0xff, (timestamp >> 32) & 0xff, (timestamp >> 40) & 0xff,
		(timestamp >> 48) & 0xff, (timestamp >> 56) & 0xff};
	rewind(image);
	fseek(image, next_free, SEEK_SET);
	fwrite(att_buf, 2, 1, image);
	unsigned char* name_buf = calloc(38, 1);
	strcpy(name_buf, name);
	fwrite(name_buf, 38, 1, image);
	fwrite(block_buf, 8, 1, image);
	fwrite(size_buf, 8, 1, image);
	fwrite(timestamp_buf, 8, 1, image);
	
	// if directory
	if ((attributes >> 4) & 1 == 1) {
		printf("DIR!!!\n");
		rewind(image);
		fseek(image, block*BLOCK_SIZE, SEEK_SET);
		fwrite(att_buf, 2, 1, image);
		fwrite(name_buf, 38, 1, image);
		fwrite(block_buf, 8, 1, image);
		fwrite(size_buf, 8, 1, image);
		fwrite(timestamp_buf, 8, 1, image);
		rewind(image);
		fseek(image, directory->block*BLOCK_SIZE, SEEK_SET);
		unsigned char* parent_buf = calloc(64, 1);
		fread(parent_buf, 64, 1, image);
		rewind(image);
		fseek(image, block*BLOCK_SIZE, SEEK_SET);
		fseek(image, 64, SEEK_CUR);
		fwrite(parent_buf, 64, 1, image);
		fseek(image, -62, SEEK_CUR);
		unsigned char* par_name_buf = calloc(38,1);
		strcpy(par_name_buf, "..");
		fwrite(par_name_buf, 38, 1, image);
		fseek(image, 8, SEEK_CUR);
		unsigned char* blank = calloc(8, 1);
		fwrite(blank, 8, 1, image);
		free(blank);
		// overwrite anything after so no tos errors occur
		blank = calloc(0x180, 1);
		fseek(image, 8, SEEK_CUR);
		fwrite(blank, 0x180, 1, image);
		free(parent_buf);
		free(par_name_buf);
		free(blank);
	}
	else {
		printf("FILE!!!\n");
	}

	free(name_buf);

	return next_free - directory->block*BLOCK_SIZE;

}

static int fuse_rs_file_attributes(const char *path, struct stat *st) {

	if (strncmp(path, "/.Trash", 7) == 0) {
		errno = ENOENT;
		return -errno;

	}

	else if (strcmp(path, "/") == 0 || is_directory(path)) {
		unsigned long long int did = directory_position(path);			// did is Directory ID
		if (did == -1) return -1;
		st->st_size = directory_structs[did]->size;
		st->st_mtime = cdate_to_unix(directory_structs[did]->mod_date);
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
		
	}
	else {
		unsigned long long int fid = file_position(path);			// fid is File ID
		if (fid == -1) {
			errno = ENOENT;
			return -errno;	
		}
		else {
			st->st_size = file_structs[fid]->size;
			st->st_mtime = cdate_to_unix(file_structs[fid]->mod_date);
			st-> st_mode = S_IFREG | 0644;
			st-> st_nlink = 2;
		}
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
	printf("FREE_POS: %#x\n", find_free_dir_entry(directory_structs[did]));
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
	printf("FID: %lld !!!!!!!\n");
	if (fid == -1) {
		if (directory_position(path) != -1) errno = EISDIR;
		return -errno;
	}
	struct redsea_file* file = file_structs[fid];
	unsigned char* name = file -> name;
	struct redsea_directory* parent = file -> parent;
	unsigned long long int seek_to = file -> seek_to;	
	if (redsea_remove_common(parent, seek_to, name) == -1) return -1;

	for (unsigned long long int i = fid; i<file_count; i++) {
		file_structs[i]=file_structs[i+1];
		file_paths[i]=file_paths[i+1];
	}
	file_count--;

	return 0;
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
		return -errno;
	}
	struct redsea_directory* directory = directory_structs[did];
	if (directory->num_children != 0) {
		errno = ENOTEMPTY;
		return -errno;
	}
	unsigned char* name = directory -> name;
	struct redsea_directory* parent = directory -> parent;
	unsigned long long int seek_to = directory -> seek_to;

	if (redsea_remove_common(parent, seek_to, name) == -1) return -1;

	for (unsigned long long int i = did; i<directory_count; i++) {
		directory_structs[i]=directory_structs[i+1];
		directory_paths[i]=directory_paths[i+1];
	}
	directory_count--;

	return 0;

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

static int fuse_rs_truncate(const char* path, off_t length) {

	/* It seems that I forgot the proper reasons for truncate to exist when I
	 * release this last time. :(
	 * Corrupted files shouldn't be an issue anymore
	 */	
	unsigned long long int fid = file_position(path);
	struct redsea_file* file = file_structs[fid];
	
	file->size = length;
	
	unsigned char size_char[8] = {length & 0xff, (length >> 8) & 0xff, (length >> 16) & 0xff, (length >> 24) & 0xff,
		(length >> 32) & 0xff, (length >> 40) & 0xff, (length >> 48) & 0xff, (length >> 56) & 0xff};

	rewind(image);
	fseek(image, file->parent->block*BLOCK_SIZE, SEEK_SET);
	fseek(image, file->seek_to, SEEK_CUR);
	fseek(image, 48, SEEK_CUR);
	fwrite(size_char, 8, 1, image);
	
	return length;
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
	fclose(image);
}

static int fuse_rs_create(const char* path, mode_t perms, struct fuse_file_info* fi) {
	// check if it already exists
	printf("!!! ENTER RS CREATE !!!\n");
	unsigned long long int fid = file_position(path);
	printf("FID: %#x\n", fid);
	if (fid != -1) {
		errno = EEXIST;
		return -errno; 	
	}
	
	char* last_slash = strrchr(path, '/');

	int dirlen = last_slash - path;
	char* directory_path = calloc(dirlen+1,1);
	strncpy(directory_path, path, dirlen);
	
	fprintf(stderr, "%s\n", directory_path);

	if (strcmp(directory_path, "") == 0) {
		strncpy(directory_path, "/", 1);
	}
	
	unsigned long long int did = directory_position(directory_path);

	if (did == -1) {
		printf("%s\n", directory_path);
		printf("ENOENT !!!! \n");
		errno = ENOENT;
		return -errno;
	}
	
	struct redsea_directory* parent = directory_structs[did];

	if (parent->num_children+2 >= parent->size/64) {
		move_directory_to_end(parent);	
	}

	// if name too long
	if (strlen(last_slash+1) > 38) {
		errno = ENAMETOOLONG;
		return -errno;
	}

	uint16_t filetype = 0;
	long long int unix_time = time(NULL);
	unsigned long long int CDate = unix_to_cdate(unix_time);
	unsigned long long int size = 0;
	unsigned long long int block = free_space_pointer;
	free_space_pointer++;
	unsigned char* name = calloc(38,1);
	strcpy(name, last_slash+1);

	// if compressed
	if (strcmp(last_slash+strlen(last_slash)-2, ".Z") == 0) {
		filetype += 0x400;
	}
	filetype += 0x800;	// contiguous
	
	unsigned long long int seek_to = add_entry_to_dir(parent, filetype, name, block, size, CDate);
	
	if (file_count+1 > max_file_count) expand_file_array();
	
	struct redsea_file* new_file = malloc(sizeof(struct redsea_file));
	new_file -> seek_to = seek_to;
	strcpy(new_file->name, name);
	new_file -> size = size;
	new_file -> block = block;
	new_file -> mod_date = CDate;
	new_file -> parent = parent;

	parent->children[parent->num_children] = malloc(strlen(name)+1);
	strcpy(parent->children[parent->num_children], name);
	parent->num_children = parent->num_children+1;

	unsigned char* pathcp = malloc(strlen(path)+1);
	strcpy(pathcp, path);
	printf("%s\n", pathcp);
	file_paths[file_count] = pathcp;
	file_structs[file_count] = new_file;

	file_count++;

	free(name);

	return 0;

}

/* mkdir operation
 *
 * a LOT of overlap between this and the above function. I should
 * probably make this have some sort of common function.
 */

static int fuse_rs_mkdir(const char* path, mode_t perms) {
	unsigned long long int did = directory_position(path);

	if (strncmp(path, "/.Trash", 7) == 0) {
		errno = EACCES;
		return -errno;
	}

	if (did != -1) {
		errno = EEXIST;
		return -errno;
	}

	char* last_slash = strrchr(path, '/');

	int parlen = last_slash - path;
	char* parent_path = calloc(parlen+1, 1);
	strncpy(parent_path, path, parlen);

	if (strcmp(parent_path, "") == 0) {
		strncpy(parent_path, "/", 1);
	}
	
	unsigned long long int pdid = directory_position(parent_path);
	
	if (pdid == -1) {
		errno = ENOENT;
		return -errno;
	}
	
	struct redsea_directory* parent = directory_structs[pdid];

	if (parent->num_children+2 >= parent->size/64) {
		move_directory_to_end(parent);	
	}

	if (strlen(last_slash+1) > 38) {
		errno = ENAMETOOLONG;
		return -errno;
	}
	
	uint16_t filetype = 0;
	long long int unix_time = time(NULL);
	unsigned long long int CDate = unix_to_cdate(unix_time);
	unsigned long long int size = 512;
	unsigned long long int block = free_space_pointer;
	free_space_pointer++;
	unsigned char* name = calloc(38, 1);
	strcpy(name, last_slash+1);

	filetype += 0x800;	// contiguous
	filetype += 0x10;	// directory
	
	unsigned long long int seek_to = add_entry_to_dir(parent, filetype, name, block, size, CDate);

	if (directory_count+1 > max_directory_count) expand_directory_array();

	struct redsea_directory* new_dir = malloc(sizeof(struct redsea_directory));
	new_dir -> seek_to = seek_to;
	strcpy(new_dir->name, name);
	new_dir -> size = size;
	new_dir -> block = block;
	new_dir -> mod_date = CDate;
	new_dir -> parent = parent;
	new_dir -> children = malloc(sizeof(char*)*(size/64));
	new_dir -> num_children = 0;

	parent->children[parent->num_children] = malloc(strlen(name)+1);
	strcpy(parent->children[parent->num_children], name);
	parent->num_children = parent->num_children+1;

	unsigned char* pathcp = malloc(strlen(path)+1);
	strcpy(pathcp, path);
	printf("%s\n", pathcp);
	directory_paths[directory_count] = pathcp;
	directory_structs[directory_count] = new_dir;

	directory_count++;

	free(name);
	return 0;
}

static int fuse_rs_rename(const char* path, const char* newpath) {
	printf("TEST \n");

	unsigned long long int fid = file_position(path);
	unsigned long long int did = directory_position(path);
	if (fid == -1 && did == -1) {
		printf("error????? \n");
		errno = ENOENT;
		return -errno;
	}

	
	struct redsea_directory* parent;
	unsigned long long int seek_to;

	unsigned char* last_slash = strrchr(newpath, '/');
	unsigned char* new_name = calloc(38, 1);
	unsigned char* pathcp = malloc(strlen(newpath)+1);
	unsigned char* old_name = calloc(38, 1);
	strcpy(pathcp, newpath);

	if (strlen(last_slash+1) > 38) {
		errno = ENAMETOOLONG;
		return -errno;
	}
	strcpy(new_name, last_slash+1);

	printf("NEW NAME: %s !!!!!\n", new_name);

	if (did != -1) {
		strcpy(old_name, directory_structs[did]->name);
		strcpy(directory_structs[did] -> name, new_name);
		parent = directory_structs[did] -> parent;
		seek_to = directory_structs[did] -> seek_to;
		directory_paths[did] = pathcp;
	}
	else {
		strcpy(old_name, file_structs[fid]->name);
		strcpy(file_structs[fid] -> name, new_name);
		parent = file_structs[fid] -> parent;
		seek_to = file_structs[fid] -> seek_to;
		file_paths[fid] = pathcp;
	}
	
	unsigned long long int pid;

	for (int i = 0; i < parent->num_children; i++) {
		if (strcmp(old_name, parent->children[i]) == 0) {
			pid = i;
			break;
		}
	}
	printf("PID: %d\n", pid);

	parent->children[pid] = malloc(strlen(new_name)+1);
	strcpy(parent->children[pid], new_name);

	printf("SF??\n");
	rewind(image);
	fseek(image, parent->block*BLOCK_SIZE, SEEK_SET);
	fseek(image, seek_to, SEEK_CUR);
	fseek(image, 2, SEEK_CUR);
	fwrite(new_name, 38, 1, image); 
	
	return 0;
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
	.create = fuse_rs_create,
	.mkdir = fuse_rs_mkdir,
	.rename = fuse_rs_rename,
	
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
