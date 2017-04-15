#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include "bootimg.h"

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

void readHeader(boot_img_hdr *header, FILE *srcFile){
    errno = 0;

    rewind(srcFile);
    if(errno) throwError("Failed to rewind to start. %s", strerror(errno));

    fread(header, sizeof(boot_img_hdr), 1, srcFile);
    if(errno) throwError("Failed to read header. %s", strerror(errno));
}

void getPageMap(uint32_t pageMap[4], const boot_img_hdr *header){
    uint32_t pageSize = header->page_size, sizeInBytes;

    for(uint32_t i = 0; i < 4; i++){
        switch(i){
            case 1:
                sizeInBytes = header->kernel_size;
                if(!sizeInBytes) throwError("invalid kernel_size");
                break;
            case 2:
                sizeInBytes = header->ramdisk_size;
                if(!sizeInBytes) throwError("invalid ramdisk_size");
                break;
            case 3:
                sizeInBytes = header->second_size;
                break;
            default:
                sizeInBytes = pageSize; break;
        }
        pageMap[i] = (sizeInBytes + pageSize - 1) / pageSize;
    }
}

void printPageMap(uint32_t pageMap[4]){
    for(char i = 0; i < 4; i++){
        char *label;
        switch(i){
            case 0: label = "header"; break;
            case 1: label = "kernel"; break;
            case 2: label = "ramdisk"; break;
            case 3: label = "second"; break;
            default: "";
        }
        printf(
            "Size of \"%s\" slice: %u page%s\n",
            label, pageMap[i], pageMap[i] == 1 ? "" : "s"
        );
    }
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

void writeParameters(FILE *srcFile, FILE *destFile, boot_img_hdr *header){

}

int main(int argsLen, char **args){
    char *src;
    char *srcDir; size_t srcDirLen = 0;
    char *dests[] = {
        "parameters.txt",
        "kernel.img",
        "ramdisk.img",
        "secondary.img",
        ""
    };
    FILE *srcFile = NULL, *destFile = NULL;
    boot_img_hdr header;
    uint32_t pageMap[4];

    // Open srcFile
    if(argsLen < 2) throwError("src not specified");
    src = args[1];
    srcFile = openFile(src, "r");

    // Extract src's parent directory pathname, then chdir into it
    errno = 0;
    srcDirLen = (size_t)rindex(src, '/');
    srcDirLen = (srcDirLen ? srcDirLen + 1 - (size_t)src : 0) + 1;
    srcDir = malloc(srcDirLen);
    if(errno) throwError("%s", strerror(errno));
    if(srcDirLen > 1) memcpy(srcDir, src, srcDirLen - 1);
    srcDir[srcDirLen - 1] = '\0';
    chdir(srcDir);
    if(errno) throwError(
        "Failed to chdir into \"%s\". %s",
        srcDir, strerror(errno)
    );

    // Read in header and calculate pageMap
    printf("Reading header...\n");
    readHeader(&header, srcFile);
    printf("Page size: %uB\n", header.page_size);
    getPageMap(pageMap, &header);
    printPageMap(pageMap);

    // Extract slices based on pageMap, and dump them to to their
    // respective dests.
    for(size_t i = 0, offset = 0; dests[i][0] != '\0'; i++){
        printf("Extracting \"%s\"...\n", dests[i]);
        destFile = openFile(dests[i], "w");
        if(i == 0){}
        else writeSlice(
            srcFile, destFile,
            header.page_size, pageMap[i], offset
        );
        fclose(destFile);
        offset += pageMap[i];
    }

    // Cleanup
    fclose(srcFile);
    free(srcDir);

    return EXIT_SUCCESS;
}
