#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "fs.h"
#include "rfs.h"

#define DISKSIM_SIZE 1000000

uint8_t disksim[DISKSIM_SIZE];

int main(){
    FS_formatDrive(disksim, DISKSIM_SIZE);
    if((uint64_t)((uint64_t*)disksim)[0] == 0xFEDCBA9876543210)  printf("Check Worked\n");

    FS_mountDrive(disksim);

    RFS_createFolder("docs");
    RFS_changeDir("docs");
    RFS_createFolder("sub");
    RFS_changeDir("sub");

    TRFS_FILE* f;
    if(RFS_openFile("../hallo.txt", RFS_MODE_CREATE, &f) == FSE_SUCCESS){
        RFS_writeFile(f, 0, "Hallo RFS!", 10);
        RFS_closeFile(f);
    }

    char* wd;
    RFS_getWorkingDir(&wd);
    printf("Arbeitsverzeichnis: %s\n", wd);
    free(wd);

    RFS_changeDir("..");
    if(RFS_openFile("hallo.txt", RFS_MODE_READ, &f) == FSE_SUCCESS){
        char buf[32] = {0};
        uint64_t n;
        RFS_readFile(f, 0, buf, sizeof(buf) - 1, &n);
        printf("hallo.txt (%lu Bytes): %s\n", (unsigned long)n, buf);
        RFS_closeFile(f);
    }

    uint64_t count;
    RFS_getItemCount(".", &count);
    printf("Eintraege in docs: %lu\n", (unsigned long)count);
    for(uint64_t i = 0; i < count; i++){
        char* name;
        bool isFolder;
        RFS_nameByID(".", i, &name);
        RFS_isItemFolderByID(".", i, &isFolder);
        printf("  %s%s\n", name, isFolder ? "/" : "");
        free(name);
    }
    return 0;
}
