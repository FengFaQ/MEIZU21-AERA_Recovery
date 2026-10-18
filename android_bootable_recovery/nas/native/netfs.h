// SPDX-License-Identifier: Apache-2.0
#ifndef AERA_NETFS_H
#define AERA_NETFS_H
#define _GNU_SOURCE
#define FUSE_USE_VERSION 35
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <json.h>

#define NF_BLOCK (1024U * 1024U)
#define NF_WINDOW (8U * NF_BLOCK)
struct nf_config {
    char *type, *host, *user, *password, *domain, *share, *path;
    char *key_file, *key_pass, *known_hosts, *cache_dir, *status_file, *log_file;
    int port;
};
struct nf_remote {
    void *smb, *ssh, *sftp;
    int socket, share_listing;
};
struct nf_file;
typedef int (*nf_entry_fn)(void *, const char *, const struct stat *);
struct nf_ops {
    int (*connect)(struct nf_remote *, const struct nf_config *, int);
    void (*disconnect)(struct nf_remote *);
    int (*stat)(struct nf_remote *, const char *, struct stat *, void *);
    int (*list)(struct nf_remote *, const char *, nf_entry_fn, void *);
    int (*open)(struct nf_remote *, const char *, int, mode_t, void **);
    int (*close)(struct nf_remote *, void *);
    int (*read)(struct nf_remote *, void *, void *, size_t, uint64_t);
    int (*write)(struct nf_remote *, void *, const void *, size_t, uint64_t);
    int (*flush)(struct nf_remote *, void *);
    int (*truncate)(struct nf_remote *, const char *, void *, uint64_t);
    int (*mkdir)(struct nf_remote *, const char *, mode_t);
    int (*unlink)(struct nf_remote *, const char *, int);
    int (*rename)(struct nf_remote *, const char *, const char *, unsigned);
    int (*chmod)(struct nf_remote *, const char *, mode_t);
    int (*chown)(struct nf_remote *, const char *, uid_t, gid_t);
    int (*utimens)(struct nf_remote *, const char *, const struct timespec[2]);
    int (*statfs)(struct nf_remote *, const char *, struct statvfs *);
    int (*readlink)(struct nf_remote *, const char *, char *, size_t);
    int (*symlink)(struct nf_remote *, const char *, const char *);
};
extern const struct nf_ops nf_smb_ops, nf_ssh_ops;
int nf_load_config(const char *, struct nf_config *);
void nf_free_config(struct nf_config *);
int nf_path(const struct nf_config *, const char *, char *, size_t);
int nf_mount(struct nf_config *, const char *);
void nf_log(const struct nf_config *, const char *, int);
double nf_now(void);
#endif
