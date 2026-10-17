#include <stdlib.h>
#include <string.h>
#include "rfs.h"

static char* cwd = NULL;

static const char* currentDir(void){
    return cwd ? cwd : "";
}

static TFS_ERROR setCwd(const char* path){
    char* copy = malloc(strlen(path) + 1);
    if(!copy) return FSE_MEMORY_ERROR;
    strcpy(copy, path);
    free(cwd);
    cwd = copy;
    return FSE_SUCCESS;
}

static TFS_ERROR resolve(const char* rel, char** out){
    if(!rel || !out) return FSE_INVALID_ARGUMENT;
    const char* base = rel[0] == '/' ? "" : currentDir();
    size_t baseLen = strlen(base);
    size_t relLen = strlen(rel);
    char* res = malloc(baseLen + relLen + 2);
    if(!res) return FSE_MEMORY_ERROR;
    memcpy(res, base, baseLen + 1);
    size_t len = baseLen;
    const char* p = rel;
    while(*p){
        while(*p == '/') p++;
        const char* start = p;
        while(*p && *p != '/') p++;
        size_t cl = (size_t)(p - start);
        if(cl == 0 || (cl == 1 && start[0] == '.')) continue;
        if(cl == 2 && start[0] == '.' && start[1] == '.'){
            if(len == 0){
                free(res);
                return FSE_PATH_NOT_FOUND;
            }
            while(len > 0 && res[len - 1] != '/') len--;
            if(len > 0) len--;
            res[len] = '\0';
            continue;
        }
        if(len > 0) res[len++] = '/';
        memcpy(res + len, start, cl);
        len += cl;
        res[len] = '\0';
    }
    *out = res;
    return FSE_SUCCESS;
}

static TFS_ERROR split(const char* full, char** dir, const char** name){
    const char* last = strrchr(full, '/');
    size_t dirLen = last ? (size_t)(last - full) : 0;
    const char* n = last ? last + 1 : full;
    if(*n == '\0') return FSE_INVALID_ARGUMENT;
    char* d = malloc(dirLen + 1);
    if(!d) return FSE_MEMORY_ERROR;
    memcpy(d, full, dirLen);
    d[dirLen] = '\0';
    *dir = d;
    *name = n;
    return FSE_SUCCESS;
}

static TFS_ERROR resolveSplit(const char* rel, char** full, char** dir, const char** name){
    TFS_ERROR e = resolve(rel, full);
    if(e != FSE_SUCCESS) return e;
    e = split(*full, dir, name);
    if(e != FSE_SUCCESS) free(*full);
    return e;
}

void RFS_reset(void){
    free(cwd);
    cwd = NULL;
}

TFS_ERROR RFS_changeDir(char* path){
    char* full;
    TFS_ERROR e = resolve(path, &full);
    if(e != FSE_SUCCESS) return e;
    uint64_t count;
    e = FS_getItemCount(full, &count);
    if(e == FSE_SUCCESS) e = setCwd(full);
    free(full);
    return e;
}

TFS_ERROR RFS_getWorkingDir(char** path){
    if(!path) return FSE_INVALID_ARGUMENT;
    const char* c = currentDir();
    char* out = malloc(strlen(c) + 2);
    if(!out) return FSE_MEMORY_ERROR;
    out[0] = '/';
    strcpy(out + 1, c);
    *path = out;
    return FSE_SUCCESS;
}

TFS_ERROR RFS_getItemCount(char* path, uint64_t* count){
    char* full;
    TFS_ERROR e = resolve(path, &full);
    if(e != FSE_SUCCESS) return e;
    e = FS_getItemCount(full, count);
    free(full);
    return e;
}

TFS_ERROR RFS_nameByID(char* path, uint64_t id, char** name){
    char* full;
    TFS_ERROR e = resolve(path, &full);
    if(e != FSE_SUCCESS) return e;
    e = FS_nameByID(full, id, name);
    free(full);
    return e;
}

TFS_ERROR RFS_isItemFolderByID(char* path, uint64_t id, bool* isFolder){
    char* full;
    TFS_ERROR e = resolve(path, &full);
    if(e != FSE_SUCCESS) return e;
    e = FS_isItemFolderByID(full, id, isFolder);
    free(full);
    return e;
}

TFS_ERROR RFS_createFolder(char* path){
    char *full, *dir;
    const char* name;
    TFS_ERROR e = resolveSplit(path, &full, &dir, &name);
    if(e != FSE_SUCCESS) return e;
    e = FS_createFolder(dir, (char*)name);
    free(dir);
    free(full);
    return e;
}

TFS_ERROR RFS_deleteFolder(char* path){
    char *full, *dir;
    const char* name;
    TFS_ERROR e = resolveSplit(path, &full, &dir, &name);
    if(e != FSE_SUCCESS) return e;
    e = FS_deleteFolder(dir, (char*)name);
    if(e == FSE_SUCCESS){
        size_t fl = strlen(full);
        const char* c = currentDir();
        if(strncmp(c, full, fl) == 0 && (c[fl] == '\0' || c[fl] == '/')) e = setCwd(dir);
    }
    free(dir);
    free(full);
    return e;
}

