// SPDX-License-Identifier: Apache-2.0
#include "netfs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

double nf_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
void nf_log(const struct nf_config *c, const char *operation, int error) {
    fprintf(stderr, "Network storage: %s (%d: %s)\n", operation, -error, strerror(-error));
    if (c->log_file && *c->log_file) {
        int fd = open(c->log_file, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            dprintf(fd, "Network storage: %s (%d: %s)\n", operation, -error, strerror(-error));
            close(fd);
        }
    }
}
static char *field(struct json_object *root, const char *name) {
    struct json_object *value;
    if (!json_object_object_get_ex(root, name, &value)) return strdup("");
    if (!json_object_is_type(value, json_type_string)) return NULL;
    const char *s = json_object_get_string(value);
    size_t n = (size_t)json_object_get_string_len(value);
    if (n > 65536 || strlen(s) != n) return NULL;
    return strdup(s);
}
int nf_load_config(const char *path, struct nf_config *c) {
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -errno;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 0077) || st.st_uid != geteuid() || st.st_size > 131072) {
        close(fd); return -EACCES;
    }
    struct json_object *root = json_object_from_fd(fd);
    close(fd);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return -EINVAL;
    }
    const char *names[] = {"type","host","user","password","domain","share","path","key_file","key_pass","known_hosts","cache_dir","status_file","log_file"};
    char **targets[] = {&c->type,&c->host,&c->user,&c->password,&c->domain,&c->share,&c->path,&c->key_file,&c->key_pass,&c->known_hosts,&c->cache_dir,&c->status_file,&c->log_file};
    int result = 0;
    for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
        *targets[i] = field(root, names[i]);
        if (!*targets[i]) { result = -EINVAL; break; }
    }
    struct json_object *port;
    c->port = json_object_object_get_ex(root, "port", &port) ? json_object_get_int(port) : 0;
    json_object_put(root);
    if (result) return result;
    if (strcmp(c->type, "smb") && strcmp(c->type, "sftp")) return -EINVAL;
    if (!*c->host || strchr(c->host, '\n') || strchr(c->host, '\r')) return -EINVAL;
    if (!c->port) c->port = !strcmp(c->type, "smb") ? 445 : 22;
    if (c->port < 1 || c->port > 65535) return -EINVAL;
    if (strchr(c->share, '/') || strchr(c->share, '\\')) return -EINVAL;
    char checked[8192];
    return nf_path(c, "/", checked, sizeof(checked));
}
void nf_free_config(struct nf_config *c) {
    char **fields[] = {&c->type,&c->host,&c->user,&c->password,&c->domain,&c->share,&c->path,&c->key_file,&c->key_pass,&c->known_hosts,&c->cache_dir,&c->status_file,&c->log_file};
    for (unsigned i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i) {
        if (*fields[i]) {
            size_t length=strlen(*fields[i]);
            volatile unsigned char *p=(volatile unsigned char *)*fields[i];
            while (length--) *p++=0;
            free(*fields[i]);
        }
    }
    memset(c, 0, sizeof(*c));
}
static int safe_segments(const char *s) {
    while (*s) {
        while (*s == '/') ++s;
        const char *end = strchr(s, '/');
        size_t n = end ? (size_t)(end-s) : strlen(s);
        if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
        s += n;
    }
    return 1;
}
int nf_path(const struct nf_config *c, const char *relative, char *out, size_t length) {
    const char *base = c->path ? c->path : "";
    if (!strcmp(c->type, "smb")) while (*base == '/') ++base;
    if (!safe_segments(base) || !safe_segments(relative)) return -EINVAL;
    while (*relative == '/') ++relative;
    size_t n = strlen(base);
    while (n && base[n-1] == '/') --n;
    int written = snprintf(out, length, "%.*s%s%s", (int)n, base,
                           n && *relative ? "/" : "", relative);
    if (written < 0 || (size_t)written >= length) return -ENAMETOOLONG;
    if (!*out) {
        const char *root = base[0] == '/' ? "/" : (!strcmp(c->type,"sftp") ? "." : "");
        if (strlen(root) >= length) return -ENAMETOOLONG;
        strcpy(out, root);
    }
    return 0;
}
static int list_entry(void *opaque, const char *name, const struct stat *st) {
    if (!S_ISDIR(st->st_mode) || !strcmp(name,".") || !strcmp(name,"..") || strchr(name,'/')) return 0;
    struct json_object *array = opaque;
    if (json_object_array_length(array) >= 32768) return -EOVERFLOW;
    struct json_object *item = json_object_new_object();
    json_object_object_add(item, "Name", json_object_new_string(name));
    json_object_object_add(item, "IsDir", json_object_new_boolean(1));
    json_object_array_add(array, item);
    return 0;
}
int main(int argc, char **argv) {
    const char *command = argc > 1 ? argv[1] : "";
    const char *config_path = NULL, *mountpoint = NULL, *share = NULL, *path = NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i],"--config") && i+1 < argc) config_path = argv[++i];
        else if (!strcmp(argv[i],"--share") && i+1 < argc) share = argv[++i];
        else if (!strcmp(argv[i],"--path") && i+1 < argc) path = argv[++i];
        else if (!mountpoint) mountpoint = argv[i];
        else return 2;
    }
    if (!config_path || (strcmp(command,"mount") && strcmp(command,"list"))) return 2;
    struct nf_config config = {0};
    int result = nf_load_config(config_path, &config);
    if (result) { nf_log(&config,"Invalid private configuration",result); nf_free_config(&config); return 1; }
    if (share) { free(config.share); config.share = strdup(share); }
    if (path) { free(config.path); config.path = strdup(path); }
    if (!strcmp(command,"mount")) result = mountpoint ? nf_mount(&config,mountpoint) : -EINVAL;
    else {
        const struct nf_ops *ops = !strcmp(config.type,"smb") ? &nf_smb_ops : &nf_ssh_ops;
        struct nf_remote remote = {.socket = -1};
        result = ops->connect(&remote,&config, !*config.share && !strcmp(config.type,"smb"));
        if (!result) {
            char target[8192];
            result = nf_path(&config,"/",target,sizeof(target));
            struct json_object *array = json_object_new_array();
            if (!result) result = ops->list(&remote,target,list_entry,array);
            if (!result) puts(json_object_to_json_string_ext(array,JSON_C_TO_STRING_PLAIN));
            json_object_put(array);
        }
        ops->disconnect(&remote);
    }
    if (result) nf_log(&config,"Network storage operation failed",result);
    nf_free_config(&config);
    return result ? 1 : 0;
}
