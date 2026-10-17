#include "fs.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_PATTERN 0xFEDCBA9876543210
#define MIN_DISK_SIZE 1024
#define MAX_FOLDER_DEPTH 32
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static void* drivePTR; 
static void* rootBlockPTR;

typedef struct __attribute__((__packed__)) SHeaderDisk {
    uint64_t CHECK;
    uint64_t diskSize;
    uint64_t startRootBlockAddr;
    uint64_t blockCount;
    uint64_t freeSize;
} THeaderDisk;
#define THeaderDisk_SIZE sizeof(THeaderDisk)
#define MIN_FREE_BLOCK_SIZE 8
#define MIN_FREE_FULL_BLOCK_SIZE (THeaderBlock_SIZE + MIN_FREE_BLOCK_SIZE)

typedef struct __attribute__((__packed__)) SHeaderBlock {
    uint64_t blockSize;
    uint64_t nextBlockAddr;
    uint64_t nextRecordBlockAddr;
    uint8_t deadSize;
    bool free;
    uint8_t data[];
} THeaderBlock;
#define THeaderBlock_SIZE sizeof(THeaderBlock)

typedef struct __attribute__((__packed__)) SFolderRecord {
    uint64_t startBlockAddr;
    uint64_t sizeBytes;
    uint64_t blockCount;
    bool folder;
    bool ownRootFolder;
} TFolderRecord;
#define TFolderRecord_SIZE sizeof(TFolderRecord)

typedef struct __attribute__((__packed__)) SFolder {
    uint64_t sizeBytes;
    uint64_t blockCount;
    uint64_t entryCount;
    TFolderRecord records[];
} TFolder;
#define TFolder_SIZE sizeof(TFolder)

static uint64_t processPath(char* rawPath, char** folders);
static uint64_t nameSize(void* blockHeaderPTR);
static uint64_t blockDataSize(const THeaderBlock* block);
static bool validFolderEntryCount(const TFolder* folder);
static TFS_ERROR writeName(void* blockHeaderPTR, char* name);
static bool cmpName(void* blockHeaderPTR, char* name);
static TFS_ERROR copyBlocked(void* dest, uint64_t vOffsetSrc, uint64_t size, void* blockHeaderPTR);
static TFS_ERROR insertBlocked(uint64_t vOffsetDest, void* src, uint64_t size, void* blockHeaderPTR);
static void* vOffsetToPTR(uint64_t offset, void* blockHeaderPTR, void** foundBlockPTR);
static void* findFolderInFolder(void* curFolderBlockPTR, char* folder);
static TFS_ERROR updateParentFolderRecord(void* folderBlockPTR);
static uint64_t countRecordBlocks(void* blockHeaderPTR);
static TFS_ERROR gotToFolder(char* rawPath, void** blockHeaderPTR);
static TFS_ERROR allocateBlocks(uint64_t size, void** firstBlockPTR, uint64_t* blockCount);
static TFS_ERROR appendAllocateBlocks(uint64_t size, void* blockPTR, uint64_t* addedBlockCount);
static TFS_ERROR validateFolderTree(void* folderBlockPTR, uint64_t expectedBlockCount, uint64_t expectedParentOffset, uint64_t depth);
static TFS_ERROR freeFolderTree(void* folderBlockPTR, uint64_t depth);
static void* splitFromBlock(void* blockPTR, uint64_t sizeNeeded);
static TFS_ERROR freeBlocks(void* firstBlockPTR);
static TFS_ERROR freeEnd(uint64_t size, void* firstBlockPTR);
static TFS_ERROR mergeFreeBlocks(void);

inline static void* offsetToPTR(uint64_t offset); 
inline static uint64_t PTRToOffset(void* ptr);

TFS_ERROR FS_formatDrive(uint8_t* disk, uint64_t size){
    if(disk == NULL) return FSE_INVALID_ARGUMENT;
    if(size < MIN_DISK_SIZE) return FSE_DISK_TOO_SMALL;

    drivePTR = disk;

    THeaderDisk diskhead;
    diskhead.CHECK = CHECK_PATTERN;
    diskhead.diskSize = size;
    diskhead.startRootBlockAddr = THeaderDisk_SIZE;
    diskhead.blockCount = 2;
    diskhead.freeSize = 0;
    memcpy(drivePTR, &diskhead, THeaderDisk_SIZE);

    uint64_t rootBlockOffset = THeaderDisk_SIZE;
    THeaderBlock rootBlock;
    rootBlock.blockSize = TFolder_SIZE + 1;
    rootBlock.nextBlockAddr = rootBlockOffset + THeaderBlock_SIZE + rootBlock.blockSize;
    rootBlock.nextRecordBlockAddr = 0;
    rootBlock.deadSize = 0;
    rootBlock.free = false;
    rootBlockPTR = offsetToPTR(rootBlockOffset);
    memcpy(rootBlockPTR, &rootBlock, THeaderBlock_SIZE);

    TFS_ERROR error = writeName(rootBlockPTR, "");
    if(error != FSE_SUCCESS) return error;

    TFolder root;
    root.sizeBytes = rootBlock.blockSize;
    root.blockCount = 1;
    root.entryCount = 0;
    error = insertBlocked(0, &root, TFolder_SIZE, rootBlockPTR);
    if(error != FSE_SUCCESS) return error;

    void* emptyBlockPTR = offsetToPTR(rootBlock.nextBlockAddr);
    THeaderBlock emptyBlock;
    emptyBlock.blockSize = size - rootBlock.nextBlockAddr - THeaderBlock_SIZE;
    emptyBlock.nextBlockAddr = 0;
    emptyBlock.nextRecordBlockAddr = 0;
    emptyBlock.deadSize = 0;
    emptyBlock.free = true;
    memcpy(emptyBlockPTR, &emptyBlock, THeaderBlock_SIZE);

    diskhead.freeSize = emptyBlock.blockSize;
    memcpy(drivePTR, &diskhead, THeaderDisk_SIZE);

    return FSE_SUCCESS;
}

TFS_ERROR FS_mountDrive(uint8_t* disk){
    if(disk == NULL) return FSE_INVALID_ARGUMENT;

    drivePTR = disk;
    void* headerPTR = (void*) disk;
    THeaderDisk header;
    memcpy(&header, headerPTR, THeaderDisk_SIZE);
    if(header.CHECK != CHECK_PATTERN) {drivePTR = NULL; return FSE_NOT_FORMATED; }
    if(header.diskSize < MIN_DISK_SIZE ||
       header.startRootBlockAddr != THeaderDisk_SIZE ||
       header.startRootBlockAddr > header.diskSize - THeaderBlock_SIZE ||
       header.blockCount < 2 ||
       header.freeSize > header.diskSize) {
        drivePTR = NULL;
        return FSE_BLOCKS_CORRUPTED;
    }
    
    uint64_t blockCount = 1;
    uint64_t freeSize = 0;
    uint64_t curBlockAddr = header.startRootBlockAddr;
    void* curBlockPTR = offsetToPTR(curBlockAddr);
    THeaderBlock curBlock; 
    memcpy(&curBlock, curBlockPTR, THeaderBlock_SIZE);
    if(curBlock.free) {
        drivePTR = NULL;
        return FSE_BLOCKS_CORRUPTED;
    }

    while(true){
        if(curBlockAddr > header.diskSize - THeaderBlock_SIZE ||
           curBlock.blockSize > header.diskSize - curBlockAddr - THeaderBlock_SIZE ||
           curBlock.deadSize > curBlock.blockSize ||
           (curBlock.free && curBlock.deadSize != 0)) {
            drivePTR = NULL;
            return FSE_BLOCKS_CORRUPTED;
        }
        if(curBlock.free){
            if(freeSize > UINT64_MAX - curBlock.blockSize) {
                drivePTR = NULL;
                return FSE_BLOCKS_CORRUPTED;
            }
            freeSize += curBlock.blockSize;
        }
        if(curBlock.nextBlockAddr == 0) break;
        if(blockCount >= header.blockCount ||
           curBlock.nextBlockAddr != curBlockAddr + THeaderBlock_SIZE + curBlock.blockSize ||
           curBlock.nextBlockAddr > header.diskSize - THeaderBlock_SIZE) {
            drivePTR = NULL; return FSE_BLOCKS_CORRUPTED;
        }
        curBlockAddr = curBlock.nextBlockAddr;
        curBlockPTR = offsetToPTR(curBlockAddr);
        memcpy(&curBlock, curBlockPTR, THeaderBlock_SIZE);
        blockCount++;
    }
    uint64_t sizeCheck = curBlockAddr + THeaderBlock_SIZE + curBlock.blockSize;

    if(sizeCheck < header.diskSize) { drivePTR = NULL; return FSE_TO_FEW_ALLOCATED; }
    else if(sizeCheck > header.diskSize) { drivePTR = NULL; return FSE_TO_MUCH_ALLOCATED; }
    
    if(blockCount != header.blockCount || freeSize != header.freeSize) {
        drivePTR = NULL;
        return FSE_BLOCKS_CORRUPTED;
    }

    drivePTR = disk;
    rootBlockPTR = offsetToPTR(header.startRootBlockAddr);
    
    return FSE_SUCCESS;
}

TFS_ERROR FS_getItemCount(char* path, uint64_t* count){
    if(path == NULL || count == NULL) return FSE_INVALID_ARGUMENT;
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;

    void* blockHeaderPTR;
    TFS_ERROR error = gotToFolder(path, &blockHeaderPTR);
    if(error) return error;

    TFolder curFolder;
    error = copyBlocked(&curFolder, 0, TFolder_SIZE, blockHeaderPTR);
    if(error != FSE_SUCCESS) return error;
    if(!validFolderEntryCount(&curFolder)) return FSE_BLOCKS_CORRUPTED;
    *count = 0;
    for(uint64_t i = 0; i < curFolder.entryCount; i++){
        TFolderRecord record;
        error = copyBlocked(&record, TFolder_SIZE + i * TFolderRecord_SIZE,
                            TFolderRecord_SIZE, blockHeaderPTR);
        if(error != FSE_SUCCESS) return error;
        if(!record.ownRootFolder) (*count)++;
    }

    return FSE_SUCCESS;
}

