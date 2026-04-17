#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <errno.h>

void hash_to_hex(const ObjectID *id, char *hex_out) {
for (int i = 0; i < HASH_SIZE; i++) {
sprintf(hex_out + i * 2, "%02x", id->hash[i]);
}
hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
if (strlen(hex) < HASH_HEX_SIZE) return -1;
for (int i = 0; i < HASH_SIZE; i++) {
unsigned int byte;
if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
id_out->hash[i] = (uint8_t)byte;
}
return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
unsigned int hash_len;
EVP_MD_CTX *ctx = EVP_MD_CTX_new();
EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
EVP_DigestUpdate(ctx, data, len);
EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
char hex[HASH_HEX_SIZE + 1];
hash_to_hex(id, hex);
snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
char path[512];
object_path(id, path, sizeof(path));
return access(path, F_OK) == 0;
}

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
const char *type_str;
if      (type == OBJ_BLOB)   type_str = "blob";
else if (type == OBJ_TREE)   type_str = "tree";
else if (type == OBJ_COMMIT) type_str = "commit";
else return -1;


char header[64];
int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
if (header_len < 0 || (size_t)header_len >= sizeof(header)) return -1;
header_len++; // include '\0'

size_t total = header_len + len;
uint8_t *buf = malloc(total);
if (!buf) return -1;

memcpy(buf, header, header_len);
memcpy(buf + header_len, data, len);

compute_hash(buf, total, id_out);

if (object_exists(id_out)) {
    free(buf);
    return 0;
}

// Create directories safely
if (mkdir(".pes", 0755) != 0 && errno != EEXIST) {
    free(buf);
    return -1;
}

if (mkdir(OBJECTS_DIR, 0755) != 0 && errno != EEXIST) {
    free(buf);
    return -1;
}

char hex[HASH_HEX_SIZE + 1];
hash_to_hex(id_out, hex);

char shard_dir[256];
snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);

if (mkdir(shard_dir, 0755) != 0 && errno != EEXIST) {
    free(buf);
    return -1;
}

char final_path[512];
object_path(id_out, final_path, sizeof(final_path));

// Create temp file INSIDE shard dir
char tmp_path[512];
snprintf(tmp_path, sizeof(tmp_path), "%s/tmp.XXXXXX", shard_dir);

int fd = mkstemp(tmp_path);
if (fd < 0) {
    free(buf);
    return -1;
}

// Write fully
size_t written = 0;
while (written < total) {
    ssize_t n = write(fd, buf + written, total - written);
    if (n <= 0) {
        close(fd);
        unlink(tmp_path);
        free(buf);
        return -1;
    }
    written += n;
}

if (fsync(fd) != 0) {
    close(fd);
    unlink(tmp_path);
    free(buf);
    return -1;
}

close(fd);
free(buf);

if (rename(tmp_path, final_path) != 0) {
    unlink(tmp_path);
    return -1;
}

int dir_fd = open(shard_dir, O_RDONLY);
if (dir_fd >= 0) {
    fsync(dir_fd);
    close(dir_fd);
}

return 0;


}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
char path[512];
object_path(id, path, sizeof(path));

FILE *f = fopen(path, "rb");
if (!f) return -1;

if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
long fsz = ftell(f);
if (fsz < 0) { fclose(f); return -1; }
rewind(f);

uint8_t *raw = malloc((size_t)fsz);
if (!raw) { fclose(f); return -1; }

if (fread(raw, 1, (size_t)fsz, f) != (size_t)fsz) {
    free(raw);
    fclose(f);
    return -1;
}
fclose(f);

ObjectID computed;
compute_hash(raw, (size_t)fsz, &computed);

if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
    free(raw);
    return -1;
}

uint8_t *nul = memchr(raw, '\0', (size_t)fsz);
if (!nul) {
    free(raw);
    return -1;
}

size_t data_len = (size_t)fsz - (size_t)(nul - raw) - 1;

if      (strncmp((char *)raw, "blob ", 5) == 0) *type_out = OBJ_BLOB;
else {
    free(raw);
    return -1;
}

uint8_t *payload = malloc(data_len ? data_len : 1);
if (!payload) {
    free(raw);
    return -1;
}

memcpy(payload, nul + 1, data_len);

*data_out = payload;
*len_out = data_len;

free(raw);
return 0;

}
