#include <stdio.h>
#include <stdint.h>
#include "fs.h"

#define DISKSIM_SIZE 1000000

uint8_t disksim[DISKSIM_SIZE];

int main(){
    FS_formatDrive(disksim, DISKSIM_SIZE);
    if((uint64_t)((uint64_t*)disksim)[0] == 0xFEDCBA9876543210)  printf("Check Worked\n");
    
    FS_mountDrive(disksim);

    uint64_t count;
    FS_getItemCount("/", &count);
    printf("%ld\n", count);
    return 0;
}