TFS_ERROR FS_nameByID(char* path, uint64_t id, char** name){
    if(path == NULL || name == NULL) return FSE_INVALID_ARGUMENT;
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;

    *name = NULL;
    void* folderPTR;
    TFS_ERROR error = gotToFolder(path, &folderPTR);
    if(error) return error;

    TFolder curFolder;
    error = copyBlocked(&curFolder, 0, TFolder_SIZE, folderPTR);
    if(error != FSE_SUCCESS) return error;
    if(!validFolderEntryCount(&curFolder)) return FSE_BLOCKS_CORRUPTED;
    THeaderDisk diskHeader;
    memcpy(&diskHeader, drivePTR, THeaderDisk_SIZE);
    uint64_t itemID = 0;
    for(uint64_t i = 0; i < curFolder.entryCount; i++){
        TFolderRecord record;
        error = copyBlocked(&record, TFolder_SIZE + i * TFolderRecord_SIZE,
                            TFolderRecord_SIZE, folderPTR);
        if(error != FSE_SUCCESS) return error;
        if(record.ownRootFolder) continue;
        if(itemID++ != id) continue;

        if(record.startBlockAddr > diskHeader.diskSize - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;
        void* itemBlockPTR = offsetToPTR(record.startBlockAddr);
        uint64_t itemNameSize = nameSize(itemBlockPTR);
        if(itemNameSize == 0 || itemNameSize > record.sizeBytes)
            return FSE_BLOCKS_CORRUPTED;
        if(record.blockCount == 0 ||
           countRecordBlocks(itemBlockPTR) != record.blockCount)
            return FSE_BLOCKS_CORRUPTED;
        if(itemNameSize > SIZE_MAX) return FSE_MEMORY_ERROR;

        char* itemName = malloc(itemNameSize);
        if(itemName == NULL) return FSE_MEMORY_ERROR;

        uint64_t remaining = itemNameSize;
        char* dest = itemName;
        for(uint64_t copiedBlocks = 0;
            remaining && copiedBlocks < diskHeader.blockCount;
            copiedBlocks++){
            THeaderBlock itemBlock;
            memcpy(&itemBlock, itemBlockPTR, THeaderBlock_SIZE);
            uint64_t itemBlockOffset = PTRToOffset(itemBlockPTR);
            if(itemBlockOffset > diskHeader.diskSize - THeaderBlock_SIZE ||
               itemBlock.deadSize > itemBlock.blockSize || itemBlock.free ||
               itemBlock.blockSize > diskHeader.diskSize - itemBlockOffset - THeaderBlock_SIZE){
                free(itemName);
                return FSE_BLOCKS_CORRUPTED;
            }
            uint64_t chunk = MIN(blockDataSize(&itemBlock), remaining);
            memcpy(dest, (uint8_t*)itemBlockPTR + THeaderBlock_SIZE, chunk);
            dest += chunk;
            remaining -= chunk;
            if(remaining){
                if(itemBlock.nextRecordBlockAddr == 0){
                    free(itemName);
                    return FSE_BLOCKS_CORRUPTED;
                }
                if(itemBlock.nextRecordBlockAddr > diskHeader.diskSize - THeaderBlock_SIZE){
                    free(itemName);
                    return FSE_BLOCKS_CORRUPTED;
                }
                itemBlockPTR = offsetToPTR(itemBlock.nextRecordBlockAddr);
            }
        }
        if(remaining){
            free(itemName);
            return FSE_BLOCKS_CORRUPTED;
        }

        *name = itemName;
        return FSE_SUCCESS;
    }
    return FSE_INVAILD_ITEM_ID;
}

TFS_ERROR FS_isItemFolderByID(char* path, uint64_t id, bool* isFolder) {
    if(path == NULL || isFolder == NULL) return FSE_INVALID_ARGUMENT;
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;

    *isFolder = false;
    void* folderPTR;
    TFS_ERROR error = gotToFolder(path, &folderPTR);
    if(error != FSE_SUCCESS) return error;

    TFolder folder;
    error = copyBlocked(&folder, 0, TFolder_SIZE, folderPTR);
    if(error != FSE_SUCCESS) return error;
    if(!validFolderEntryCount(&folder)) return FSE_BLOCKS_CORRUPTED;

    uint64_t itemID = 0;
    for(uint64_t i = 0; i < folder.entryCount; i++){
        TFolderRecord record;
        error = copyBlocked(&record, TFolder_SIZE + i * TFolderRecord_SIZE,
                            TFolderRecord_SIZE, folderPTR);
        if(error != FSE_SUCCESS) return error;
        if(record.ownRootFolder) continue;
        if(itemID++ != id) continue;

        *isFolder = record.folder;
        return FSE_SUCCESS;
    }
    return FSE_INVAILD_ITEM_ID;
}

TFS_ERROR FS_createFolder(char* path, char* name) {
    if(path == NULL || name == NULL) return FSE_INVALID_ARGUMENT;
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;
    if(name[0] == '\0' || strchr(name, '/') != NULL) return FSE_INVALID_ARGUMENT;

    void* frontBlockHeaderPTR;
    TFS_ERROR error = gotToFolder(path, &frontBlockHeaderPTR);
    if(error) return error;

    TFolder frontFolder;
    error = copyBlocked(&frontFolder, 0, TFolder_SIZE, frontBlockHeaderPTR);
    if(error != FSE_SUCCESS) return error;
    if(!validFolderEntryCount(&frontFolder)) return FSE_BLOCKS_CORRUPTED;
    if(frontFolder.entryCount == UINT64_MAX ||
       frontFolder.sizeBytes > UINT64_MAX - TFolderRecord_SIZE)
        return FSE_BLOCKS_CORRUPTED;
    for(uint64_t i = 0; i < frontFolder.entryCount; i++){
        TFolderRecord existingRecord;
        error = copyBlocked(&existingRecord,
                            TFolder_SIZE + i * TFolderRecord_SIZE,
                            TFolderRecord_SIZE,
                            frontBlockHeaderPTR);
        if(error != FSE_SUCCESS) return error;
        if(!existingRecord.ownRootFolder && existingRecord.folder &&
           cmpName(offsetToPTR(existingRecord.startBlockAddr), name))
            return FSE_ALREADY_EXISTS;
    }

    uint64_t nameLength = strlen(name);
    if(nameLength > UINT64_MAX - 1 - TFolder_SIZE - TFolderRecord_SIZE)
        return FSE_NAME_TOO_LONG;
    uint64_t folderSize = nameLength + 1 + TFolder_SIZE + TFolderRecord_SIZE;
    uint64_t addedBlocks;
    error = appendAllocateBlocks(TFolderRecord_SIZE, frontBlockHeaderPTR, &addedBlocks);
    if(error != FSE_SUCCESS) return error;

    error = copyBlocked(&frontFolder, 0, TFolder_SIZE, frontBlockHeaderPTR);
    if(error != FSE_SUCCESS) return error;
    frontFolder.blockCount = countRecordBlocks(frontBlockHeaderPTR);
    if(frontFolder.blockCount == 0) return FSE_BLOCKS_CORRUPTED;
    error = insertBlocked(0, &frontFolder, TFolder_SIZE, frontBlockHeaderPTR);
    if(error != FSE_SUCCESS) return error;
    error = updateParentFolderRecord(frontBlockHeaderPTR);
    if(error != FSE_SUCCESS) return error;

    void* backBlockHeaderPTR;
    uint64_t blockCount;
    error = allocateBlocks(folderSize, &backBlockHeaderPTR, &blockCount);
    if(error != FSE_SUCCESS) return error;

    frontFolder.sizeBytes += TFolderRecord_SIZE;

    TFolderRecord frontRec;
    frontRec.startBlockAddr = PTRToOffset(backBlockHeaderPTR);
    frontRec.sizeBytes = folderSize;
    frontRec.blockCount = blockCount;
    frontRec.folder = true;
    frontRec.ownRootFolder = false;

    TFolder backFolder;
    backFolder.sizeBytes = folderSize;
    backFolder.blockCount = blockCount;
    backFolder.entryCount = 1;

    TFolderRecord backRec;
    backRec.startBlockAddr = PTRToOffset(frontBlockHeaderPTR);
    backRec.sizeBytes = frontFolder.sizeBytes;
    backRec.blockCount = frontFolder.blockCount;
    backRec.folder = true;
    backRec.ownRootFolder = true;

    error = writeName(backBlockHeaderPTR, name);
    if(error != FSE_SUCCESS){
        TFS_ERROR freeError = freeBlocks(backBlockHeaderPTR);
        return freeError == FSE_SUCCESS ? error : freeError;
    }

    error = insertBlocked(0, &backFolder, TFolder_SIZE, backBlockHeaderPTR);
    if(error == FSE_SUCCESS)
        error = insertBlocked(TFolder_SIZE, &backRec, TFolderRecord_SIZE, backBlockHeaderPTR);
    if(error != FSE_SUCCESS){
        TFS_ERROR freeError = freeBlocks(backBlockHeaderPTR);
        return freeError == FSE_SUCCESS ? error : freeError;
    }

    error = insertBlocked(TFolder_SIZE + (frontFolder.entryCount * TFolderRecord_SIZE),
                          &frontRec, TFolderRecord_SIZE, frontBlockHeaderPTR);
    if(error != FSE_SUCCESS){
        TFS_ERROR freeError = freeBlocks(backBlockHeaderPTR);
        return freeError == FSE_SUCCESS ? error : freeError;
    }

    frontFolder.entryCount++;
    error = insertBlocked(0, &frontFolder, TFolder_SIZE, frontBlockHeaderPTR);
    if(error != FSE_SUCCESS) return error;
    return updateParentFolderRecord(frontBlockHeaderPTR);
}

TFS_ERROR FS_deleteFolder(char* path, char* name){
    if(path == NULL || name == NULL) return FSE_INVALID_ARGUMENT;
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;

    if(name[0] == '\0' || strchr(name, '/') != NULL) return FSE_INVALID_ARGUMENT;

    void* parentFolderPTR;
    TFS_ERROR error = gotToFolder(path, &parentFolderPTR);
    if(error != FSE_SUCCESS) return error;

    TFolder parentFolder;
    error = copyBlocked(&parentFolder, 0, TFolder_SIZE, parentFolderPTR);
    if(error != FSE_SUCCESS) return error;
    if(!validFolderEntryCount(&parentFolder)) return FSE_BLOCKS_CORRUPTED;
    if(parentFolder.entryCount == 0 ||
       parentFolder.sizeBytes < TFolderRecord_SIZE)
        return FSE_BLOCKS_CORRUPTED;

    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    void* targetFolderPTR = NULL;
    TFolderRecord targetRecord;
    uint64_t targetIndex = 0;
    uint64_t targetRecordOffset = 0;
    for(; targetIndex < parentFolder.entryCount; targetIndex++){
        targetRecordOffset = TFolder_SIZE + targetIndex * TFolderRecord_SIZE;
        TFolderRecord record;
        error = copyBlocked(&record, targetRecordOffset, TFolderRecord_SIZE,
                            parentFolderPTR);
        if(error != FSE_SUCCESS) return error;
        if(record.ownRootFolder || !record.folder) continue;
        if(record.startBlockAddr > diskhead.diskSize - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;

        void* candidatePTR = offsetToPTR(record.startBlockAddr);
        if(cmpName(candidatePTR, name)){
            targetFolderPTR = candidatePTR;
            targetRecord = record;
            break;
        }
    }
    if(targetFolderPTR == NULL) return FSE_PATH_NOT_FOUND;

    error = validateFolderTree(targetFolderPTR, targetRecord.blockCount,
                                PTRToOffset(parentFolderPTR), 0);
    if(error != FSE_SUCCESS) return error;
    error = freeFolderTree(targetFolderPTR, 0);
    if(error != FSE_SUCCESS) return error;

    for(uint64_t i = targetIndex + 1; i < parentFolder.entryCount; i++){
        TFolderRecord nextRecord;
        uint64_t nextOffset = TFolder_SIZE + i * TFolderRecord_SIZE;
        error = copyBlocked(&nextRecord, nextOffset, TFolderRecord_SIZE, parentFolderPTR);
        if(error != FSE_SUCCESS) return error;
        error = insertBlocked(nextOffset - TFolderRecord_SIZE, &nextRecord,
                              TFolderRecord_SIZE, parentFolderPTR);
        if(error != FSE_SUCCESS) return error;
    }

    parentFolder.entryCount--;
    parentFolder.sizeBytes -= TFolderRecord_SIZE;
    error = insertBlocked(0, &parentFolder, TFolder_SIZE, parentFolderPTR);
    if(error != FSE_SUCCESS) return error;
    error = freeEnd(TFolderRecord_SIZE, parentFolderPTR);
    if(error != FSE_SUCCESS) return error;
    parentFolder.blockCount = countRecordBlocks(parentFolderPTR);
    if(parentFolder.blockCount == 0) return FSE_BLOCKS_CORRUPTED;
    error = insertBlocked(0, &parentFolder, TFolder_SIZE, parentFolderPTR);
    if(error != FSE_SUCCESS) return error;
    error = updateParentFolderRecord(parentFolderPTR);
    if(error != FSE_SUCCESS) return error;
    return mergeFreeBlocks();
}

static TFS_ERROR validateFolderTree(void* folderBlockPTR, uint64_t expectedBlockCount,
                                    uint64_t expectedParentOffset, uint64_t depth){
    if(folderBlockPTR == NULL || depth > MAX_FOLDER_DEPTH)
        return FSE_BLOCKS_CORRUPTED;
    if(PTRToOffset(folderBlockPTR) == PTRToOffset(rootBlockPTR))
        return FSE_BLOCKS_CORRUPTED;

    TFolder folder;
    TFS_ERROR error = copyBlocked(&folder, 0, TFolder_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS) return error;
    if(!validFolderEntryCount(&folder)) return FSE_BLOCKS_CORRUPTED;
    uint64_t actualBlockCount = countRecordBlocks(folderBlockPTR);
    if(actualBlockCount == 0 || actualBlockCount != expectedBlockCount ||
       folder.blockCount != actualBlockCount ||
       folder.entryCount > (UINT64_MAX - TFolder_SIZE) / TFolderRecord_SIZE)
        return FSE_BLOCKS_CORRUPTED;

    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    uint64_t ownRootCount = 0;
    for(uint64_t i = 0; i < folder.entryCount; i++){
        TFolderRecord record;
        uint64_t recordOffset = TFolder_SIZE + i * TFolderRecord_SIZE;
        error = copyBlocked(&record, recordOffset, TFolderRecord_SIZE, folderBlockPTR);
        if(error != FSE_SUCCESS) return error;
        if(record.ownRootFolder){
            if(!record.folder || ++ownRootCount > 1 ||
               record.startBlockAddr > diskhead.diskSize - THeaderBlock_SIZE ||
               record.startBlockAddr != expectedParentOffset)
                return FSE_BLOCKS_CORRUPTED;
            continue;
        }
        if(record.startBlockAddr > diskhead.diskSize - THeaderBlock_SIZE ||
           record.blockCount == 0)
            return FSE_BLOCKS_CORRUPTED;

        void* itemBlockPTR = offsetToPTR(record.startBlockAddr);
        uint64_t itemBlocks = countRecordBlocks(itemBlockPTR);
        if(itemBlocks == 0 || itemBlocks != record.blockCount)
            return FSE_BLOCKS_CORRUPTED;

        if(record.folder){
            error = validateFolderTree(itemBlockPTR, record.blockCount,
                                       PTRToOffset(folderBlockPTR), depth + 1);
            if(error != FSE_SUCCESS) return error;
        } else {
            uint64_t itemNameSize = nameSize(itemBlockPTR);
            if(itemNameSize == 0 || record.sizeBytes < itemNameSize)
                return FSE_BLOCKS_CORRUPTED;
        }
    }
    return ownRootCount == 1 ? FSE_SUCCESS : FSE_BLOCKS_CORRUPTED;
}

static TFS_ERROR freeFolderTree(void* folderBlockPTR, uint64_t depth){
    if(folderBlockPTR == NULL || depth > MAX_FOLDER_DEPTH)
        return FSE_BLOCKS_CORRUPTED;

    TFolder folder;
    TFS_ERROR error = copyBlocked(&folder, 0, TFolder_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS) return error;

    for(uint64_t i = 0; i < folder.entryCount; i++){
        TFolderRecord record;
        error = copyBlocked(&record, TFolder_SIZE + i * TFolderRecord_SIZE,
                            TFolderRecord_SIZE, folderBlockPTR);
        if(error != FSE_SUCCESS) return error;
        if(record.ownRootFolder) continue;

        void* itemBlockPTR = offsetToPTR(record.startBlockAddr);
        if(record.folder){
            error = freeFolderTree(itemBlockPTR, depth + 1);
            if(error != FSE_SUCCESS) return error;
        } else {
            error = freeBlocks(itemBlockPTR);
            if(error != FSE_SUCCESS) return error;
        }
    }
    return freeBlocks(folderBlockPTR);
}

TFS_ERROR FS_loadFile(char* path, char* name, TFS_FILE* file){
    if(path == NULL || name == NULL || file == NULL)
        return FSE_INVALID_ARGUMENT;
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;

    file->name = NULL;
    file->size = 0;
    file->data = NULL;

    void* folderBlockPTR;
    TFS_ERROR error = gotToFolder(path, &folderBlockPTR);
    if(error != FSE_SUCCESS) return error;

    TFolder folder;
    error = copyBlocked(&folder, 0, TFolder_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS) return error;
    if(!validFolderEntryCount(&folder)) return FSE_BLOCKS_CORRUPTED;
    if(folder.entryCount == UINT64_MAX ||
       folder.sizeBytes > UINT64_MAX - TFolderRecord_SIZE)
        return FSE_BLOCKS_CORRUPTED;

    void* fileBlockPTR = NULL;
    TFolderRecord fileRecord;
    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    for(uint64_t i = 0; i < folder.entryCount; i++){
        TFolderRecord record;
        error = copyBlocked(&record, TFolder_SIZE + i * TFolderRecord_SIZE,
                            TFolderRecord_SIZE, folderBlockPTR);
        if(error != FSE_SUCCESS) return error;
        if(record.ownRootFolder || record.folder) continue;
        if(record.startBlockAddr > diskhead.diskSize - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;

        void* itemBlockPTR = offsetToPTR(record.startBlockAddr);
        if(cmpName(itemBlockPTR, name)){
            fileBlockPTR = itemBlockPTR;
            fileRecord = record;
            break;
        }
    }
    if(fileBlockPTR == NULL) return FSE_PATH_NOT_FOUND;

    uint64_t itemNameSize = nameSize(fileBlockPTR);
    if(itemNameSize == 0 || fileRecord.sizeBytes < itemNameSize ||
       countRecordBlocks(fileBlockPTR) != fileRecord.blockCount)
        return FSE_BLOCKS_CORRUPTED;

    uint64_t dataSize = fileRecord.sizeBytes - itemNameSize;
    if(dataSize > SIZE_MAX) return FSE_MEMORY_ERROR;
    char* fileName = strdup(name);
    if(fileName == NULL) return FSE_MEMORY_ERROR;
    uint8_t* data = malloc(dataSize == 0 ? 1 : (size_t)dataSize);
    if(data == NULL){
        free(fileName);
        return FSE_MEMORY_ERROR;
    }

    error = copyBlocked(data, 0, dataSize, fileBlockPTR);
    if(error != FSE_SUCCESS){
        free(data);
        free(fileName);
        return error;
    }

    file->name = fileName;
    file->size = dataSize;
    file->data = data;
    return FSE_SUCCESS;
}

TFS_ERROR FS_createFile(char* path, TFS_FILE* file){
    if(path == NULL || file == NULL || file->name == NULL ||
       (file->size != 0 && file->data == NULL))
        return FSE_INVALID_ARGUMENT;
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;
    if(file->name[0] == '\0' || strchr(file->name, '/') != NULL)
        return FSE_INVALID_ARGUMENT;
    if(file->size > SIZE_MAX) return FSE_MEMORY_ERROR;

    uint64_t nameSizeBytes = strlen(file->name) + 1;
    if(file->size > UINT64_MAX - nameSizeBytes)
        return FSE_INVALID_ARGUMENT;
    uint64_t itemSize = nameSizeBytes + file->size;

    void* folderBlockPTR;
    TFS_ERROR error = gotToFolder(path, &folderBlockPTR);
    if(error != FSE_SUCCESS) return error;

    TFolder folder;
    error = copyBlocked(&folder, 0, TFolder_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS) return error;
    if(!validFolderEntryCount(&folder)) return FSE_BLOCKS_CORRUPTED;

    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    for(uint64_t i = 0; i < folder.entryCount; i++){
        TFolderRecord record;
        error = copyBlocked(&record, TFolder_SIZE + i * TFolderRecord_SIZE,
                            TFolderRecord_SIZE, folderBlockPTR);
        if(error != FSE_SUCCESS) return error;
        if(record.ownRootFolder || record.folder) continue;
        if(record.startBlockAddr > diskhead.diskSize - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;
        if(cmpName(offsetToPTR(record.startBlockAddr), file->name))
            return FSE_ALREADY_EXISTS;
    }

    void* fileBlockPTR;
    uint64_t fileBlockCount;
    error = allocateBlocks(itemSize, &fileBlockPTR, &fileBlockCount);
    if(error != FSE_SUCCESS) return error;

    error = writeName(fileBlockPTR, file->name);
    if(error == FSE_SUCCESS && file->size != 0)
        error = insertBlocked(0, file->data, file->size, fileBlockPTR);
    if(error != FSE_SUCCESS){
        TFS_ERROR freeError = freeBlocks(fileBlockPTR);
        return freeError == FSE_SUCCESS ? error : freeError;
    }

    uint64_t addedBlocks;
    error = appendAllocateBlocks(TFolderRecord_SIZE, folderBlockPTR, &addedBlocks);
    if(error != FSE_SUCCESS){
        TFS_ERROR freeError = freeBlocks(fileBlockPTR);
        return freeError == FSE_SUCCESS ? error : freeError;
    }

    error = copyBlocked(&folder, 0, TFolder_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS){
        TFS_ERROR freeError = freeBlocks(fileBlockPTR);
        return freeError == FSE_SUCCESS ? error : freeError;
    }
    folder.blockCount = countRecordBlocks(folderBlockPTR);
    if(folder.blockCount == 0 || folder.entryCount == UINT64_MAX ||
       folder.sizeBytes > UINT64_MAX - TFolderRecord_SIZE){
        TFS_ERROR freeError = freeBlocks(fileBlockPTR);
        return freeError == FSE_SUCCESS ? FSE_BLOCKS_CORRUPTED : freeError;
    }
    error = insertBlocked(0, &folder, TFolder_SIZE, folderBlockPTR);
    if(error == FSE_SUCCESS) error = updateParentFolderRecord(folderBlockPTR);
    if(error != FSE_SUCCESS){
        TFS_ERROR freeError = freeBlocks(fileBlockPTR);
        return freeError == FSE_SUCCESS ? error : freeError;
    }

    TFolderRecord record = {
        .startBlockAddr = PTRToOffset(fileBlockPTR),
        .sizeBytes = itemSize,
        .blockCount = fileBlockCount,
        .folder = false,
        .ownRootFolder = false
    };
    uint64_t recordOffset = TFolder_SIZE + folder.entryCount * TFolderRecord_SIZE;
    error = insertBlocked(recordOffset, &record, TFolderRecord_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS){
        TFS_ERROR freeError = freeBlocks(fileBlockPTR);
        return freeError == FSE_SUCCESS ? error : freeError;
    }

    folder.entryCount++;
    folder.sizeBytes += TFolderRecord_SIZE;
    error = insertBlocked(0, &folder, TFolder_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS) return error;
    return updateParentFolderRecord(folderBlockPTR);
}

TFS_ERROR FS_editFile(char* path, TFS_FILE* file){
    if(path == NULL || file == NULL || file->name == NULL ||
       (file->size != 0 && file->data == NULL))
        return FSE_INVALID_ARGUMENT;
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;
    if(file->name[0] == '\0' || strchr(file->name, '/') != NULL)
        return FSE_INVALID_ARGUMENT;
    if(file->size > SIZE_MAX) return FSE_MEMORY_ERROR;

    void* folderBlockPTR;
    TFS_ERROR error = gotToFolder(path, &folderBlockPTR);
    if(error != FSE_SUCCESS) return error;

    TFolder folder;
    error = copyBlocked(&folder, 0, TFolder_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS) return error;
    if(!validFolderEntryCount(&folder)) return FSE_BLOCKS_CORRUPTED;

    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    void* fileBlockPTR = NULL;
    TFolderRecord fileRecord;
    uint64_t recordOffset = 0;
    for(uint64_t i = 0; i < folder.entryCount; i++){
        recordOffset = TFolder_SIZE + i * TFolderRecord_SIZE;
        TFolderRecord record;
        error = copyBlocked(&record, recordOffset, TFolderRecord_SIZE, folderBlockPTR);
        if(error != FSE_SUCCESS) return error;
        if(record.ownRootFolder || record.folder) continue;
        if(record.startBlockAddr > diskhead.diskSize - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;

        void* itemBlockPTR = offsetToPTR(record.startBlockAddr);
        if(cmpName(itemBlockPTR, file->name)){
            fileBlockPTR = itemBlockPTR;
            fileRecord = record;
            break;
        }
    }
    if(fileBlockPTR == NULL) return FSE_PATH_NOT_FOUND;

    uint64_t itemNameSize = nameSize(fileBlockPTR);
    if(itemNameSize == 0 || fileRecord.sizeBytes < itemNameSize ||
       countRecordBlocks(fileBlockPTR) != fileRecord.blockCount)
        return FSE_BLOCKS_CORRUPTED;
    if(file->size > UINT64_MAX - itemNameSize) return FSE_INVALID_ARGUMENT;

    uint64_t oldDataSize = fileRecord.sizeBytes - itemNameSize;
    if(file->size > oldDataSize){
        uint64_t addedBlocks;
        error = appendAllocateBlocks(file->size - oldDataSize, fileBlockPTR, &addedBlocks);
        if(error != FSE_SUCCESS) return error;
    } else if(file->size < oldDataSize){
        error = freeEnd(oldDataSize - file->size, fileBlockPTR);
        if(error != FSE_SUCCESS) return error;
    }

    if(file->size != 0){
        error = insertBlocked(0, file->data, file->size, fileBlockPTR);
        if(error != FSE_SUCCESS) return error;
    }

    fileRecord.sizeBytes = itemNameSize + file->size;
    fileRecord.blockCount = countRecordBlocks(fileBlockPTR);
    if(fileRecord.blockCount == 0) return FSE_BLOCKS_CORRUPTED;
    return insertBlocked(recordOffset, &fileRecord, TFolderRecord_SIZE, folderBlockPTR);
}

TFS_ERROR FS_deleteFile(char* path, char* name){
    if(path == NULL || name == NULL) return FSE_INVALID_ARGUMENT;
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;

    void* folderBlockPTR;
    TFS_ERROR error = gotToFolder(path, &folderBlockPTR);
    if(error != FSE_SUCCESS) return error;

    TFolder folder;
    error = copyBlocked(&folder, 0, TFolder_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS) return error;
    if(!validFolderEntryCount(&folder)) return FSE_BLOCKS_CORRUPTED;
    if(folder.entryCount == 0 || folder.sizeBytes < TFolderRecord_SIZE)
        return FSE_BLOCKS_CORRUPTED;

    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    void* fileBlockPTR = NULL;
    TFolderRecord fileRecord;
    uint64_t itemIndex = 0;
    uint64_t recordOffset = 0;
    for(; itemIndex < folder.entryCount; itemIndex++){
        recordOffset = TFolder_SIZE + itemIndex * TFolderRecord_SIZE;
        TFolderRecord record;
        error = copyBlocked(&record, recordOffset, TFolderRecord_SIZE, folderBlockPTR);
        if(error != FSE_SUCCESS) return error;
        if(record.ownRootFolder || record.folder) continue;
        if(record.startBlockAddr > diskhead.diskSize - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;

        void* itemBlockPTR = offsetToPTR(record.startBlockAddr);
        if(cmpName(itemBlockPTR, name)){
            fileBlockPTR = itemBlockPTR;
            fileRecord = record;
            break;
        }
    }
    if(fileBlockPTR == NULL) return FSE_PATH_NOT_FOUND;

    uint64_t itemNameSize = nameSize(fileBlockPTR);
    if(itemNameSize == 0 || fileRecord.sizeBytes < itemNameSize ||
       fileRecord.blockCount == 0 ||
       countRecordBlocks(fileBlockPTR) != fileRecord.blockCount)
        return FSE_BLOCKS_CORRUPTED;

    for(uint64_t i = itemIndex + 1; i < folder.entryCount; i++){
        TFolderRecord nextRecord;
        uint64_t nextOffset = TFolder_SIZE + i * TFolderRecord_SIZE;
        error = copyBlocked(&nextRecord, nextOffset, TFolderRecord_SIZE, folderBlockPTR);
        if(error != FSE_SUCCESS) return error;
        error = insertBlocked(nextOffset - TFolderRecord_SIZE, &nextRecord,
                              TFolderRecord_SIZE, folderBlockPTR);
        if(error != FSE_SUCCESS) return error;
    }

    folder.entryCount--;
    folder.sizeBytes -= TFolderRecord_SIZE;
    error = insertBlocked(0, &folder, TFolder_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS) return error;
    error = freeEnd(TFolderRecord_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS) return error;
    folder.blockCount = countRecordBlocks(folderBlockPTR);
    if(folder.blockCount == 0) return FSE_BLOCKS_CORRUPTED;
    error = insertBlocked(0, &folder, TFolder_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS) return error;
    error = updateParentFolderRecord(folderBlockPTR);
    if(error != FSE_SUCCESS) return error;

    error = freeBlocks(fileBlockPTR);
    if(error != FSE_SUCCESS) return error;
    return mergeFreeBlocks();
}


/* -------------------- Helper Functions ------------------- */
static uint64_t processPath(char* rawPath, char** folders){
    uint64_t count = 0;
    char* token = strtok(rawPath, "/"); 
    while(token != NULL) {
        if(count == MAX_FOLDER_DEPTH) return MAX_FOLDER_DEPTH + 1;
        folders[count++] = token;
        token = strtok(NULL, "/");
    }
    return count;
}

static uint64_t nameSize(void* blockHeaderPTR){
    if(drivePTR == NULL || blockHeaderPTR == NULL) return 0;
    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    uint64_t size = 0;
    for(uint64_t i = 0; i < diskhead.blockCount; i++){
        uint64_t blockOffset = PTRToOffset(blockHeaderPTR);
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE) return 0;
        THeaderBlock block;
        memcpy(&block, blockHeaderPTR, THeaderBlock_SIZE);
        if(block.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE ||
           block.deadSize > block.blockSize ||
           (block.free && block.deadSize != 0) ||
           size > UINT64_MAX - blockDataSize(&block))
            return 0;
        uint8_t* data = (uint8_t*)((uint64_t)blockHeaderPTR + THeaderBlock_SIZE);
        uint64_t dataSize = blockDataSize(&block);
        for(uint64_t i = 0; i < dataSize; i++){
            if(data[i] == 0) return size + i + 1;
        }
        size += dataSize;
        if(block.nextRecordBlockAddr == 0) return 0;
        blockHeaderPTR = offsetToPTR(block.nextRecordBlockAddr);
    }
    return 0;
}

static uint64_t blockDataSize(const THeaderBlock* block){
    return block->deadSize <= block->blockSize
        ? block->blockSize - block->deadSize
        : 0;
}

static bool validFolderEntryCount(const TFolder* folder){
    return folder != NULL &&
           folder->entryCount <= (UINT64_MAX - TFolder_SIZE) / TFolderRecord_SIZE;
}

static TFS_ERROR writeName(void* blockHeaderPTR, char* name){
    if(drivePTR == NULL || blockHeaderPTR == NULL || name == NULL)
        return FSE_INVALID_ARGUMENT;
    uint64_t left = strlen(name) + 1;
    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    void* checkBlockPTR = blockHeaderPTR;
    uint64_t capacity = 0;
    for(uint64_t i = 0; i < diskhead.blockCount; i++){
        uint64_t blockOffset = PTRToOffset(checkBlockPTR);
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;
        THeaderBlock checkBlock;
        memcpy(&checkBlock, checkBlockPTR, THeaderBlock_SIZE);
        if(checkBlock.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE ||
           checkBlock.deadSize > checkBlock.blockSize ||
           checkBlock.free ||
           capacity > UINT64_MAX - blockDataSize(&checkBlock))
            return FSE_BLOCKS_CORRUPTED;
        capacity += blockDataSize(&checkBlock);
        if(capacity >= left) break;
        if(checkBlock.nextRecordBlockAddr == 0) return FSE_NAME_TOO_LONG;
        checkBlockPTR = offsetToPTR(checkBlock.nextRecordBlockAddr);
    }
    if(capacity < left) return FSE_NAME_TOO_LONG;

    for(uint64_t i = 0; i < diskhead.blockCount; i++){
        THeaderBlock block;
        memcpy(&block, blockHeaderPTR, THeaderBlock_SIZE);
        uint64_t chunk = MIN(blockDataSize(&block), left);
        memcpy((void*)((uint64_t)blockHeaderPTR + THeaderBlock_SIZE), name, chunk);
        name += chunk;
        left -= chunk;
        if(left == 0) return FSE_SUCCESS;
        if(block.nextRecordBlockAddr == 0) return FSE_NAME_TOO_LONG;
        blockHeaderPTR = offsetToPTR(block.nextRecordBlockAddr);
    }
    return FSE_BLOCKS_CORRUPTED;
}

static bool cmpName(void* blockHeaderPTR, char* name){
    if(blockHeaderPTR == NULL || name == NULL || drivePTR == NULL) return false;
    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    for(uint64_t j = 0; j < diskhead.blockCount; j++){
        uint64_t blockOffset = PTRToOffset(blockHeaderPTR);
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE) return false;
        THeaderBlock block;
        memcpy(&block, blockHeaderPTR, THeaderBlock_SIZE);
        if(block.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE ||
           block.deadSize > block.blockSize || block.free)
            return false;
        uint8_t* data = (uint8_t*)((uint64_t)blockHeaderPTR + THeaderBlock_SIZE);
        for(uint64_t i = 0; i < blockDataSize(&block); i++){
            if(data[i] != (uint8_t)*name) return false;
            if(data[i] == 0) return true;
            name++;
        }
        if(block.nextRecordBlockAddr == 0) return false;
        blockHeaderPTR = offsetToPTR(block.nextRecordBlockAddr);
    }
    return false;
}

static TFS_ERROR copyBlocked(void* dest, uint64_t vOffsetSrc, uint64_t size, void* blockHeaderPTR){
    if(dest == NULL || blockHeaderPTR == NULL) return FSE_INVALID_ARGUMENT;
    if(size == 0) return FSE_SUCCESS;
    uint64_t nameLen = nameSize(blockHeaderPTR);
    if(nameLen == 0 || vOffsetSrc > UINT64_MAX - nameLen) return FSE_BLOCKS_CORRUPTED;
    void* src = vOffsetToPTR(nameLen + vOffsetSrc, blockHeaderPTR, &blockHeaderPTR);
    if(src == NULL) return FSE_BLOCKS_CORRUPTED;
    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    THeaderBlock blockHeader;
    for(uint64_t i = 0; size && i < diskhead.blockCount; i++){
        memcpy(&blockHeader, blockHeaderPTR, THeaderBlock_SIZE);
        uint64_t blockOffset = PTRToOffset(blockHeaderPTR);
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE ||
           blockHeader.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE ||
           blockHeader.deadSize > blockHeader.blockSize ||
           blockHeader.free)
            return FSE_BLOCKS_CORRUPTED;
        uint64_t payload = (uint64_t)blockHeaderPTR + THeaderBlock_SIZE +
                           blockHeader.blockSize - blockHeader.deadSize - (uint64_t)src;
        payload = MIN(payload, size);
        memcpy(dest, src, payload);
        size -= payload;
        if(size == 0) return FSE_SUCCESS;
        if(blockHeader.nextRecordBlockAddr == 0) return FSE_BLOCKS_CORRUPTED;
        dest = (void*)((uint64_t)dest + payload);
        blockHeaderPTR = offsetToPTR(blockHeader.nextRecordBlockAddr);
        if(PTRToOffset(blockHeaderPTR) > diskhead.diskSize - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;
        src = (void*)((uint64_t)blockHeaderPTR + THeaderBlock_SIZE);
    }
    return FSE_BLOCKS_CORRUPTED;
}

static TFS_ERROR insertBlocked(uint64_t vOffsetDest, void* src, uint64_t size, void* blockHeaderPTR){
    if(src == NULL || blockHeaderPTR == NULL) return FSE_INVALID_ARGUMENT;
    if(size == 0) return FSE_SUCCESS;
    uint64_t nameLen = nameSize(blockHeaderPTR);
    if(nameLen == 0 || vOffsetDest > UINT64_MAX - nameLen) return FSE_BLOCKS_CORRUPTED;
    void* dest = vOffsetToPTR(nameLen + vOffsetDest, blockHeaderPTR, &blockHeaderPTR);
    if(dest == NULL) return FSE_BLOCKS_CORRUPTED;
    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);

    uint64_t capacity = 0;
    void* checkBlockPTR = blockHeaderPTR;
    uint64_t offsetInBlock = (uint64_t)dest -
        ((uint64_t)blockHeaderPTR + THeaderBlock_SIZE);
    for(uint64_t i = 0; i < diskhead.blockCount; i++){
        THeaderBlock checkBlock;
        uint64_t blockOffset = PTRToOffset(checkBlockPTR);
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE) return FSE_BLOCKS_CORRUPTED;
        memcpy(&checkBlock, checkBlockPTR, THeaderBlock_SIZE);
        if(checkBlock.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE ||
           checkBlock.deadSize > checkBlock.blockSize ||
           checkBlock.free ||
           offsetInBlock > checkBlock.blockSize)
            return FSE_BLOCKS_CORRUPTED;
        uint64_t dataSize = blockDataSize(&checkBlock);
        if(offsetInBlock > dataSize) return FSE_BLOCKS_CORRUPTED;
        uint64_t available = dataSize - offsetInBlock;
        if(capacity > UINT64_MAX - available) return FSE_BLOCKS_CORRUPTED;
        capacity += available;
        if(capacity >= size) break;
        if(checkBlock.nextRecordBlockAddr == 0) return FSE_BLOCKS_CORRUPTED;
        checkBlockPTR = offsetToPTR(checkBlock.nextRecordBlockAddr);
        offsetInBlock = 0;
    }
    if(capacity < size) return FSE_BLOCKS_CORRUPTED;

    THeaderBlock blockHeader;
    for(uint64_t i = 0; size && i < diskhead.blockCount; i++){
        memcpy(&blockHeader, blockHeaderPTR, THeaderBlock_SIZE);
        uint64_t blockOffset = PTRToOffset(blockHeaderPTR);
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE ||
           blockHeader.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE ||
           blockHeader.deadSize > blockHeader.blockSize ||
           blockHeader.free)
            return FSE_BLOCKS_CORRUPTED;
        uint64_t payload = (uint64_t)blockHeaderPTR + THeaderBlock_SIZE +
                           blockHeader.blockSize - blockHeader.deadSize - (uint64_t)dest;
        payload = MIN(payload, size);
        memcpy(dest, src, payload);
        size -= payload;
        if(size == 0) return FSE_SUCCESS;
        if(blockHeader.nextRecordBlockAddr == 0) return FSE_BLOCKS_CORRUPTED;
        src = (void*)((uint64_t)src + payload);
        blockHeaderPTR = offsetToPTR(blockHeader.nextRecordBlockAddr);
        if(PTRToOffset(blockHeaderPTR) > diskhead.diskSize - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;
        dest = (void*)((uint64_t)blockHeaderPTR + THeaderBlock_SIZE);
    }
    return FSE_BLOCKS_CORRUPTED;
}


static void* vOffsetToPTR(uint64_t offset, void* blockHeaderPTR, void** foundBlockPTR){
    if(drivePTR == NULL || blockHeaderPTR == NULL) return NULL;
    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    THeaderBlock curBlock;
    for(uint64_t i = 0; i < diskhead.blockCount; i++){
        uint64_t blockOffset = PTRToOffset(blockHeaderPTR);
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE) return NULL;
        memcpy(&curBlock, blockHeaderPTR, THeaderBlock_SIZE);
        if(curBlock.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE ||
           curBlock.deadSize > curBlock.blockSize ||
           curBlock.free)
            return NULL;
        uint64_t dataSize = blockDataSize(&curBlock);
        if(offset < dataSize){
            if(foundBlockPTR != NULL) *foundBlockPTR = blockHeaderPTR;
            return (void*)((uint64_t)blockHeaderPTR + THeaderBlock_SIZE + offset);
        }
        offset -= dataSize;
        if(curBlock.nextRecordBlockAddr == 0) return NULL;
        blockHeaderPTR = offsetToPTR(curBlock.nextRecordBlockAddr);
    }
    return NULL;
}

static void* findFolderInFolder(void* curFolderBlockPTR, char* folder){
    TFolder curFolder;
    if(copyBlocked(&curFolder, 0, TFolder_SIZE, curFolderBlockPTR) != FSE_SUCCESS)
        return NULL;
    if(!validFolderEntryCount(&curFolder)) return NULL;
    for(uint64_t i = 0; i < curFolder.entryCount; i++){
        TFolderRecord rec;
        if(copyBlocked(&rec, TFolder_SIZE + i * TFolderRecord_SIZE,
                       TFolderRecord_SIZE, curFolderBlockPTR) != FSE_SUCCESS)
            return NULL;
        if(!rec.folder || rec.ownRootFolder) continue;
        void* recBlockPTR = offsetToPTR(rec.startBlockAddr);
        if(cmpName(recBlockPTR, folder)) return recBlockPTR;
    }
    return NULL;
}

static TFS_ERROR updateParentFolderRecord(void* folderBlockPTR){
    if(folderBlockPTR == rootBlockPTR) return FSE_SUCCESS;

    TFolderRecord parentRecord;
    TFS_ERROR error = copyBlocked(&parentRecord, TFolder_SIZE, TFolderRecord_SIZE, folderBlockPTR);
    if(error != FSE_SUCCESS) return error;
    if(!parentRecord.ownRootFolder) return FSE_SUCCESS;

    void* parentBlockPTR = offsetToPTR(parentRecord.startBlockAddr);
    TFolder parentFolder;
    error = copyBlocked(&parentFolder, 0, TFolder_SIZE, parentBlockPTR);
    if(error != FSE_SUCCESS) return error;
    if(!validFolderEntryCount(&parentFolder)) return FSE_BLOCKS_CORRUPTED;
    uint64_t folderOffset = PTRToOffset(folderBlockPTR);

    for(uint64_t i = 0; i < parentFolder.entryCount; i++){
        TFolderRecord childRecord;
        uint64_t recordOffset = TFolder_SIZE + i * TFolderRecord_SIZE;
        error = copyBlocked(&childRecord, recordOffset, TFolderRecord_SIZE, parentBlockPTR);
        if(error != FSE_SUCCESS) return error;
        if(childRecord.folder && childRecord.startBlockAddr == folderOffset){
            TFolder folder;
            error = copyBlocked(&folder, 0, TFolder_SIZE, folderBlockPTR);
            if(error != FSE_SUCCESS) return error;
            childRecord.sizeBytes = folder.sizeBytes;
            childRecord.blockCount = folder.blockCount;
            return insertBlocked(recordOffset, &childRecord, TFolderRecord_SIZE, parentBlockPTR);
        }
    }
    return FSE_BLOCKS_CORRUPTED;
}

static uint64_t countRecordBlocks(void* blockHeaderPTR){
    if(drivePTR == NULL || blockHeaderPTR == NULL) return 0;
    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    if(diskhead.diskSize < THeaderBlock_SIZE || diskhead.blockCount == 0)
        return 0;
    uint64_t count = 0;
    while(blockHeaderPTR != NULL && count < diskhead.blockCount){
        uint64_t blockOffset = PTRToOffset(blockHeaderPTR);
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE) return 0;
        THeaderBlock block;
        memcpy(&block, blockHeaderPTR, THeaderBlock_SIZE);
        if(block.free || block.deadSize > block.blockSize ||
           block.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE)
            return 0;
        count++;
        if(block.nextRecordBlockAddr == 0) return count;
        if(block.nextRecordBlockAddr > diskhead.diskSize - THeaderBlock_SIZE)
            return 0;
        blockHeaderPTR = offsetToPTR(block.nextRecordBlockAddr);
    }
    return 0;
}

