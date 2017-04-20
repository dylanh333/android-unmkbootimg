/**

MIT License

Copyright (c) 2017 Dylan Hicks (aka. dylanh333)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

**/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <linux/limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <getopt.h>
#include "bootimg.h"

#define BOOT_ID_SIZE (sizeof(uint32_t) * 8)

enum sliceIndex {
    SLICE_HEADER,
    SLICE_KERNEL,
    SLICE_RAMDISK,
    SLICE_SECOND
};

enum destsIndex {
    DEST_MKSCRIPT,
    DEST_KERNEL,
    DEST_RAMDISK,
    DEST_SECOND,
    DEST_NEWBOOT
};

#define throwError(message, ...) {\
    fprintf(stderr, "Error in %s(): " message "\n", __func__, ##__VA_ARGS__);\
    exit(EXIT_FAILURE);\
}

#define throwWarning(message, ...) \
    fprintf(stderr, "Warning in %s(): " message "\n", __func__, ##__VA_ARGS__);

FILE *openFile(const char *path, const char *mode){
    FILE *file = NULL; errno = 0;
    file = fopen(path, mode);
    if(errno) throwError(
        "Failed to open \"%s\" in \"%s\" mode. %s",
        path, mode, strerror(errno)
    );
    return file;
}

void changeDir(const char *dir){
    errno = 0;
    chdir(dir);
    if(errno == ENOENT){
        errno = 0;
        mkdir(dir, 0777);
        if(errno) throwError(
            "Failed to create directory \"%s\". %s",
            dir, strerror(errno)
        );
        chdir(dir);
    }
    if(errno) throwError(
        "Failed to open directory \"%s\". %s",
        dir, strerror(errno)
    );
}

size_t getEnclosingDir(char *dir, size_t dirSize, const char *file){
    size_t dirLen;
    dirLen = (size_t)rindex(file, '/');
    dirLen = (dirLen ? dirLen + 1 - (size_t)file : 0) + 1;
    if(dirLen > dirSize) return 0;
    memcpy(dir, file, dirLen - 1);
    dir[dirLen - 1] = '\0';
    return dirLen;
}

void readHeader(boot_img_hdr *header, FILE *srcFile){
    errno = 0;

    // Read header info supplied buffer
    rewind(srcFile);
    if(errno) throwError("Failed to rewind to start. %s", strerror(errno));
    fread(header, sizeof(boot_img_hdr), 1, srcFile);
    if(errno) throwError("Failed to read header. %s", strerror(errno));

    // Validate critical header values
    if(strncmp(header->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) != 0)
        throwError("Invalid magic number at start of header");
    if(header->kernel_size == 0) throwError("Invalid kernel_size");
    if(header->ramdisk_size == 0) throwError("Invalid ramdisk_size");
}


/**
 * Gets the actual size of all slices in the bootimg, without rounding up to
 * the nearest page.
 * Eg. sizeMap[SLICE_KERNEL] gives the size of the kernel slice
 */
void getSizeMap(uint32_t sizeMap[4], const boot_img_hdr *header){
    for(int slice = 0; slice < 4; slice++){
        uint32_t size;

        switch(slice){
            case SLICE_HEADER: size = sizeof(boot_img_hdr); break;
            case SLICE_KERNEL: size = header->kernel_size; break;
            case SLICE_RAMDISK: size = header->ramdisk_size; break;
            case SLICE_SECOND: size = header->second_size; break;
            default: size = 0;
        }

        sizeMap[slice] = size;
    }
}

/**
 * Gets the offsets of all slices in the bootimg, with each slice page-aligned.
 * Eg. offsetMap[SLICE_KERNEL] gives the offset of the kernel in the
 * bootimg.
 * offsetMap[SLICE_SECOND+1] gives the end of SLICE_SECOND
 */
void getOffsetMap(uint32_t offsetMap[4], const boot_img_hdr *header){
    uint32_t pageSize = header->page_size;
    uint32_t sizeMap[4];

    getSizeMap(sizeMap, header);

    offsetMap[0] = 0;
    for(int slice = 0; slice < 3; slice++){
        uint32_t currSliceSize = sizeMap[slice];

        // Slice size in bytes is the kernel/ramdisk/second size, but rounded
        // up to the nearest page.
        currSliceSize = ((currSliceSize + pageSize - 1) / pageSize) * pageSize;

        offsetMap[slice+1] = offsetMap[slice] + currSliceSize;
    }
}

#define OS_VERSION_SIZE 12
void getOsVersion(
    char version[12], char patchLevel[12], const boot_img_hdr *header
){
    uint8_t a = 0, b = 0, c = 0;
    uint8_t y = 0, m = 0, d = 1;

    // Raw bit layout: aaaaaaabbbbbbbcccccccyyyyyyymmmm
    uint32_t rawVersion = header->os_version >> 11;
    uint16_t rawPatchLevel = header->os_version & 2047;

    // Extract version
    a = (rawVersion >> 14) & 127;
    b = (rawVersion >> 7) & 127;
    c = rawVersion & 127;

    // Extract patchLevel
    y = (rawPatchLevel >> 4) & 127;
    m = rawPatchLevel & 15;

    // Output human readable version information
    snprintf(version, 12, "%u.%u.%u", a, b, c);
    snprintf(patchLevel, 11, "%04u-%02u-%02u", 2000 + y, m, d);
}

#define IMAGE_ID_SIZE (BOOT_ID_SIZE * 3 + 1)
void getImageId(
	char imageId[IMAGE_ID_SIZE],
	const boot_img_hdr *header,
	bool noSeparator
){
	// We want to deal with header->id one byte at a time, not one uint32_t
    uint8_t *id = (void*)header->id;
	bool isSha1 = true;
	char *separator;
	size_t i; //offset in id
	int j; //offset in imageId string

	// If the last three 32b uints of header->id are zero, then lets assume
	// that the first 5 uints (160 bits) aren't, and that this is an sha1
	// digest
	for(i = 20; i < BOOT_ID_SIZE; i++) if(id[i] > 0) isSha1 = false;

    for(i = 0, j = 0; i < BOOT_ID_SIZE && j < IMAGE_ID_SIZE - 1; i++){
		if(isSha1){
			if(i >= 20){
				j += snprintf(
					imageId + j, IMAGE_ID_SIZE - j,
					" (sha1)"
				);
				break;
			} else separator = "";
		}else{
			if(i == BOOT_ID_SIZE - 1) separator = "";
			else if((i + 1) % 4 == 0) separator = " ";
			else separator = ":";
		}
	
        j += snprintf(
			imageId + j, IMAGE_ID_SIZE - j,
			"%02x%s",
			id[i], separator
		);	
    }
}

void writeHeaderInfo(FILE *destFile, boot_img_hdr *header){
	char osVersion[OS_VERSION_SIZE], osPatchLevel[OS_VERSION_SIZE];
	char imageId[IMAGE_ID_SIZE];

	fprintf(destFile, "Kernel size: %uB\n", header->kernel_size);
	fprintf(destFile, "Kernel load address: %#x\n", header->kernel_addr);

	fprintf(destFile, "Ramdisk size: %uB\n", header->ramdisk_size);
	fprintf(destFile, "Ramdisk load address: %#x\n", header->ramdisk_addr);

	fprintf(destFile, "Second size: %uB\n", header->second_size);
	fprintf(destFile, "Second load address: %#x\n", header->second_addr);

	fprintf(destFile, "Tags address: %#x\n", header->tags_addr);
	fprintf(destFile, "Page size: %uB\n", header->page_size);

	getOsVersion(osVersion, osPatchLevel, header);
	fprintf(destFile, 
		"Android version: %s\nAndroid patch Level: %s\n",
		osVersion, osPatchLevel
	);

	fprintf(destFile, "Product name: \"%.*s\"\n", BOOT_NAME_SIZE, header->name);
	
	fprintf(destFile, 
		"Command line (including extra): \"%.*s%.*s\"\n",
		BOOT_ARGS_SIZE, header->cmdline,
		BOOT_EXTRA_ARGS_SIZE, header->extra_cmdline
	);

	getImageId(imageId, header, false);
	fprintf(destFile, "Image ID (eg. checksum): %s\n", imageId);
}

void writeMakeScript(
    FILE *destFile, char **dests,
    const char *mkbootimgCmd, const boot_img_hdr *header
){
    char osVersion[12] = "", osPatchLevel[12] = "";

    // Calculate version information
    getOsVersion(osVersion, osPatchLevel, header);

    // Write the script
    fprintf(destFile, "#!/bin/sh\n");
    fprintf(destFile, "%s \\\n", mkbootimgCmd);
    fprintf(destFile, " --kernel \"%s\" \\\n", dests[1]);
    fprintf(destFile, " --ramdisk \"%s\" \\\n", dests[2]);
    if(header->second_size)
        fprintf(destFile, " --second \"%s\" \\\n", dests[3]);
    fprintf(
        destFile, " --cmdline \"%.*s%.*s\"\\\n",
		BOOT_ARGS_SIZE, header->cmdline,
		BOOT_EXTRA_ARGS_SIZE, header->extra_cmdline
	);
    fprintf(destFile, " --base %#x \\\n", 0);
    fprintf(destFile, " --kernel_offset %#x \\\n", header->kernel_addr);
    fprintf(destFile, " --ramdisk_offset %#x \\\n", header->ramdisk_addr);
    fprintf(destFile, " --second_offset %#x \\\n", header->second_addr);
    fprintf(destFile, " --os_version \"%s\" \\\n", osVersion);
    fprintf(destFile, " --os_patch_level \"%s\" \\\n", osPatchLevel);
    fprintf(destFile, " --tags_offset %#x \\\n", header->tags_addr);
    fprintf(destFile, " --board \"%.*s\" \\\n", BOOT_NAME_SIZE, header->name);
    fprintf(destFile, " --pagesize %#x \\\n", header->page_size);
    fprintf(destFile, " --output \"%s\"\n", dests[DEST_NEWBOOT]);

    // Make sure the script is executable
    errno = 0;
    fchmod(fileno(destFile), 0750);
    if(errno) throwWarning(
        "Failed to change file mode to 0750. %s",
        strerror(errno)
    );
}

void writeSlice(
    FILE *srcFile, FILE *destFile,
    size_t blockSize, size_t byteOffset, size_t byteCount
){
    int err = 0;

    // Allocate a blockSized buffer to reduce fread operations and thus system
    // call overhead
    void *buffer = NULL;
    if(!(buffer = calloc(1, blockSize))) throwError(
        "Failed to allocate buffer of blockSize %luB. %s",
        blockSize, strerror(errno)
    );

    // Ensure that file is at the right position
    fseek(srcFile, byteOffset, SEEK_SET);
    rewind(destFile);

    // Begin extracting slice, one blockSize at a time
    for(size_t i = 0; i < byteCount / blockSize + 1; i++){
        size_t quota;

        if((quota = byteCount - i * blockSize) < blockSize);
        else quota = blockSize;
        if(quota == 0) break;

        if(fread(buffer, quota, 1, srcFile) < 1){
            err = errno;
            if(feof(srcFile)){
                throwError(
                    "Unexpected end of input. Current offset: %luB. %s",
                    byteCount + i * blockSize, strerror(err)
                );
            }else{
                throwError(
                    "Failed to read file. Current offset: %luB. %s",
                    byteCount + i * blockSize, strerror(err)
                );
            }
        }

        if(fwrite(buffer, quota, 1, destFile) < 1){
            throwError("Failed to write to file. %s", strerror(errno));
        }
    }

    free(buffer);
}

void usage(char **args){
    printf(
        "Usage: %s [OPTIONS] <src>\n\n"
        "Extracts the kernel, ramdisk, and second-stage bootloader from the\n"
        "provided Android boot image, and outputs them to the same directory.\n"
        "Furthermore, this also creates a remake script that recombines these\n"
        "extracted images into newboot.img, by running mkbootimg with the\n"
        "parameters extracted from the original image header of src.\n\n"
        "OPTIONS:\n"
        "\t<src>: The source Android boot image file to extract from.\n"
        "\t-d <destDir>: Output extracted images here instead.\n"
        "\t-v: Verbose.\n"
		"\t-i: Print header information only, then exit.\n"
        "\t-r <remakeScript>: Save the remake script using this filename\n"
        "\t\tinstead.\n"
        "\t-m <mkbootimgCmd>: Use this command in the remake script for\n"
        "\t\tmkbootimg instead.\n"
        "\t-n <newBootImgName>: Direct the remake script to output the\n"
        "\t\tremade boot image using this filename instead, rather than\n"
        "\t\tnewboot.img.\n",
        args[0]
    );
}

int main(int argsLen, char **args){
    char *src = NULL;
    char *destDir = NULL; bool destDirMalloced = false;
    char *dests[] = { //see destIndex
        "remkbootimg.sh",
        "kernel.img",
        "ramdisk.img",
        "secondary.img",
        "newboot.img"
    };
    FILE *srcFile = NULL, *destFile = NULL;
    boot_img_hdr header;
    uint32_t offsetMap[4], sizeMap[4];
    bool verbose = false, onlyPrintHeader = false;
    char *mkbootimgCmd = "mkbootimg";

    // Parse supplied arguments
    int opt = 0;
    while((opt = getopt(argsLen, args, "-s:d:vir:m:n:")) >= 0) switch(opt){
		case 1: src = optarg; break;
        case 'd': destDir = optarg; break;
        case 'v': verbose = true; onlyPrintHeader = false; break;
		case 'i': onlyPrintHeader = true; verbose = false; break;
        case 'r': dests[DEST_MKSCRIPT] = optarg; break;
        case 'm': mkbootimgCmd = optarg; break;
        case 'n': dests[DEST_NEWBOOT] = optarg; break;
        default:
            usage(args);
            return EXIT_FAILURE;
    };

    // Open srcFile
    if(src == NULL){
        usage(args);
        return EXIT_FAILURE;
    }
    srcFile = openFile(src, "r");

    // Extract src's parent directory pathname, then chdir into it
    if(destDir == NULL){
        destDir = malloc(PATH_MAX);
        destDirMalloced = true;
        getEnclosingDir(destDir, PATH_MAX, src);
    }
    changeDir(destDir);

    // Read in header information
    if(verbose) printf("Reading header...\n");
    readHeader(&header, srcFile);
    getSizeMap(sizeMap, &header);
    getOffsetMap(offsetMap, &header);
    if(verbose) printf("---\n");
	if(verbose || onlyPrintHeader) writeHeaderInfo(stdout, &header);
	if(verbose) printf("---\n\n");

    // Extract slices based on offsetMap, and dump them to to their
    // respective dests.
    if(!onlyPrintHeader) for(size_t dest = 0; dest <= SLICE_SECOND; dest++){
        if(sizeMap[dest] == 0) continue;
        if(verbose) printf("Writing \"%s\"...\n", dests[dest]);
        destFile = openFile(dests[dest], "w");

        switch(dest){
            case DEST_MKSCRIPT:
                writeMakeScript(destFile, dests, mkbootimgCmd, &header);
                break;
            default:
                writeSlice(
                    srcFile, destFile,
                    header.page_size, offsetMap[dest], sizeMap[dest]
                );
        }

        fclose(destFile);
    };

    // Cleanup
    fclose(srcFile);
    if(destDirMalloced) free(destDir);

    return EXIT_SUCCESS;
}
