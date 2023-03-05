
# RedSea FUSE Drivers

FUSE drivers to support the RedSea filesystem. Supports full R/W access to ISO.C files.



## Features

- Supports full read and write to ISO.C files
- Does not support writing to raw RedSea filesystem images, nor does it support creating new ISO.C images. Hopefully these will be supported soon.


## Documentation

To use this program, you should have FUSE installed on your system. You should then be able to run the fuse drivers with `./redsea [RedSea.ISO.C] [directory]`

where `RedSea.ISO.C` is any RedSea ISO.C file and `directory` is the directory you wish to view the filesystem in.

THIS IS A VERY EARLY RELEASE. THIS MAY SOMEHOW BREAK YOUR ISO.C FILES. So please create a backup of any ISO.C files you wish to use with this program, especially if you plan on writing to the disk.

This is not a completely faithful implementation of the RedSea filesystem. Any ISO.C files modified with this porgram should work with TempleOS, but they might not. Please report any inconsistencies to me.

## RedSea Documentation

Some documenation of what I know about the RedSea filesystem

### Blocks

RedSea operates with 512 byte Blocks

### Headers

RedSea ISO.C file headers follow the ISO9660 format w/ El Torito extension for the first couple of blocks.

In RedSea block `0x58` byte `0x18` seems to always point to the block starting the root directory.

### Directories

Directories are split into 64 byte blocks indicating different files and subdirectories contained within the directory. The first two entries are always the directory itself and its parent directory `..`

Entries start with a 2 byte file attribute entry with the following possible attributes:


`0x01` - Read Only

`0x02` - Hidden

`0x04` - System

`0x08` - Volume ID

`0x10` - Directory

`0x20` - Archive (file?)

`0x100` - Deleted

`0x200` - Resident

`0x400` - Compressed

`0x800` - Contiguous (seems to be true even without this attribute though? all RedSea files are contiguous)

`0x1000` - Fixed


Note that some of these attributes, such as "Read Only" are unsupported by this driver.

There are also attributes for long names. These are not supported by this driver at the time. 

The marker is followed by a 38 bit section for a name. 

Bytes 40-47 represent the starting block of the file.

Bytes 48-55 represent the file size in bytes.

Bytes 56-63 represent the time the file was last modified using the CDate date specification.

### CDate

CDate is TempleOS's custom date format, based off of the birth of Christ.

CDates are given using a 64 bit integer, with the upper 32 bits `0xFFFFFFFF` representing the number of days since the birth of Christ 719527 days before the unix epoch start and the lower 32 bits `0xFFFFFFFF` representing the time of day divided by 4 billion, giving it the precision of 1/49710ths of a second.

As an example: the CDATE `0x000B4371D95FF3DD` would represent January 7th, 2021 at 20:22:44, with `0x00B04371` representing the days since the birth of christ and `0xD95FF3DD` representing the number of 1/49710ths of a second intervals since the start of the day.

# Credits

Terrence Andrew Davis for creating the RedSea filesystem and TempleOS.

