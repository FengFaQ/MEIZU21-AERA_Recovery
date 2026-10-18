// SPDX-License-Identifier: Apache-2.0
#include "netfs.h"
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

struct nf_file {
    struct nf_file *next;
    void *remote;
    char *path;
    int flags, cache_fd, dirty, error;
    char cache_path[PATH_MAX], manifest[PATH_MAX];
    unsigned char *write_buffer, *read_buffer;
    size_t write_count, read_count;
    uint64_t write_offset, read_offset, size;
};
struct filesystem {
    struct nf_config *config;
    struct nf_remote remote;
    const struct nf_ops *ops;
    struct nf_file *files;
    uint64_t written, read, write_errors, retained, retained_files;
    size_t memory;
    double status_time;
    char status_temp[PATH_MAX];
} fs;

static int healthy(void) {
    return (!strcmp(fs.config->type,"smb") ? fs.remote.smb!=NULL : fs.remote.sftp!=NULL) ? 0 : -ENOTCONN;
}
static void status_update(int force) {
    if (!*fs.config->status_file || (!force && nf_now()-fs.status_time<0.25)) return;
    fs.status_time=nf_now();
    uint64_t pending=fs.retained,files=fs.retained_files;
    for (struct nf_file *f=fs.files;f;f=f->next) {
        if (f->dirty) { pending+=f->cache_fd>=0 ? f->size : f->write_count; ++files; }
    }
    struct json_object *root=json_object_new_object();
    json_object_object_add(root,"version",json_object_new_int(1));
    json_object_object_add(root,"pid",json_object_new_int(getpid()));
    json_object_object_add(root,"written",json_object_new_uint64(fs.written));
    json_object_object_add(root,"read",json_object_new_uint64(fs.read));
    json_object_object_add(root,"pending_bytes",json_object_new_uint64(pending));
    json_object_object_add(root,"pending_files",json_object_new_uint64(files));
    json_object_object_add(root,"retained_files",json_object_new_uint64(fs.retained_files));
    json_object_object_add(root,"write_errors",json_object_new_uint64(fs.write_errors));
    json_object_object_add(root,"connected",json_object_new_boolean(healthy()==0));
    json_object_object_add(root,"disk_cache",json_object_new_boolean(*fs.config->cache_dir!=0));
    const char *data=json_object_to_json_string_ext(root,JSON_C_TO_STRING_PLAIN);
    int fd=open(fs.status_temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if (fd>=0) {
        size_t size=strlen(data),done=0;
        while (done<size) {
            ssize_t n=write(fd,data+done,size-done);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) break;
            done+=(size_t)n;
        }
        if (close(fd)==0 && done==size) rename(fs.status_temp,fs.config->status_file);
        else unlink(fs.status_temp);
    }
    json_object_put(root);
}
static int file_error(struct nf_file *f,int rc) {
    if (rc>=0) rc=-EIO;
    if (!f->error) {
        f->error=rc;
        ++fs.write_errors;
        nf_log(fs.config,"Remote file write/flush failed",rc);
    }
    status_update(1);
    return f->error;
}
static unsigned char *allocate_buffer(void) {
    /* File sessions are serialized, so idle readahead buffers can be evicted. */
    for (struct nf_file *f=fs.files;fs.memory+NF_WINDOW>64U*NF_BLOCK && f;f=f->next) {
        if (f->read_buffer) {
            free(f->read_buffer);f->read_buffer=NULL;f->read_count=0;fs.memory-=NF_WINDOW;
        }
        if (fs.memory+NF_WINDOW>64U*NF_BLOCK && f->write_buffer && !f->write_count) {
            free(f->write_buffer);f->write_buffer=NULL;fs.memory-=NF_WINDOW;
        }
    }
    if (fs.memory+NF_WINDOW>64U*NF_BLOCK) return NULL;
    unsigned char *buffer=malloc(NF_WINDOW);
    if (buffer) fs.memory+=NF_WINDOW;
    return buffer;
}
static void free_buffer(unsigned char **buffer) {
    if (*buffer) { free(*buffer);*buffer=NULL;fs.memory-=NF_WINDOW; }
}
static int drain(struct nf_file *f) {
    if (f->error) return f->error;
    if (!f->write_count) return 0;
    int rc=healthy();
    if (!rc) rc=fs.ops->write(&fs.remote,f->remote,f->write_buffer,f->write_count,f->write_offset);
    if (rc<0 || (size_t)rc!=f->write_count) return file_error(f,rc);
    fs.written+=f->write_count;
    f->write_count=0;
    status_update(0);
    return 0;
}
static int flush_file(struct nf_file *f) {
    if (f->error) return f->error;
    if (!f->dirty) return 0;
    if (healthy()) return file_error(f,-ENOTCONN);
    int rc=0;
    if (f->cache_fd>=0) {
        unsigned char *buffer=allocate_buffer();
        if (!buffer) return file_error(f,-ENOMEM);
        if (fsync(f->cache_fd)) rc=-errno;
        if (!rc) rc=fs.ops->truncate(&fs.remote,f->path,f->remote,0);
        for (uint64_t offset=0;!rc && offset<f->size;) {
            size_t count=f->size-offset>NF_WINDOW ? NF_WINDOW : (size_t)(f->size-offset);
            ssize_t n=pread(f->cache_fd,buffer,count,(off_t)offset);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) { rc=-EIO;break; }
            int sent=fs.ops->write(&fs.remote,f->remote,buffer,(size_t)n,offset);
            if (sent!=n) { rc=sent<0 ? sent : -EIO;break; }
            offset+=(uint64_t)n;
            fs.written+=(uint64_t)n;
            status_update(0);
        }
        free_buffer(&buffer);
    } else rc=drain(f);
    if (!rc) rc=fs.ops->flush(&fs.remote,f->remote);
    if (!rc) {
        struct stat st;
        rc=fs.ops->stat(&fs.remote,f->path,&st,f->remote);
        if (!rc && (uint64_t)st.st_size!=f->size) rc=-EIO;
    }
    if (rc) return file_error(f,rc);
    f->dirty=0;
    free_buffer(&f->write_buffer);
    status_update(1);
    return 0;
}
static int cache_open(struct nf_file *f) {
    if (snprintf(f->cache_path,sizeof(f->cache_path),"%s/.aera-netfs-XXXXXX",fs.config->cache_dir)>=(int)sizeof(f->cache_path)) return -ENAMETOOLONG;
    f->cache_fd=mkstemp(f->cache_path);
    if (f->cache_fd<0) return -errno;
    fcntl(f->cache_fd,F_SETFD,FD_CLOEXEC);
    if (snprintf(f->manifest,sizeof(f->manifest),"%s.json",f->cache_path)>=(int)sizeof(f->manifest)) return -ENAMETOOLONG;
    struct json_object *root=json_object_new_object();
    json_object_object_add(root,"version",json_object_new_int(1));
    json_object_object_add(root,"remote_path",json_object_new_string(f->path));
    json_object_object_add(root,"host",json_object_new_string(fs.config->host));
    json_object_object_add(root,"share",json_object_new_string(fs.config->share));
    json_object_object_add(root,"type",json_object_new_string(fs.config->type));
    int fd=open(f->manifest,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    int rc=0;
    const char *json=json_object_to_json_string_ext(root,JSON_C_TO_STRING_PLAIN);
    if (fd<0) rc=-errno;
    else {
        size_t n=strlen(json);
        if (write(fd,json,n)!=(ssize_t)n || fsync(fd)) rc=-EIO;
        close(fd);
    }
    json_object_put(root);
    if (rc || !f->size) return rc;
    if (f->size>25ULL*1024*NF_BLOCK) return -EFBIG;
    void *reader=NULL;
    rc=fs.ops->open(&fs.remote,f->path,O_RDONLY,0,&reader);
    if (rc) return rc;
    unsigned char *buffer=allocate_buffer();
    if (!buffer) rc=-ENOMEM;
    for (uint64_t offset=0;!rc && offset<f->size;) {
        size_t count=f->size-offset>NF_WINDOW ? NF_WINDOW : (size_t)(f->size-offset);
        int n=fs.ops->read(&fs.remote,reader,buffer,count,offset);
        if (n<=0 || pwrite(f->cache_fd,buffer,(size_t)n,(off_t)offset)!=n) { rc=n<0 ? n : -EIO;break; }
        offset+=(uint64_t)n;
    }
    free_buffer(&buffer);
    int closed=fs.ops->close(&fs.remote,reader);
    return rc ? rc : closed;
}
static int open_file(const char *path,mode_t mode,struct fuse_file_info *info,int create) {
    if (healthy()) return -ENOTCONN;
    struct nf_file *f=calloc(1,sizeof(*f));
    if (!f) return -ENOMEM;
    f->cache_fd=-1;
    char remote[8192]={0};
    int rc=nf_path(fs.config,path,remote,sizeof(remote));
    f->path=strdup(remote);
    f->flags=info->flags|(create ? O_CREAT : 0);
    if (!f->path) rc=-ENOMEM;
    if (!rc) rc=fs.ops->open(&fs.remote,f->path,f->flags,mode,&f->remote);
    if (!rc) {
        struct stat st;
        rc=fs.ops->stat(&fs.remote,f->path,&st,f->remote);
        if (!rc) f->size=st.st_size;
    }
    int writable=(f->flags&O_ACCMODE)!=O_RDONLY;
    if (!rc && writable && *fs.config->cache_dir) rc=cache_open(f);
    if (rc) {
        if (f->remote) fs.ops->close(&fs.remote,f->remote);
        if (f->cache_fd>=0) close(f->cache_fd);
        if (*f->cache_path) unlink(f->cache_path);
        if (*f->manifest) unlink(f->manifest);
        free(f->path);free(f);return rc;
    }
    f->dirty=writable && (f->flags&(O_CREAT|O_TRUNC));
    f->next=fs.files;fs.files=f;
    info->fh=(uint64_t)(uintptr_t)f;
    info->direct_io=1;
    info->keep_cache=0;
    status_update(1);
    return 0;
}
static int nf_open(const char *p,struct fuse_file_info *f) { return open_file(p,0666,f,0); }
static int nf_create(const char *p,mode_t mode,struct fuse_file_info *f) { return open_file(p,mode,f,1); }
static int nf_getattr(const char *p,struct stat *st,struct fuse_file_info *info) {
    if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));
    struct nf_file *f=info ? (void *)(uintptr_t)info->fh : NULL;
    if (!rc) rc=fs.ops->stat(&fs.remote,path,st,f ? f->remote : NULL);
    if (!rc) for (struct nf_file *open=fs.files;open;open=open->next) {
        if (!strcmp(open->path,path) && open->dirty) { st->st_size=open->size;st->st_blocks=(open->size+511)/512;break; }
    }
    return rc;
}
struct listing { void *buffer;fuse_fill_dir_t fill;off_t skip,index;int full;enum fuse_fill_dir_flags flags; };
static int emit(void *opaque,const char *name,const struct stat *st) {
    struct listing *l=opaque;
    ++l->index;
    if (l->index<=l->skip || l->full) return 0;
    l->full=l->fill(l->buffer,name,st,l->index,l->flags)!=0;
    return 0;
}
static int nf_readdir(const char *p,void *buffer,fuse_fill_dir_t fill,off_t offset,struct fuse_file_info *info,enum fuse_readdir_flags flags) {
    (void)info;
    if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));
    struct listing l={.buffer=buffer,.fill=fill,.skip=offset,
        .flags=(flags&FUSE_READDIR_PLUS) ? FUSE_FILL_DIR_PLUS : 0};
    return rc ? rc : fs.ops->list(&fs.remote,path,emit,&l);
}
static int nf_read(const char *p,char *out,size_t count,off_t offset,struct fuse_file_info *info) {
    (void)p;
    struct nf_file *f=(void *)(uintptr_t)info->fh;
    if (offset<0) return -EINVAL;
    if (f->error) return f->error;
    if (f->cache_fd>=0) {
        ssize_t n=pread(f->cache_fd,out,count,offset);
        return n<0 ? -errno : (int)n;
    }
    int rc=flush_file(f);
    if (rc || healthy()) return rc ? rc : -ENOTCONN;
    if ((uint64_t)offset>=f->size) return 0;
    if (count>f->size-(uint64_t)offset) count=(size_t)(f->size-offset);
    if (!f->read_buffer && !(f->read_buffer=allocate_buffer())) return -ENOMEM;
    size_t done=0;
    while (done<count) {
        uint64_t at=(uint64_t)offset+done;
        if (!(at>=f->read_offset && at<f->read_offset+f->read_count)) {
            /* File browsers sniff a few KB; reserve deep readahead for streams. */
            size_t window=(count>=128U*1024 ||
                (f->read_count && at==f->read_offset+f->read_count)) ? NF_WINDOW : 32U*1024;
            size_t n=f->size-at>window ? window : (size_t)(f->size-at);
            rc=fs.ops->read(&fs.remote,f->remote,f->read_buffer,n,at);
            if (rc<=0) return done ? (int)done : rc;
            f->read_offset=at;f->read_count=rc;
            fs.read+=(uint64_t)rc;status_update(0);
        }
        size_t available=(size_t)(f->read_offset+f->read_count-at);
        size_t n=count-done<available ? count-done : available;
        memcpy(out+done,f->read_buffer+(at-f->read_offset),n);
        done+=n;
    }
    return (int)done;
}
static int nf_write(const char *p,const char *input,size_t count,off_t offset,struct fuse_file_info *info) {
    (void)p;
    struct nf_file *f=(void *)(uintptr_t)info->fh;
    if (offset<0 || count>INT_MAX || (uint64_t)offset>INT64_MAX-count) return -EINVAL;
    if (f->error) return f->error;
    f->read_count=0;
    if (f->cache_fd>=0) {
        struct statvfs free_space;
        if ((uint64_t)offset+count>25ULL*1024*NF_BLOCK || fstatvfs(f->cache_fd,&free_space) ||
            (uint64_t)free_space.f_bavail*free_space.f_frsize<1024ULL*NF_BLOCK+count) return file_error(f,-ENOSPC);
        ssize_t n=pwrite(f->cache_fd,input,count,offset);
        if (n<0) return file_error(f,-errno);
        if ((uint64_t)offset+(uint64_t)n>f->size) f->size=(uint64_t)offset+n;
        f->dirty=1;status_update(0);return (int)n;
    }
    if (!f->write_buffer && !(f->write_buffer=allocate_buffer())) return -ENOMEM;
    if (f->write_count && (uint64_t)offset!=f->write_offset+f->write_count) {
        int rc=drain(f);if (rc) return rc;
    }
    size_t done=0;
    while (done<count) {
        if (!f->write_count) f->write_offset=(uint64_t)offset+done;
        size_t available=NF_WINDOW-f->write_count;
        size_t n=count-done<available ? count-done : available;
        memcpy(f->write_buffer+f->write_count,input+done,n);
        f->write_count+=n;done+=n;f->dirty=1;
        if ((uint64_t)offset+done>f->size) f->size=(uint64_t)offset+done;
        status_update(0);
        if (f->write_count==NF_WINDOW) { int rc=drain(f);if (rc) return rc; }
    }
    return (int)done;
}
static int nf_flush(const char *p,struct fuse_file_info *info) { (void)p;return flush_file((void *)(uintptr_t)info->fh); }
static int nf_fsync(const char *p,int data,struct fuse_file_info *info) { (void)data;return nf_flush(p,info); }
static int nf_release(const char *p,struct fuse_file_info *info) {
    (void)p;struct nf_file *f=(void *)(uintptr_t)info->fh;
    int rc=flush_file(f);
    int closed=healthy() ? -ENOTCONN : fs.ops->close(&fs.remote,f->remote);
    if (!rc && closed && (f->flags&O_ACCMODE)!=O_RDONLY) rc=file_error(f,closed);
    if (f->cache_fd>=0) {
        close(f->cache_fd);
        if (!rc) { unlink(f->cache_path);unlink(f->manifest); }
        else { fs.retained+=f->size;++fs.retained_files; }
    }
    struct nf_file **link=&fs.files;
    while (*link && *link!=f) link=&(*link)->next;
    if (*link) *link=f->next;
    free_buffer(&f->write_buffer);free_buffer(&f->read_buffer);
    free(f->path);free(f);status_update(1);
    return rc;
}
static int flush_path(const char *path,int descendants) {
    for (struct nf_file *f=fs.files;f;f=f->next) {
        if (strcmp(f->path,path) && (!descendants || strncmp(f->path,path,strlen(path)) || f->path[strlen(path)]!='/')) continue;
        int rc=flush_file(f);if (rc) return rc;
    }
    return healthy();
}
static int nf_truncate(const char *p,off_t size,struct fuse_file_info *info) {
    if (size<0) return -EINVAL;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));if (rc) return rc;
    rc=flush_path(path,0);if (rc) return rc;
    struct nf_file *f=info ? (void *)(uintptr_t)info->fh : NULL;
    rc=fs.ops->truncate(&fs.remote,path,f ? f->remote : NULL,(uint64_t)size);
    if (!rc) for (f=fs.files;f;f=f->next) if (!strcmp(f->path,path)) {
        if (f->cache_fd>=0 && ftruncate(f->cache_fd,size)) return file_error(f,-errno);
        f->size=(uint64_t)size;f->read_count=0;
    }
    return rc;
}
static int nf_mkdir(const char *p,mode_t mode) {
    if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));
    return rc ? rc : fs.ops->mkdir(&fs.remote,path,mode);
}
static int remove_path(const char *p,int dir) {
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));
    if (!rc) rc=flush_path(path,dir);
    return rc ? rc : fs.ops->unlink(&fs.remote,path,dir);
}
static int nf_unlink(const char *p) { return remove_path(p,0); }
static int nf_rmdir(const char *p) { return remove_path(p,1); }
static int nf_rename(const char *a,const char *b,unsigned flags) {
    int rc=healthy();if (rc) return rc;
    char from[8192],to[8192];
    if ((rc=nf_path(fs.config,a,from,sizeof(from))) || (rc=nf_path(fs.config,b,to,sizeof(to)))) return rc;
    /* An open staging handle must not be detached from its recovery manifest. */
    for (struct nf_file *f=fs.files;f;f=f->next)
        if (!strcmp(f->path,from) || (!strncmp(f->path,from,strlen(from)) && f->path[strlen(from)]=='/')) return -EBUSY;
    return fs.ops->rename(&fs.remote,from,to,flags);
}
static int nf_chmod(const char *p,mode_t mode,struct fuse_file_info *info) {
    (void)info;if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));return rc ? rc : fs.ops->chmod(&fs.remote,path,mode);
}
static int nf_chown(const char *p,uid_t uid,gid_t gid,struct fuse_file_info *info) {
    (void)info;if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));return rc ? rc : fs.ops->chown(&fs.remote,path,uid,gid);
}
static int nf_utimens(const char *p,const struct timespec times[2],struct fuse_file_info *info) {
    (void)info;char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));
    if (!rc) rc=flush_path(path,0);
    return rc ? rc : fs.ops->utimens(&fs.remote,path,times);
}
static int nf_statfs(const char *p,struct statvfs *st) {
    (void)p;if (healthy()) return -ENOTCONN;
    char root[8192];int rc=nf_path(fs.config,"/",root,sizeof(root));
    return rc ? rc : fs.ops->statfs(&fs.remote,root,st);
}
static int nf_readlink(const char *p,char *buffer,size_t n) {
    if (!n) return -EINVAL;
    if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));return rc ? rc : fs.ops->readlink(&fs.remote,path,buffer,n);
}
static int nf_symlink(const char *target,const char *p) {
    if (healthy()) return -ENOTCONN;
    char path[8192];int rc=nf_path(fs.config,p,path,sizeof(path));return rc ? rc : fs.ops->symlink(&fs.remote,target,path);
}
static int nf_listxattr(const char *p,char *b,size_t n) { (void)p;(void)b;(void)n;return 0; }
static void *nf_init(struct fuse_conn_info *conn,struct fuse_config *config) {
    config->entry_timeout=0;config->attr_timeout=1;config->negative_timeout=0;
    config->nullpath_ok=0;
    conn->max_write=NF_BLOCK;conn->max_read=NF_BLOCK;conn->max_readahead=NF_WINDOW;
    /* Userspace owns buffering; kernel writeback must not hide remote errors. */
    conn->want &= ~(FUSE_CAP_WRITEBACK_CACHE|FUSE_CAP_ASYNC_READ);
    conn->want |= conn->capable & FUSE_CAP_READDIRPLUS;
    status_update(1);
    return NULL;
}
static const struct fuse_operations operations={
    .init=nf_init,.getattr=nf_getattr,.readdir=nf_readdir,.open=nf_open,.create=nf_create,
    .read=nf_read,.write=nf_write,.flush=nf_flush,.fsync=nf_fsync,.release=nf_release,
    .truncate=nf_truncate,.mkdir=nf_mkdir,.unlink=nf_unlink,.rmdir=nf_rmdir,.rename=nf_rename,
    .chmod=nf_chmod,.chown=nf_chown,.utimens=nf_utimens,.statfs=nf_statfs,
    .readlink=nf_readlink,.symlink=nf_symlink,.listxattr=nf_listxattr
};
int nf_mount(struct nf_config *config,const char *mountpoint) {
    memset(&fs,0,sizeof(fs));fs.config=config;fs.remote.socket=-1;
    fs.ops=!strcmp(config->type,"smb") ? &nf_smb_ops : &nf_ssh_ops;
    if (*config->status_file && snprintf(fs.status_temp,sizeof(fs.status_temp),"%s.%d.tmp",config->status_file,getpid())>=(int)sizeof(fs.status_temp)) return -ENAMETOOLONG;
    int rc=fs.ops->connect(&fs.remote,config,0);
    if (rc) { fs.ops->disconnect(&fs.remote);return rc; }
    const char *arguments[]={"aera-netfs",mountpoint,"-f","-s","-o","fsname=aera-nas,subtype=aera-netfs,allow_other,default_permissions,max_read=1048576"};
    rc=fuse_main((int)(sizeof(arguments)/sizeof(arguments[0])),(char **)arguments,&operations,NULL);
    /* Never remove disk-cache files here: unclean exits may need recovery. */
    fs.ops->disconnect(&fs.remote);
    unlink(config->status_file);
    if (*fs.status_temp) unlink(fs.status_temp);
    return rc ? -EIO : 0;
}
