#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include "bootimg.h"

#define PATHLEN_MAX 256

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

int main(int argsLen, char **args){
    char src[PATHLEN_MAX] = "";
    char dests[][PATHLEN_MAX] = {
        "parameters.txt",
        "kernel.img",
        "ramdisk.img",
        "secondary.img",
        ""
    };
    char
        destKernel[PATHLEN_MAX] = "kernel.img",
        destRamdisk[PATHLEN_MAX] = "ramdisk.img",
        destSecondary[PATHLEN_MAX] = "secondary.img"
    ;
    boot_img_hdr header;
    uint32_t pageMap[4];

    if(argsLen < 2) throwError("src not specified");
    snprintf(src, PATHLEN_MAX, "%s", args[1]);

    FILE *srcFile = openFile(src, "r");

    printf("Reading header...\n");
    readHeader(&header, srcFile);
    printf("Page size: %uB\n", header.page_size);

    getPageMap(pageMap, &header);
    printPageMap(pageMap);

    for(size_t i = 0, offset = 0; dests[i][0] != '\0'; i++){
        FILE *destFile;
        if(pageMap[i] && i > 0){
            printf("Extracting \"%s\"...\n", dests[i]);
            destFile = openFile(dests[i], "w");
            writeSlice(
                srcFile, destFile,
                header.page_size, pageMap[i], offset
            );
            fclose(destFile);
        }
        offset += pageMap[i];
    }

    return EXIT_SUCCESS;
}