static TFS_ERROR gotToFolder(char* rawPath, void** blockHeaderPTR){
    char* Apath = strdup(rawPath);
    if(Apath == NULL) return FSE_MEMORY_ERROR;

    char* folders[MAX_FOLDER_DEPTH];
    uint64_t folderCount = processPath( Apath, folders);
    if(folderCount > MAX_FOLDER_DEPTH) {
        free(Apath);
        return FSE_PATH_TOO_LONG;
    }

    void* curBlockPTR = rootBlockPTR;
    for(uint64_t i = 0; i < folderCount; i++){
        curBlockPTR = findFolderInFolder(curBlockPTR, folders[i]);
        if(curBlockPTR == NULL) {
            free(Apath);
            return FSE_PATH_NOT_FOUND;
        }
    }
    *blockHeaderPTR = curBlockPTR;
    free(Apath);
    return FSE_SUCCESS; 
}

static TFS_ERROR allocateBlocks(uint64_t size, void** firstBlockPTR, uint64_t* blockCount){
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;
    if(firstBlockPTR == NULL || blockCount == NULL) return FSE_INVALID_ARGUMENT;
    *firstBlockPTR = NULL;
    *blockCount = 0;
    if(size == 0) return FSE_SUCCESS;

    TFS_ERROR mergeError = mergeFreeBlocks();
    if(mergeError != FSE_SUCCESS) return mergeError;

    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    if(diskhead.freeSize < size) return FSE_NO_SPACE_AVALAIBLE;

    void* bestFitBlockPTR = NULL;
    uint64_t bestFitSize = UINT64_MAX;
    void* curBlockPTR = offsetToPTR(diskhead.startRootBlockAddr);
    for(uint64_t i = 0; i < diskhead.blockCount; i++){
        THeaderBlock curBlock;
        memcpy(&curBlock, curBlockPTR, THeaderBlock_SIZE);
        if(curBlock.free && curBlock.deadSize == 0 &&
           curBlock.blockSize >= size && curBlock.blockSize < bestFitSize){
            bestFitBlockPTR = curBlockPTR;
            bestFitSize = curBlock.blockSize;
        }
        if(curBlock.nextBlockAddr == 0) break;
        curBlockPTR = offsetToPTR(curBlock.nextBlockAddr);
    }

    if(bestFitBlockPTR != NULL){
        splitFromBlock(bestFitBlockPTR, size);
        *firstBlockPTR = bestFitBlockPTR;
        *blockCount = 1;
        return FSE_SUCCESS;
    }

    uint64_t remaining = size;
    void* lastBlockPTR = NULL;
    while(remaining){
        THeaderDisk currentDiskhead;
        memcpy(&currentDiskhead, drivePTR, THeaderDisk_SIZE);
        void* blockPTR = NULL;
        uint64_t biggestBlockSize = 0;
        void* scanBlockPTR = offsetToPTR(currentDiskhead.startRootBlockAddr);
        for(uint64_t i = 0; i < currentDiskhead.blockCount; i++){
            THeaderBlock scanBlock;
            memcpy(&scanBlock, scanBlockPTR, THeaderBlock_SIZE);
            if(scanBlock.free && scanBlock.blockSize > biggestBlockSize){
                blockPTR = scanBlockPTR;
                biggestBlockSize = scanBlock.blockSize;
            }
            if(scanBlock.nextBlockAddr == 0) break;
            scanBlockPTR = offsetToPTR(scanBlock.nextBlockAddr);
        }
        if(blockPTR == NULL) return FSE_NO_SPACE_AVALAIBLE;
        splitFromBlock(blockPTR, MIN(biggestBlockSize, remaining));
        THeaderBlock takenBlock;
        memcpy(&takenBlock, blockPTR, THeaderBlock_SIZE);

        if(lastBlockPTR == NULL) *firstBlockPTR = blockPTR;
        else{
            THeaderBlock lastBlock;
            memcpy(&lastBlock, lastBlockPTR, THeaderBlock_SIZE);
            lastBlock.nextRecordBlockAddr = PTRToOffset(blockPTR);
            memcpy(lastBlockPTR, &lastBlock, THeaderBlock_SIZE);
        }
        lastBlockPTR = blockPTR;
        (*blockCount)++;
        remaining -= MIN(takenBlock.blockSize, remaining);
    }
    return FSE_SUCCESS;
}

