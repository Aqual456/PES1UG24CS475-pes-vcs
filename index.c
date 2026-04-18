#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include "pes.h"
#include <errno.h>

// 🔥 REQUIRED: declare object_write
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ───────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
for (int i = 0; i < index->count; i++) {
if (strcmp(index->entries[i].path, path) == 0)
return &index->entries[i];
}
return NULL;
}

int index_remove(Index *index, const char *path) {
for (int i = 0; i < index->count; i++) {
if (strcmp(index->entries[i].path, path) == 0) {
int remaining = index->count - i - 1;
if (remaining > 0)
memmove(&index->entries[i], &index->entries[i + 1],
remaining * sizeof(IndexEntry));
index->count--;
return index_save(index);
}
}
fprintf(stderr, "error: '%s' is not in the index\n", path);
return -1;
}

int index_status(const Index *index) {
printf("Staged changes:\n");
int staged_count = 0;

for (int i = 0; i < index->count; i++) {
    printf("  staged:     %s\n", index->entries[i].path);
    staged_count++;
}
if (staged_count == 0) printf("  (nothing to show)\n");
printf("\n");

printf("Unstaged changes:\n");
int unstaged_count = 0;

for (int i = 0; i < index->count; i++) {
    struct stat st;
    if (stat(index->entries[i].path, &st) != 0) {
        printf("  deleted:    %s\n", index->entries[i].path);
        unstaged_count++;
    } else {
        if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
            st.st_size != (off_t)index->entries[i].size) {
            printf("  modified:   %s\n", index->entries[i].path);
            unstaged_count++;
        }
    }
}
if (unstaged_count == 0) printf("  (nothing to show)\n");
printf("\n");

printf("Untracked files:\n");
int untracked_count = 0;

DIR *dir = opendir(".");
if (dir) {
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {

        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0 ||
            strcmp(ent->d_name, ".pes") == 0 ||
            strcmp(ent->d_name, "pes") == 0 ||
            strstr(ent->d_name, ".o") != NULL)
            continue;

        int tracked = 0;
        for (int i = 0; i < index->count; i++) {
            if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                tracked = 1;
                break;
            }
        }

        if (!tracked) {
            struct stat st;
            stat(ent->d_name, &st);
            if (S_ISREG(st.st_mode)) {
                printf("  untracked:  %s\n", ent->d_name);
                untracked_count++;
            }
        }
    }
    closedir(dir);
}

if (untracked_count == 0) printf("  (nothing to show)\n");
printf("\n");

return 0;

}

// ─── IMPLEMENTATION ─────────────────────────────────────────────────────────

int index_load(Index *index) {
FILE *f = fopen(INDEX_FILE, "r");


if (!f) {
    index->count = 0;
    return 0;
}

index->count = 0;

char line[1024];

while (fgets(line, sizeof(line), f)) {
    IndexEntry *e = &index->entries[index->count];

    char hex[HASH_HEX_SIZE + 1];

    if (sscanf(line, "%o %64s %lu %u %511[^\n]",
               &e->mode,
               hex,
               &e->mtime_sec,
               &e->size,
               e->path) == 5) {

        hex_to_hash(hex, &e->hash);
        index->count++;
    }
}

fclose(f);
return 0;


}

int compare_entries(const void *a, const void *b) {
return strcmp(((IndexEntry*)a)->path, ((IndexEntry*)b)->path);
}

int index_save(const Index *index) {
// 🔥 FIX: ensure .pes directory exists
if (mkdir(".pes", 0755) != 0 && errno != EEXIST) {
return -1;
}

char tmp_path[512];
snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

FILE *f = fopen(tmp_path, "w");
if (!f) return -1;

IndexEntry temp[MAX_INDEX_ENTRIES];
int n = index->count;

if (n < 0 || n > MAX_INDEX_ENTRIES) {
    fclose(f);
    return -1;
}

memcpy(temp, index->entries, sizeof(IndexEntry) * n);

qsort(temp, n, sizeof(IndexEntry), compare_entries);

for (int i = 0; i < n; i++) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&temp[i].hash, hex);

    fprintf(f, "%o %s %lu %u %s\n",
            temp[i].mode,
            hex,
            temp[i].mtime_sec,
            temp[i].size,
            temp[i].path);
}

fflush(f);
fsync(fileno(f));
fclose(f);

return rename(tmp_path, INDEX_FILE);


}

int index_add(Index *index, const char *path) {
FILE *f = fopen(path, "rb");
if (!f) return -1;

fseek(f, 0, SEEK_END);
size_t size = ftell(f);
rewind(f);

void *data = NULL;

if (size > 0) {
    data = malloc(size);
    if (!data) {
        fclose(f);
        return -1;
    }

    if (fread(data, 1, size, f) != size) {
        free(data);
        fclose(f);
        return -1;
    }
}

fclose(f);

ObjectID id;
if (object_write(OBJ_BLOB, data, size, &id) != 0) {
    free(data);
    return -1;
}

free(data);

struct stat st;
if (stat(path, &st) != 0) return -1;

IndexEntry *e = index_find(index, path);

if (!e) {
    if (index->count >= MAX_INDEX_ENTRIES) return -1;
    e = &index->entries[index->count++];
}

e->mode = 100644;
e->hash = id;
e->mtime_sec = st.st_mtime;
e->size = st.st_size;

strncpy(e->path, path, sizeof(e->path) - 1);
e->path[sizeof(e->path) - 1] = '\0';

return index_save(index);

}
