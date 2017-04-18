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

// Destination filenames for each slice of the image
// Slice 0 is the header, and doesn't get output directly:
// A dump of parameters to mkbootimg is instead produced.
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

void getSliceMap(uint32_t sliceMap[4], const boot_img_hdr *header){
    uint32_t pageSize = header->page_size, sizeInBytes;

    for(uint32_t i = 0; i < 4; i++){
        switch(i){
            default: sizeInBytes = pageSize; break;
            case 1: sizeInBytes = header->kernel_size; break;
            case 2: sizeInBytes = header->ramdisk_size; break;
            case 3: sizeInBytes = header->second_size; break;
        }
        sliceMap[i] = (sizeInBytes + pageSize - 1) / pageSize;
    }
}

void writeSliceMap(FILE *destFile, uint32_t sliceMap[4]){
    for(char i = 0; i < 4; i++){
        char *label;
        switch(i){
            case 0: label = "header"; break;
            case 1: label = "kernel"; break;
            case 2: label = "ramdisk"; break;
            case 3: label = "second"; break;
            default: "";
        }
        fprintf(
            destFile,
            "Size of \"%s\" slice: %u page%s\n",
            label, sliceMap[i], sliceMap[i] == 1 ? "" : "s"
        );
    }
}

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

void writeSlice(
    FILE *srcFile, FILE *destFile,
    size_t pageSize, size_t pageCount, size_t offset
){
    void *buffer = NULL;
    if(!(buffer = calloc(1, pageSize)))
        throwError("Failed to allocate buffer. %s", strerror(errno));

    fseek(srcFile, (long)(pageSize * offset), SEEK_SET);
    for(size_t i = 0; i < pageCount; i++){
        if(!fread(buffer, pageSize, 1, srcFile))
            throwError(
                "Failed to read an entire page at offset %lluB. %s",
                (unsigned long long)((offset + i) * pageSize),
                strerror(errno)
            );

        if(!fwrite(buffer, pageSize, 1, destFile))
            throwError(
                "Failed to write an entire page at offset %lluB. %s",
                (unsigned long long)(i * pageSize),
                strerror(errno)
            );
    }

    free(buffer);
}

void writeMakeScript(
    FILE *destFile, char **dests,
    const char *mkbootimgCmd, const boot_img_hdr *header
){
    char osVersion[12] = "", osPatchLevel[12] = "";

    fprintf(destFile, "#!/bin/sh\n");
    fprintf(destFile, "%s \\\n", mkbootimgCmd);
    fprintf(destFile, " --kernel \"%s\" \\\n", dests[1]);
    fprintf(destFile, " --ramdisk \"%s\" \\\n", dests[2]);
    if(header->second_size)
        fprintf(destFile, " --second \"%s\" \\\n", dests[3]);
    fprintf(
        destFile, " --cmdline \"%s%s\" \\\n",
        header->cmdline, header->extra_cmdline
    );
    fprintf(destFile, " --base %#x \\\n", 0);
    fprintf(destFile, " --kernel_offset %#x \\\n", header->kernel_addr);
    fprintf(destFile, " --ramdisk_offset %#x \\\n", header->ramdisk_addr);
    fprintf(destFile, " --second_offset %#x \\\n", header->second_addr);
    getOsVersion(osVersion, osPatchLevel, header);
    fprintf(destFile, " --os_version \"%s\" \\\n", osVersion);
    fprintf(destFile, " --os_patch_level \"%s\" \\\n", osPatchLevel);
    fprintf(destFile, " --tags_offset %#x \\\n", header->tags_addr);
    fprintf(destFile, " --board \"%s\" \\\n", header->name);
    fprintf(destFile, " --pagesize %#x \\\n", header->page_size);
    fprintf(destFile, " --output \"%s\"\n", dests[DEST_NEWBOOT]);
}

void usage(char **args){
    printf(
        "Usage: %s -s <src> [OPTIONS]\n\n"
        "Extracts the kernel, ramdisk, and second-stage bootloader from the\n"
        "provided Android boot image, and outputs them to the same directory.\n"
        "Furthermore, this also creates a remake script that recombines these\n"
        "extracted images into newboot.img, by running mkbootimg with the\n"
        "parameters extracted from the original image header of src.\n\n"
        "OPTIONS:\n"
        "\t-s <src>: The source Android boot image file to extract from.\n"
        "\t-d <destDir>: Output extracted images here instead.\n"
        "\t-v: Verbose.\n"
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
    char *dests[] = { //see destsIndex
        "remkbootimg.sh",
        "kernel.img",
        "ramdisk.img",
        "secondary.img",
        "newboot.img"
    };
    FILE *srcFile = NULL, *destFile = NULL;
    boot_img_hdr header;
    uint32_t sliceMap[4];
    bool verbose = false;
    char *mkbootimgCmd = "mkbootimg";

    // Parse supplied arguments
    int opt = 0;
    while((opt = getopt(argsLen, args, "s:d:vr:m:n:")) >= 0) switch(opt){
        case 's': src = optarg; break;
        case 'd': destDir = optarg; break;
        case 'v': verbose = true; break;
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

    // Read in header and calculate sliceMap
    if(verbose) printf("Reading header...\n");
    readHeader(&header, srcFile);
    if(verbose) printf("Page size: %uB\n", header.page_size);
    getSliceMap(sliceMap, &header);
    if(verbose) writeSliceMap(stdout, sliceMap);

    // Extract slices based on sliceMap, and dump them to to their
    // respective dests.
    for(size_t dest = 0, offset = 0; dest <= DEST_SECOND; dest++){
        if(verbose) printf("Writing \"%s\"\n", dests[dest]);
        destFile = openFile(dests[dest], "w");
        if(dest == DEST_MKSCRIPT)
            writeMakeScript(destFile, dests, mkbootimgCmd, &header);
        else writeSlice(
            srcFile, destFile,
            header.page_size, sliceMap[dest], offset
        );
        fclose(destFile);
        offset += sliceMap[dest];
    }

    // Cleanup
    fclose(srcFile);
    if(destDirMalloced) free(destDir);

    return EXIT_SUCCESS;
}