static TFS_ERROR appendAllocateBlocks(uint64_t size, void* blockPTR, uint64_t* addedBlockCount){
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;
    if(blockPTR == NULL || addedBlockCount == NULL) return FSE_INVALID_ARGUMENT;
    *addedBlockCount = 0;
    if(size == 0) return FSE_SUCCESS;

    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    THeaderBlock curBlock;
    memcpy(&curBlock, blockPTR, THeaderBlock_SIZE);
    if(curBlock.free) return FSE_INVALID_ARGUMENT;
    if(curBlock.deadSize > curBlock.blockSize) return FSE_BLOCKS_CORRUPTED;

    for(uint64_t i = 0; curBlock.nextRecordBlockAddr; i++){
        if(i >= diskhead.blockCount) return FSE_BLOCKS_CORRUPTED;
        blockPTR = offsetToPTR(curBlock.nextRecordBlockAddr);
        if(PTRToOffset(blockPTR) > diskhead.diskSize - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;
        memcpy(&curBlock, blockPTR, THeaderBlock_SIZE);
        if(curBlock.free) return FSE_BLOCKS_CORRUPTED;
    }

    void* tailBlockPTR = blockPTR;

    uint64_t remaining = size;
    remaining -= MIN(remaining, (uint64_t)curBlock.deadSize);
    uint64_t consumedFreeSize = 0;
    THeaderBlock plannedTail = curBlock;
    while(remaining && plannedTail.nextBlockAddr){
        void* nextBlockPTR = offsetToPTR(plannedTail.nextBlockAddr);
        THeaderBlock nextBlock;
        memcpy(&nextBlock, nextBlockPTR, THeaderBlock_SIZE);
        if(!nextBlock.free) break;
        if(nextBlock.deadSize != 0 ||
           PTRToOffset(nextBlockPTR) > diskhead.diskSize - THeaderBlock_SIZE ||
           nextBlock.blockSize > diskhead.diskSize - PTRToOffset(nextBlockPTR) - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;

        if(nextBlock.blockSize >= MIN_FREE_BLOCK_SIZE &&
           remaining <= nextBlock.blockSize - MIN_FREE_BLOCK_SIZE){
            consumedFreeSize += remaining;
            remaining = 0;
        }
        else{
            if(consumedFreeSize > UINT64_MAX - nextBlock.blockSize)
                return FSE_BLOCKS_CORRUPTED;
            consumedFreeSize += nextBlock.blockSize;

            uint64_t growth = THeaderBlock_SIZE + nextBlock.blockSize;
            remaining = remaining > growth ? remaining - growth : 0;
            plannedTail.blockSize += growth;
            plannedTail.nextBlockAddr = nextBlock.nextBlockAddr;
        }
    }
    if(consumedFreeSize > diskhead.freeSize)
        return FSE_BLOCKS_CORRUPTED;
    if(remaining > diskhead.freeSize - consumedFreeSize)
        return FSE_NO_SPACE_AVALAIBLE;

    remaining = size;
    uint64_t deadSizeUsed = MIN(remaining, (uint64_t)curBlock.deadSize);
    curBlock.deadSize -= (uint8_t)deadSizeUsed;
    remaining -= deadSizeUsed;
    bool changed = deadSizeUsed != 0;
    if(remaining == 0){
        if(changed) memcpy(tailBlockPTR, &curBlock, THeaderBlock_SIZE);
        return FSE_SUCCESS;
    }

    while(remaining && curBlock.nextBlockAddr){
        void* nextBlockPTR = offsetToPTR(curBlock.nextBlockAddr);
        THeaderBlock nextBlock;
        memcpy(&nextBlock, nextBlockPTR, THeaderBlock_SIZE);
        if(!nextBlock.free) break;
        if(nextBlock.deadSize != 0 ||
           PTRToOffset(nextBlockPTR) > diskhead.diskSize - THeaderBlock_SIZE ||
           nextBlock.blockSize > diskhead.diskSize - PTRToOffset(nextBlockPTR) - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;

        if(nextBlock.blockSize >= MIN_FREE_BLOCK_SIZE &&
           remaining <= nextBlock.blockSize - MIN_FREE_BLOCK_SIZE){
            curBlock.blockSize += remaining;
            curBlock.nextBlockAddr += remaining;
            nextBlock.blockSize -= remaining;
            diskhead.freeSize -= remaining;
            void* shiftedNextBlockPTR = offsetToPTR(curBlock.nextBlockAddr);
            memcpy(shiftedNextBlockPTR, &nextBlock, THeaderBlock_SIZE);
            remaining = 0;
        }
        else{
            uint64_t growth = THeaderBlock_SIZE + nextBlock.blockSize;
            curBlock.blockSize += growth;
            curBlock.nextBlockAddr = nextBlock.nextBlockAddr;
            diskhead.freeSize -= nextBlock.blockSize;
            diskhead.blockCount--;
            remaining = remaining > growth ? remaining - growth : 0;
        }
        changed = true;
    }

    if(changed){
        memcpy(tailBlockPTR, &curBlock, THeaderBlock_SIZE);
        memcpy(drivePTR, &diskhead, THeaderDisk_SIZE);
    }

    if(remaining){
        void* firstAddedBlockPTR = NULL;
        uint64_t allocatedBlockCount = 0;
        TFS_ERROR error = allocateBlocks(remaining, &firstAddedBlockPTR, &allocatedBlockCount);
        if(error != FSE_SUCCESS) return error;

        memcpy(&curBlock, tailBlockPTR, THeaderBlock_SIZE);
        curBlock.nextRecordBlockAddr = PTRToOffset(firstAddedBlockPTR);
        memcpy(tailBlockPTR, &curBlock, THeaderBlock_SIZE);
        *addedBlockCount = allocatedBlockCount;
    }
    return FSE_SUCCESS;
}

static void* splitFromBlock(void* blockPTR, uint64_t sizeNeeded){
    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    THeaderBlock block;
    memcpy(&block, blockPTR, THeaderBlock_SIZE);

    void* freeBlockPTR = NULL;
    if(block.blockSize >= sizeNeeded + MIN_FREE_FULL_BLOCK_SIZE){
        THeaderBlock freeBlock;
        freeBlock.free = true;
        freeBlock.blockSize = block.blockSize - sizeNeeded - THeaderBlock_SIZE;
        freeBlock.nextBlockAddr = block.nextBlockAddr;
        freeBlock.nextRecordBlockAddr = 0;
        freeBlock.deadSize = 0;

        freeBlockPTR = (void*)((uint64_t)blockPTR + THeaderBlock_SIZE + sizeNeeded);
        block.nextBlockAddr = PTRToOffset(freeBlockPTR);
        block.blockSize = sizeNeeded;

        diskhead.freeSize -= sizeNeeded + THeaderBlock_SIZE;
        diskhead.blockCount++;
        memcpy(freeBlockPTR, &freeBlock, THeaderBlock_SIZE);
        block.deadSize = 0;
    }
    else{
        block.deadSize = (uint8_t)(block.blockSize - sizeNeeded);
        diskhead.freeSize -= block.blockSize;
    }
    block.free = false;
    block.nextRecordBlockAddr = 0;

    memcpy(blockPTR, &block, THeaderBlock_SIZE);
    memcpy(drivePTR, &diskhead, THeaderDisk_SIZE);
    return freeBlockPTR;
}

static TFS_ERROR freeBlocks(void* firstBlockPTR){
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;
    if(firstBlockPTR == NULL) return FSE_INVALID_ARGUMENT;
    if(firstBlockPTR == rootBlockPTR) return FSE_INVALID_ARGUMENT;

    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    if(diskhead.diskSize < THeaderDisk_SIZE + THeaderBlock_SIZE ||
       diskhead.freeSize > diskhead.diskSize ||
       diskhead.blockCount == 0)
        return FSE_BLOCKS_CORRUPTED;

    uint64_t firstBlockOffset = PTRToOffset(firstBlockPTR);
    if(firstBlockOffset > diskhead.diskSize - THeaderBlock_SIZE)
        return FSE_BLOCKS_CORRUPTED;

    uint64_t totalFreeSize = 0;
    uint64_t blocksToFree = 0;
    void* curBlockPTR = firstBlockPTR;
    THeaderBlock block;
    while(true){
        uint64_t blockOffset = PTRToOffset(curBlockPTR);
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE ||
           blocksToFree >= diskhead.blockCount)
            return FSE_BLOCKS_CORRUPTED;
        memcpy(&block, curBlockPTR, THeaderBlock_SIZE);
        if(block.free ||
           block.deadSize > block.blockSize ||
           block.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;
        if(totalFreeSize > UINT64_MAX - block.blockSize)
            return FSE_BLOCKS_CORRUPTED;
        totalFreeSize += block.blockSize;
        blocksToFree++;
        if(block.nextRecordBlockAddr == 0) break;
        curBlockPTR = offsetToPTR(block.nextRecordBlockAddr);
    }
    if(totalFreeSize > diskhead.diskSize - diskhead.freeSize)
        return FSE_BLOCKS_CORRUPTED;

    curBlockPTR = firstBlockPTR;
    while(true){
        memcpy(&block, curBlockPTR, THeaderBlock_SIZE);
        void* nextBlockPTR = block.nextRecordBlockAddr == 0
            ? NULL : offsetToPTR(block.nextRecordBlockAddr);
        block.free = true;
        block.nextRecordBlockAddr = 0;
        block.deadSize = 0;
        memcpy(curBlockPTR, &block, THeaderBlock_SIZE);
        if(nextBlockPTR == NULL) break;
        curBlockPTR = nextBlockPTR;
    }

    diskhead.freeSize += totalFreeSize;
    memcpy(drivePTR, &diskhead, THeaderDisk_SIZE);
    return FSE_SUCCESS;
}

static TFS_ERROR freeEnd(uint64_t size, void* firstBlockPTR){
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;
    if(firstBlockPTR == NULL) return FSE_INVALID_ARGUMENT;
    if(size == 0) return FSE_SUCCESS;

    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    if(diskhead.diskSize < THeaderDisk_SIZE + THeaderBlock_SIZE ||
       diskhead.freeSize > diskhead.diskSize ||
       diskhead.blockCount == 0)
        return FSE_BLOCKS_CORRUPTED;

    uint64_t totalDataSize = 0;
    uint64_t blockCount = 0;
    void* curBlockPTR = firstBlockPTR;
    THeaderBlock block;
    while(true){
        uint64_t blockOffset = PTRToOffset(curBlockPTR);
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE ||
           blockCount >= diskhead.blockCount)
            return FSE_BLOCKS_CORRUPTED;

        memcpy(&block, curBlockPTR, THeaderBlock_SIZE);
        if(block.free ||
           block.deadSize > block.blockSize ||
           block.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;

        uint64_t usableSize = blockDataSize(&block);
        if(totalDataSize > UINT64_MAX - usableSize)
            return FSE_BLOCKS_CORRUPTED;
        totalDataSize += usableSize;
        blockCount++;

        if(block.nextRecordBlockAddr == 0) break;
        curBlockPTR = offsetToPTR(block.nextRecordBlockAddr);
    }

    uint64_t nameLength = nameSize(firstBlockPTR);
    if(nameLength == 0 || nameLength > totalDataSize) return FSE_BLOCKS_CORRUPTED;
    if(size > totalDataSize - nameLength) return FSE_INVALID_ARGUMENT;

    uint64_t keptDataSize = totalDataSize - size;
    uint64_t prefixSize = 0;
    curBlockPTR = firstBlockPTR;
    void* keptTailPTR = NULL;
    void* insertedFreeBlockPTR = NULL;
    THeaderBlock insertedFreeBlock;
    uint64_t insertedFreePayload = 0;
    THeaderBlock keptTail;
    while(curBlockPTR != NULL){
        memcpy(&block, curBlockPTR, THeaderBlock_SIZE);
        uint64_t usableSize = blockDataSize(&block);
        uint64_t blockEnd = prefixSize + usableSize;

        if(keptDataSize < blockEnd){
            uint64_t keptInBlock = keptDataSize - prefixSize;
            uint64_t newDeadSize = block.blockSize - keptInBlock;
            if(newDeadSize > UINT8_MAX){
                if(newDeadSize < THeaderBlock_SIZE + MIN_FREE_BLOCK_SIZE)
                    return FSE_BLOCKS_CORRUPTED;
                insertedFreePayload = newDeadSize - THeaderBlock_SIZE;
                uint64_t blockOffset = PTRToOffset(curBlockPTR);
                insertedFreeBlockPTR = offsetToPTR(blockOffset + THeaderBlock_SIZE + keptInBlock);
                insertedFreeBlock.blockSize = insertedFreePayload;
                insertedFreeBlock.nextBlockAddr = block.nextBlockAddr;
                insertedFreeBlock.nextRecordBlockAddr = 0;
                insertedFreeBlock.deadSize = 0;
                insertedFreeBlock.free = true;
                block.blockSize = keptInBlock;
                block.nextBlockAddr = PTRToOffset(insertedFreeBlockPTR);
                block.deadSize = 0;
            } else {
                block.deadSize = (uint8_t)newDeadSize;
            }
            keptTailPTR = curBlockPTR;
            keptTail = block;
            break;
        }

        if(keptDataSize == blockEnd){
            keptTailPTR = curBlockPTR;
            keptTail = block;
            break;
        }

        prefixSize = blockEnd;
        curBlockPTR = block.nextRecordBlockAddr == 0
            ? NULL : offsetToPTR(block.nextRecordBlockAddr);
    }

    if(keptTailPTR == NULL) return FSE_BLOCKS_CORRUPTED;

    void* firstFreedBlockPTR = keptTail.nextRecordBlockAddr == 0
        ? NULL : offsetToPTR(keptTail.nextRecordBlockAddr);
    uint64_t releasedPhysicalSize = 0;
    uint64_t releasedBlockCount = 0;
    curBlockPTR = firstFreedBlockPTR;
    while(curBlockPTR != NULL){
        uint64_t blockOffset = PTRToOffset(curBlockPTR);
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE ||
           releasedBlockCount >= diskhead.blockCount)
            return FSE_BLOCKS_CORRUPTED;
        memcpy(&block, curBlockPTR, THeaderBlock_SIZE);
        if(block.free ||
           block.deadSize > block.blockSize ||
           block.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE ||
           releasedPhysicalSize > UINT64_MAX - block.blockSize)
            return FSE_BLOCKS_CORRUPTED;
        releasedPhysicalSize += block.blockSize;
        releasedBlockCount++;
        curBlockPTR = block.nextRecordBlockAddr == 0
            ? NULL : offsetToPTR(block.nextRecordBlockAddr);
    }
    if((insertedFreePayload != 0 &&
        (diskhead.blockCount == UINT64_MAX ||
         insertedFreePayload > diskhead.diskSize - diskhead.freeSize)) ||
       releasedPhysicalSize > diskhead.diskSize - diskhead.freeSize - insertedFreePayload)
        return FSE_BLOCKS_CORRUPTED;

    keptTail.nextRecordBlockAddr = 0;
    if(insertedFreeBlockPTR != NULL){
        memcpy(insertedFreeBlockPTR, &insertedFreeBlock, THeaderBlock_SIZE);
        diskhead.freeSize += insertedFreePayload;
        diskhead.blockCount++;
    }
    memcpy(keptTailPTR, &keptTail, THeaderBlock_SIZE);

    curBlockPTR = firstFreedBlockPTR;
    while(curBlockPTR != NULL){
        memcpy(&block, curBlockPTR, THeaderBlock_SIZE);
        void* nextRecordBlockPTR = block.nextRecordBlockAddr == 0
            ? NULL : offsetToPTR(block.nextRecordBlockAddr);
        block.free = true;
        block.deadSize = 0;
        block.nextRecordBlockAddr = 0;
        memcpy(curBlockPTR, &block, THeaderBlock_SIZE);
        curBlockPTR = nextRecordBlockPTR;
    }

    diskhead.freeSize += releasedPhysicalSize;
    memcpy(drivePTR, &diskhead, THeaderDisk_SIZE);
    return FSE_SUCCESS;
}

static TFS_ERROR mergeFreeBlocks(void){
    if(drivePTR == NULL) return FSE_NOT_MOUNTED;

    THeaderDisk diskhead;
    memcpy(&diskhead, drivePTR, THeaderDisk_SIZE);
    if(diskhead.diskSize < THeaderDisk_SIZE + THeaderBlock_SIZE ||
       diskhead.startRootBlockAddr > diskhead.diskSize - THeaderBlock_SIZE ||
       diskhead.blockCount == 0 ||
       diskhead.freeSize > diskhead.diskSize)
        return FSE_BLOCKS_CORRUPTED;

    uint64_t actualBlockCount = 0;
    uint64_t actualFreeSize = 0;
    uint64_t additionalFreeSize = 0;
    uint64_t blocksToRemove = 0;
    uint64_t blockOffset = diskhead.startRootBlockAddr;
    bool previousFree = false;
    uint8_t previousDeadSize = 0;

    while(true){
        if(blockOffset > diskhead.diskSize - THeaderBlock_SIZE ||
           actualBlockCount >= diskhead.blockCount)
            return FSE_BLOCKS_CORRUPTED;

        THeaderBlock block;
        memcpy(&block, offsetToPTR(blockOffset), THeaderBlock_SIZE);
        if(block.blockSize > diskhead.diskSize - blockOffset - THeaderBlock_SIZE ||
           block.deadSize > block.blockSize ||
           (block.free && block.deadSize != 0) ||
           (actualBlockCount == 0 && block.free))
            return FSE_BLOCKS_CORRUPTED;

        if(block.free){
            if(actualFreeSize > UINT64_MAX - block.blockSize)
                return FSE_BLOCKS_CORRUPTED;
            actualFreeSize += block.blockSize;

            uint64_t additional = previousFree
                ? THeaderBlock_SIZE
                : (uint64_t)previousDeadSize;
            if(additionalFreeSize > UINT64_MAX - additional)
                return FSE_BLOCKS_CORRUPTED;
            additionalFreeSize += additional;
            if(previousFree) blocksToRemove++;
        }

        actualBlockCount++;
        uint64_t expectedNextOffset = blockOffset + THeaderBlock_SIZE + block.blockSize;
        if(block.nextBlockAddr == 0){
            if(expectedNextOffset != diskhead.diskSize)
                return FSE_BLOCKS_CORRUPTED;
            break;
        }
        if(block.nextBlockAddr != expectedNextOffset ||
           block.nextBlockAddr > diskhead.diskSize - THeaderBlock_SIZE)
            return FSE_BLOCKS_CORRUPTED;

        previousFree = block.free;
        previousDeadSize = block.deadSize;
        blockOffset = block.nextBlockAddr;
    }

    if(actualBlockCount != diskhead.blockCount ||
       actualFreeSize != diskhead.freeSize ||
       blocksToRemove >= diskhead.blockCount ||
       additionalFreeSize > diskhead.diskSize - diskhead.freeSize)
        return FSE_BLOCKS_CORRUPTED;

    uint64_t currentOffset = diskhead.startRootBlockAddr;
    void* previousBlockPTR = NULL;
    uint64_t scannedBlocks = 0;
    while(currentOffset != 0){
        if(scannedBlocks >= actualBlockCount)
            return FSE_BLOCKS_CORRUPTED;

        void* currentBlockPTR = offsetToPTR(currentOffset);
        THeaderBlock currentBlock;
        memcpy(&currentBlock, currentBlockPTR, THeaderBlock_SIZE);
        uint64_t nextOffset = currentBlock.nextBlockAddr;

        if(currentBlock.free && previousBlockPTR != NULL){
            THeaderBlock previousBlock;
            memcpy(&previousBlock, previousBlockPTR, THeaderBlock_SIZE);

            if(previousBlock.free){
                if(previousBlock.blockSize > UINT64_MAX - THeaderBlock_SIZE - currentBlock.blockSize)
                    return FSE_BLOCKS_CORRUPTED;
                previousBlock.blockSize += THeaderBlock_SIZE + currentBlock.blockSize;
                previousBlock.nextBlockAddr = nextOffset;
                memcpy(previousBlockPTR, &previousBlock, THeaderBlock_SIZE);
                diskhead.freeSize += THeaderBlock_SIZE;
                diskhead.blockCount--;

                currentOffset = nextOffset;
                scannedBlocks++;
                continue;
            }

            if(previousBlock.deadSize != 0){
                uint64_t deadSize = previousBlock.deadSize;
                uint64_t previousOffset = PTRToOffset(previousBlockPTR);
                if(deadSize > previousBlock.blockSize ||
                   currentOffset < previousOffset + THeaderBlock_SIZE + deadSize ||
                   currentBlock.blockSize > UINT64_MAX - deadSize)
                    return FSE_BLOCKS_CORRUPTED;

                uint64_t movedFreeOffset = currentOffset - deadSize;
                previousBlock.blockSize -= deadSize;
                previousBlock.nextBlockAddr = movedFreeOffset;
                previousBlock.deadSize = 0;

                currentBlock.blockSize += deadSize;
                currentBlock.deadSize = 0;
                memcpy(previousBlockPTR, &previousBlock, THeaderBlock_SIZE);
                currentBlockPTR = offsetToPTR(movedFreeOffset);
                memcpy(currentBlockPTR, &currentBlock, THeaderBlock_SIZE);
                diskhead.freeSize += deadSize;
                currentOffset = movedFreeOffset;
            }
        }

        previousBlockPTR = currentBlockPTR;
        scannedBlocks++;
        currentOffset = nextOffset;
    }

    memcpy(drivePTR, &diskhead, THeaderDisk_SIZE);
    return FSE_SUCCESS;
}

/* ******************** Inline Helper Functions ******************** */
inline static void* offsetToPTR(uint64_t offset){
    return (void*)((uint64_t)drivePTR + offset);
}

inline static uint64_t PTRToOffset(void* ptr){
    return (uint64_t)ptr - (uint64_t)drivePTR;
}
