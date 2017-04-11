#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include "bootimg.h"

#define PATHLEN 256

boot_img_hdr *readHeader(boot_img_hdr *buffer, FILE *srcFile){
    rewind(srcFile);
    if(!fread(buffer, sizeof(boot_img_hdr), 1, srcFile)){
        if(feof(srcFile)){
            fprintf(stderr,
                "Error: end of file reached prematurely while reading header\n"
            );
        }else{
            fprintf(stderr, "Error: error encountered while reading header\n");
        }
        return NULL;
    }

    return buffer;
}

bool dd(
    FILE *srcFile, char *dest, size_t blockSize, size_t count, size_t skip
){
    bool error = false;
    FILE *destFile = NULL;
    void *buffer = NULL;

    if(!(destFile = fopen(dest, "w"))){
        fprintf(stderr, "Error: could not open \"%s\" for writing\n", dest);
        error = true;
    }

    if(!error){
        if(!(buffer = calloc(1, blockSize))){
            fprintf(
                stderr,
                "Error: failed to allocate memory of %lluB\n",
                (unsigned long long)blockSize
            );
            error = true;
        }
    }

    if(!error){
        fseek(srcFile, (long)(blockSize * skip), SEEK_SET);
        for(size_t i = 0; i < count; i++){
            if(!fread(buffer, blockSize, 1, srcFile)){
                fprintf(
                    stderr,
                    "Error: failed to read in a full block of size %lluB from "
                    "srcFile at offset %lluB\n",
                    (unsigned long long)blockSize,
                    (unsigned long long)((skip + i) * blockSize)
                );
                error = true;
                break;
            }
            if(!fwrite(buffer, blockSize, 1, destFile)){
                fprintf(
                    stderr,
                    "Error: failed to write a full block of size %lluB to "
                    "destFile at offset %lluB\n",
                    (unsigned long long)blockSize,
                    (unsigned long long)(i * blockSize)
                );
                error = true;
                break;
            }
        }
    }

    if(destFile) fclose(destFile);
    if(buffer) free(buffer);

    return !error;
}

bool extract(FILE *srcFile, char *dest, char what, boot_img_hdr *header){
    bool error = false;

    size_t
        pageSize = header->page_size,
        kernelPageOffset = 1,
        kernelPageCount = (header->kernel_size + pageSize - 1) / pageSize,
        ramdiskPageOffset = kernelPageOffset + kernelPageCount,
        ramdiskPageCount = (header->ramdisk_size + pageSize - 1) / pageSize,
        secondPageOffset = ramdiskPageOffset + ramdiskPageCount,
        secondPageCount = (header->second_size + pageSize - 1) / pageSize
    ;

    if(pageSize > 65536){
        fprintf(
            stderr,
            "Error: excessive page_size of %llu provided in header\n",
            (unsigned long long)header->page_size
        );
        error = true;
    }

    if(!error){
        switch(what){
            case 'k':
                error = !dd(srcFile, dest, pageSize, kernelPageCount, kernelPageOffset);
                break;
            case 'r':
                error = !dd(srcFile, dest, pageSize, ramdiskPageCount, ramdiskPageOffset);
                break;
            case 's':
                if(!header->second_size) break;
                error = !dd(srcFile, dest, pageSize, secondPageCount, secondPageOffset);
                break;
            default:
                fprintf(stderr, "Error: invalid 'what' value provided to extract\n");
                error = true;
        }
    }

    return !error;
}

int main(int argsLen, char **args){
    char
        src[PATHLEN] = "",
        destKernel[PATHLEN] = "kernel.img",
        destRamdisk[PATHLEN] = "ramdisk.img",
        destSecondary[PATHLEN] = "secondary.img"
    ;
    boot_img_hdr header;

    if(argsLen < 2){
        fprintf(stderr, "Error: src not specified\n");
        exit(EXIT_FAILURE);
    } else snprintf(src, PATHLEN, "%s", args[1]);

    FILE *srcFile = NULL;
    if(!(srcFile = fopen(src, "r"))){
        fprintf(stderr, "Error: could not open \"%s\"\n", src);
        exit(EXIT_FAILURE);
    }

    printf("Reading header...\n");
    readHeader(&header, srcFile);

    printf("Extracting %s...\n", destKernel);
    if(!extract(srcFile, destKernel, 'k', &header)) exit(EXIT_FAILURE);

    printf("Extracting %s...\n", destRamdisk);
    if(!extract(srcFile, destRamdisk, 'r', &header)) exit(EXIT_FAILURE);

    printf("Extracting %s...\n", destSecondary);
    if(!extract(srcFile, destSecondary, 's', &header)) exit(EXIT_FAILURE);

    return EXIT_SUCCESS;
}
