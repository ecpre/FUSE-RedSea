
# RedSea FUSE Drivers

FUSE drivers to support the RedSea filesystem. Currently read only.



## Features

- Read-only support for the RedSea filesystem on TempleOS ISO.C files
- Should hopefully support writing as development continues and I look into how space allocation works.


## Documentation

To use this program, you should have FUSE installed on your system. You should then be able to run the fuse drivers with `./redsea [RedSea.ISO.C] [directory]`

where `RedSea.ISO.C` is any RedSea ISO.C file and `directory` is the directory you wish to view the filesystem in.

## RedSea Documentation

Some documenation of what I know about the RedSea filesystem


### Blocks

RedSea operates with 512 byte Blocks

### Headers

RedSea ISO.C file headers follow the ISO9660 format w/ El Torito extension for the first couple of blocks.

In RedSea block `0x58` byte `0x18` seems to always point to the block starting the root directory.

### Directories

Directories are split into 64 byte blocks indicating different files and subdirectories contained within the directory. The first two entries are always the directory itself and it's parent directory `..`

Entries start with one of 3 possible 2 byte markers: `0x0820`, `0x0820`, and `0x0C20` which indicate whether the entry is a directory, a file, or a compressed file, respecitvely.

The marker is followed by a 38 bit section for a name. 

Bytes 40-47 represent the starting block of the file.

Bytes 48-55 represent the file size in bytes.

Bytes 56-63 represent the time the file was last modified using the CDate date specification.

### CDate

CDate is TempleOS's custom date format, based off of the birth of Christ.

CDates are given using a 64 bit integer, with the upper 32 bits `0xFFFF` representing the number of days since the birth of Christ on January 2nd, 1 BC, and the lower 32 bits `0xFFFF` representing the time of day divided by 4 billion, giving it the precision of 1/49710ths of a second.

As an example: the CDATE `0x000B4371D95FF3DD` would represent Januayr 7th, 2021 at 20:22:44, with `0x00B04371` representing the days since January 2nd, 1BC, and `0xD95FF3DD` representing the number of 1/49710ths of a second intervals since the start of the day.
# Credits

Terrence Andrew Davis for creating the RedSea filesystem and TempleOS.

