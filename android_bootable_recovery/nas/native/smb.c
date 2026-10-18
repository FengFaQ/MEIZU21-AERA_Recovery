// SPDX-License-Identifier: Apache-2.0
#include "netfs.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-share-enum.h>

static int smb_connect(struct nf_remote *r, const struct nf_config *c, int shares) {
    struct smb2_context *smb = smb2_init_context();
    if (!smb) return -ENOMEM;
    r->smb = smb;
    r->share_listing = shares;
    smb2_set_timeout(smb, 20);
    smb2_set_security_mode(smb, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_user(smb, c->user);
    smb2_set_password(smb, c->password);
    smb2_set_domain(smb, c->domain);
    char host[1024];
    int n = c->port == 445 ? snprintf(host,sizeof(host),"%s",c->host) :
        snprintf(host,sizeof(host),strchr(c->host,':') ? "[%s]:%d" : "%s:%d",c->host,c->port);
    if (n < 0 || (size_t)n >= sizeof(host)) return -ENAMETOOLONG;
    if (!shares && !*c->share) return -EINVAL;
    return smb2_connect_share(smb,host,shares ? "IPC$" : c->share,c->user);
}
static void smb_disconnect(struct nf_remote *r) {
    if (r->smb) { smb2_disconnect_share(r->smb); smb2_destroy_context(r->smb); r->smb = NULL; }
}
static void convert_stat(const struct smb2_stat_64 *s, struct stat *st) {
    memset(st,0,sizeof(*st));
    st->st_mode = s->smb2_type == SMB2_TYPE_DIRECTORY ? S_IFDIR | 0777 :
                  s->smb2_type == SMB2_TYPE_LINK ? S_IFLNK | 0777 : S_IFREG | 0666;
    st->st_nlink = s->smb2_nlink ? s->smb2_nlink : 1;
    st->st_ino = s->smb2_ino;
    st->st_size = s->smb2_size;
    st->st_blksize = NF_BLOCK;
    st->st_blocks = (s->smb2_size+511)/512;
    st->st_atim = (struct timespec){s->smb2_atime,s->smb2_atime_nsec};
    st->st_mtim = (struct timespec){s->smb2_mtime,s->smb2_mtime_nsec};
    st->st_ctim = (struct timespec){s->smb2_ctime,s->smb2_ctime_nsec};
}
static int smb_stat(struct nf_remote *r, const char *p, struct stat *st, void *fh) {
    struct smb2_stat_64 s;
    int rc = fh ? smb2_fstat(r->smb,fh,&s) : smb2_stat(r->smb,p,&s);
    if (!rc) convert_stat(&s,st);
    return rc;
}
static int smb_list(struct nf_remote *r, const char *p, nf_entry_fn emit, void *data) {
    if (r->share_listing) {
        struct smb2_share_enum_reply *reply = smb2_share_enum_sync(r->smb,SMB2_SHARE_INFO_1);
        if (!reply) return -EIO;
        int rc = 0;
        struct stat st = {.st_mode=S_IFDIR|0777,.st_nlink=2};
        for (unsigned i = 0; i < reply->entries_read && !rc; ++i) {
            struct smb2_share_info_1 *s = &reply->share_info.info_1[i];
            if ((s->type & 3) == SMB2_SHARE_TYPE_DISKTREE) rc = emit(data,s->netname,&st);
        }
        smb2_free_data(r->smb,reply);
        return rc;
    }
    struct smb2dir *dir = smb2_opendir(r->smb,p);
    if (!dir) return -EIO;
    struct smb2dirent *entry;
    int rc = 0;
    while (!rc && (entry=smb2_readdir(r->smb,dir))) {
        struct stat st;
        convert_stat(&entry->st,&st);
        rc = emit(data,entry->name,&st);
    }
    smb2_closedir(r->smb,dir);
    return rc;
}
static int smb_open(struct nf_remote *r, const char *p, int flags, mode_t mode, void **fh) {
    (void)mode;
    /* Attribute queries must work on write handles as well. */
    if ((flags & O_ACCMODE) == O_WRONLY) flags = (flags & ~O_ACCMODE) | O_RDWR;
    *fh = smb2_open(r->smb,p,flags & (O_ACCMODE|O_CREAT|O_EXCL|O_TRUNC));
    if (!*fh) return smb2_get_nterror(r->smb) ? -EIO : -EACCES;
    return 0;
}
struct batch;
struct part { struct batch *batch; unsigned char *data; uint64_t offset; unsigned length,done; int finished; };
struct batch { struct smb2_context *smb; struct smb2fh *fh; int write,error,active; };
static void batch_done(struct smb2_context *,int,void *,void *);
static int submit(struct part *p) {
    struct batch *b=p->batch;
    return b->write ? smb2_pwrite_async(b->smb,b->fh,p->data+p->done,p->length-p->done,p->offset+p->done,batch_done,p) :
                     smb2_pread_async(b->smb,b->fh,p->data+p->done,p->length-p->done,p->offset+p->done,batch_done,p);
}
static void batch_done(struct smb2_context *smb,int status,void *command,void *opaque) {
    (void)smb; (void)command;
    struct part *p=opaque;
    struct batch *b=p->batch;
    if (status < 0 || (unsigned)status > p->length-p->done || (!status && b->write)) {
        b->error = status < 0 ? status : -EIO;
    } else p->done += status;
    if (!b->error && status && p->done < p->length) {
        int rc=submit(p);
        if (!rc) return;
        b->error=rc;
    }
    p->finished=1;
    --b->active;
}
static int smb_io(struct nf_remote *r, void *fh, void *buffer, size_t size, uint64_t offset,int writing) {
    if (!r->smb) return -ENOTCONN;
    if (!size) return 0;
    struct batch b={.smb=r->smb,.fh=fh,.write=writing};
    struct part parts[16]={0};
    size_t block=writing ? smb2_get_max_write_size(r->smb) : smb2_get_max_read_size(r->smb);
    if (block > NF_BLOCK) block=NF_BLOCK;
    if (!block) return -EINVAL;
    if (size > 16*block) size=16*block;
    unsigned n=0;
    for (size_t at=0; at<size; at+=block) {
        struct part *p=&parts[n++];
        *p=(struct part){.batch=&b,.data=(unsigned char *)buffer+at,.offset=offset+at,
            .length=(unsigned)(size-at < block ? size-at : block)};
        ++b.active;
        int rc=submit(p);
        if (rc) { --b.active; b.error=rc; break; }
    }
    double start=nf_now();
    while (b.active) {
        struct pollfd pollfd={smb2_get_fd(r->smb),smb2_which_events(r->smb),0};
        int rc=poll(&pollfd,1,500);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 || smb2_service(r->smb,rc ? pollfd.revents : 0) < 0 || nf_now()-start > 30) {
            /* Cancellation callbacks run before stack request buffers disappear. */
            smb2_destroy_context(r->smb);
            r->smb=NULL;
            return -EIO;
        }
    }
    if (b.error) return b.error;
    int total=0;
    for (unsigned i=0; i<n; ++i) {
        total+=parts[i].done;
        if (parts[i].done < parts[i].length) break;
    }
    return total;
}
static int smb_transfer(struct nf_remote *r,void *fh,void *b,size_t n,uint64_t o,int writing) {
    size_t done=0;
    while (done<n) {
        int rc=smb_io(r,fh,(unsigned char *)b+done,n-done,o+done,writing);
        if (rc<0) return rc;
        if (!rc) return writing ? -EIO : (int)done;
        done+=(size_t)rc;
    }
    return (int)done;
}
static int smb_read(struct nf_remote *r,void *fh,void *b,size_t n,uint64_t o) { return smb_transfer(r,fh,b,n,o,0); }
static int smb_write(struct nf_remote *r,void *fh,const void *b,size_t n,uint64_t o) { return smb_transfer(r,fh,(void *)b,n,o,1); }
static int smb_close(struct nf_remote *r,void *fh) { return r->smb ? smb2_close(r->smb,fh) : -ENOTCONN; }
static int smb_flush(struct nf_remote *r,void *fh) { return r->smb ? smb2_fsync(r->smb,fh) : -ENOTCONN; }
static int smb_truncate(struct nf_remote *r,const char *p,void *fh,uint64_t size) { return fh ? smb2_ftruncate(r->smb,fh,size) : smb2_truncate(r->smb,p,size); }
static int smb_mkdir(struct nf_remote *r,const char *p,mode_t mode) { (void)mode; return smb2_mkdir(r->smb,p); }
static int smb_unlink(struct nf_remote *r,const char *p,int dir) { return dir ? smb2_rmdir(r->smb,p) : smb2_unlink(r->smb,p); }
static int smb_rename(struct nf_remote *r,const char *from,const char *to,unsigned flags) {
    if (flags) return -EOPNOTSUPP;
    return smb2_rename(r->smb,from,to);
}
/* SMB shares expose synthetic Unix ownership, as the old uid/gid mount did. */
static int smb_chmod(struct nf_remote *r,const char *p,mode_t m) { (void)r;(void)p;(void)m;return 0; }
static int smb_chown(struct nf_remote *r,const char *p,uid_t u,gid_t g) { (void)r;(void)p;(void)u;(void)g;return 0; }
struct basic_update { int error,done; };
static void basic_step(struct smb2_context *smb,int status,void *reply,void *opaque) {
    (void)smb;(void)reply;
    struct basic_update *update=opaque;
    if (status && !update->error) update->error=-nterror_to_errno((uint32_t)status);
}
static void basic_done(struct smb2_context *smb,int status,void *reply,void *opaque) {
    basic_step(smb,status,reply,opaque);
    ((struct basic_update *)opaque)->done=1;
}
static int smb_utimens(struct nf_remote *r,const char *p,const struct timespec t[2]) {
    struct smb2_stat_64 old;
    int rc=smb2_stat(r->smb,p,&old);
    if (rc) return rc;
    time_t current=time(NULL);
    struct smb2_file_basic_info basic={0};
    basic.creation_time.tv_sec=old.smb2_btime;
    basic.creation_time.tv_usec=old.smb2_btime_nsec/1000;
    basic.change_time.tv_sec=old.smb2_ctime;
    basic.change_time.tv_usec=old.smb2_ctime_nsec/1000;
    basic.last_access_time.tv_sec=t[0].tv_nsec==UTIME_OMIT ? old.smb2_atime : t[0].tv_nsec==UTIME_NOW ? current : t[0].tv_sec;
    basic.last_access_time.tv_usec=t[0].tv_nsec==UTIME_OMIT ? old.smb2_atime_nsec/1000 : t[0].tv_nsec==UTIME_NOW ? 0 : t[0].tv_nsec/1000;
    basic.last_write_time.tv_sec=t[1].tv_nsec==UTIME_OMIT ? old.smb2_mtime : t[1].tv_nsec==UTIME_NOW ? current : t[1].tv_sec;
    basic.last_write_time.tv_usec=t[1].tv_nsec==UTIME_OMIT ? old.smb2_mtime_nsec/1000 : t[1].tv_nsec==UTIME_NOW ? 0 : t[1].tv_nsec/1000;
    basic.file_attributes=old.smb2_attributes;
    struct smb2_create_request create={0};
    create.impersonation_level=SMB2_IMPERSONATION_IMPERSONATION;
    create.desired_access=SMB2_FILE_WRITE_ATTRIBUTES;
    create.share_access=SMB2_FILE_SHARE_READ|SMB2_FILE_SHARE_WRITE|SMB2_FILE_SHARE_DELETE;
    create.create_disposition=SMB2_FILE_OPEN;
    create.name=p;
    struct basic_update update={0};
    struct smb2_pdu *first=smb2_cmd_create_async(r->smb,&create,basic_step,&update);
    if (!first) return -ENOMEM;
    struct smb2_set_info_request set={0};
    set.info_type=SMB2_0_INFO_FILE;set.file_info_class=SMB2_FILE_BASIC_INFORMATION;
    set.input_data=&basic;
    memcpy(set.file_id,compound_file_id,SMB2_FD_SIZE);
    struct smb2_pdu *next=smb2_cmd_set_info_async(r->smb,&set,basic_step,&update);
    if (!next) {smb2_free_pdu(r->smb,first);return -ENOMEM;}
    smb2_add_compound_pdu(r->smb,first,next);
    struct smb2_close_request close={0};
    memcpy(close.file_id,compound_file_id,SMB2_FD_SIZE);
    next=smb2_cmd_close_async(r->smb,&close,basic_done,&update);
    if (!next) {smb2_free_pdu(r->smb,first);return -ENOMEM;}
    smb2_add_compound_pdu(r->smb,first,next);
    smb2_queue_pdu(r->smb,first);
    double start=nf_now();
    while (!update.done) {
        struct pollfd fd={smb2_get_fd(r->smb),smb2_which_events(r->smb),0};
        int n=poll(&fd,1,500);
        if (n<0 && errno==EINTR) continue;
        if (n<0 || smb2_service(r->smb,n ? fd.revents : 0)<0 || nf_now()-start>30) {
            smb2_destroy_context(r->smb);r->smb=NULL;return -EIO;
        }
    }
    return update.error;
}
static int smb_statfs(struct nf_remote *r,const char *p,struct statvfs *out) {
    struct smb2_statvfs s;
    int rc=smb2_statvfs(r->smb,p,&s);
    if (!rc) {
        memset(out,0,sizeof(*out));
        out->f_bsize=s.f_bsize; out->f_frsize=s.f_frsize; out->f_blocks=s.f_blocks;
        out->f_bfree=s.f_bfree; out->f_bavail=s.f_bavail; out->f_files=s.f_files;
        out->f_ffree=s.f_ffree; out->f_favail=s.f_favail; out->f_namemax=s.f_namemax;
    }
    return rc;
}
static int smb_readlink(struct nf_remote *r,const char *p,char *buf,size_t n) { return smb2_readlink(r->smb,p,buf,(uint32_t)n); }
static int smb_symlink(struct nf_remote *r,const char *target,const char *p) { return smb2_symlink(r->smb,target,p,0); }
const struct nf_ops nf_smb_ops={smb_connect,smb_disconnect,smb_stat,smb_list,smb_open,smb_close,
    smb_read,smb_write,smb_flush,smb_truncate,smb_mkdir,smb_unlink,smb_rename,smb_chmod,smb_chown,
    smb_utimens,smb_statfs,smb_readlink,smb_symlink};