TFS_ERROR RFS_deleteFile(char* path){
    char *full, *dir;
    const char* name;
    TFS_ERROR e = resolveSplit(path, &full, &dir, &name);
    if(e != FSE_SUCCESS) return e;
    e = FS_deleteFile(dir, (char*)name);
    free(dir);
    free(full);
    return e;
}

TFS_ERROR RFS_openFile(char* path, TRFS_MODE mode, TRFS_FILE** file){
    if(!file) return FSE_INVALID_ARGUMENT;
    if(mode != RFS_MODE_READ && mode != RFS_MODE_WRITE && mode != RFS_MODE_CREATE) return FSE_INVALID_ARGUMENT;
    char *full, *dir;
    const char* name;
    TFS_ERROR e = resolveSplit(path, &full, &dir, &name);
    if(e != FSE_SUCCESS) return e;
    char* nameCopy = malloc(strlen(name) + 1);
    if(nameCopy) strcpy(nameCopy, name);
    free(full);
    if(!nameCopy){
        free(dir);
        return FSE_MEMORY_ERROR;
    }
    if(mode == RFS_MODE_CREATE){
        TFS_FILE empty = { nameCopy, 0, NULL };
        e = FS_createFile(dir, &empty);
        if(e == FSE_ALREADY_EXISTS) e = FSE_SUCCESS;
        if(e != FSE_SUCCESS){
            free(nameCopy);
            free(dir);
            return e;
        }
    }
    TRFS_FILE* h = calloc(1, sizeof(TRFS_FILE));
    if(!h){
        free(nameCopy);
        free(dir);
        return FSE_MEMORY_ERROR;
    }
    e = FS_loadFile(dir, nameCopy, &h->file);
    free(nameCopy);
    if(e != FSE_SUCCESS){
        free(dir);
        free(h);
        return e;
    }
    h->dir = dir;
    h->capacity = h->file.size;
    h->writable = mode != RFS_MODE_READ;
    *file = h;
    return FSE_SUCCESS;
}

TFS_ERROR RFS_readFile(TRFS_FILE* file, uint64_t offset, void* buffer, uint64_t len, uint64_t* read){
    if(!file || (!buffer && len > 0)) return FSE_INVALID_ARGUMENT;
    uint64_t n = 0;
    if(offset < file->file.size){
        n = file->file.size - offset;
        if(n > len) n = len;
        if(n > 0) memcpy(buffer, file->file.data + offset, n);
    }
    if(read) *read = n;
    return FSE_SUCCESS;
}

static TFS_ERROR ensureSize(TRFS_FILE* file, uint64_t size){
    if(size > file->capacity){
        uint64_t cap = file->capacity * 2;
        if(cap < size) cap = size;
        if(cap > (uint64_t)SIZE_MAX) return FSE_MEMORY_ERROR;
        uint8_t* nd = realloc(file->file.data, cap > 0 ? cap : 1);
        if(!nd) return FSE_MEMORY_ERROR;
        file->file.data = nd;
        file->capacity = cap;
    }
    if(size > file->file.size) memset(file->file.data + file->file.size, 0, size - file->file.size);
    file->file.size = size;
    return FSE_SUCCESS;
}

TFS_ERROR RFS_writeFile(TRFS_FILE* file, uint64_t offset, const void* buffer, uint64_t len){
    if(!file || !file->writable || (!buffer && len > 0)) return FSE_INVALID_ARGUMENT;
    if(len == 0) return FSE_SUCCESS;
    if(offset > UINT64_MAX - len) return FSE_INVALID_ARGUMENT;
    uint64_t end = offset + len;
    if(end > file->file.size){
        TFS_ERROR e = ensureSize(file, end);
        if(e != FSE_SUCCESS) return e;
    }
    memcpy(file->file.data + offset, buffer, len);
    file->dirty = true;
    return FSE_SUCCESS;
}

TFS_ERROR RFS_resizeFile(TRFS_FILE* file, uint64_t size){
    if(!file || !file->writable) return FSE_INVALID_ARGUMENT;
    TFS_ERROR e = ensureSize(file, size);
    if(e == FSE_SUCCESS) file->dirty = true;
    return e;
}

TFS_ERROR RFS_fileSize(TRFS_FILE* file, uint64_t* size){
    if(!file || !size) return FSE_INVALID_ARGUMENT;
    *size = file->file.size;
    return FSE_SUCCESS;
}

TFS_ERROR RFS_flushFile(TRFS_FILE* file){
    if(!file) return FSE_INVALID_ARGUMENT;
    if(!file->dirty) return FSE_SUCCESS;
    TFS_ERROR e = FS_editFile(file->dir, &file->file);
    if(e == FSE_SUCCESS) file->dirty = false;
    return e;
}

TFS_ERROR RFS_closeFile(TRFS_FILE* file){
    if(!file) return FSE_SUCCESS;
    TFS_ERROR e = RFS_flushFile(file);
    free(file->file.name);
    free(file->file.data);
    free(file->dir);
    free(file);
    return e;
}
