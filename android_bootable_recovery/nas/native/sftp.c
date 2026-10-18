// SPDX-License-Identifier: Apache-2.0
#include "netfs.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <libssh2.h>
#include <libssh2_sftp.h>

static int ssh_error(struct nf_remote *r) {
    if (r->sftp && libssh2_session_last_errno(r->ssh)==LIBSSH2_ERROR_SFTP_PROTOCOL) {
        switch (libssh2_sftp_last_error(r->sftp)) {
            case LIBSSH2_FX_NO_SUCH_FILE: case LIBSSH2_FX_NO_SUCH_PATH: return -ENOENT;
            case LIBSSH2_FX_PERMISSION_DENIED: return -EACCES;
            case LIBSSH2_FX_FILE_ALREADY_EXISTS: return -EEXIST;
            case LIBSSH2_FX_OP_UNSUPPORTED: return -EOPNOTSUPP;
            case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM: return -ENOSPC;
            case LIBSSH2_FX_WRITE_PROTECT: return -EROFS;
        }
    }
    return -EIO;
}
static int known_host(struct nf_remote *r,const struct nf_config *c) {
    if (!*c->known_hosts) return 0;
    LIBSSH2_KNOWNHOSTS *hosts=libssh2_knownhost_init(r->ssh);
    if (!hosts) return -ENOMEM;
    int kind=0,mask=LIBSSH2_KNOWNHOST_TYPE_PLAIN|LIBSSH2_KNOWNHOST_KEYENC_RAW;
    size_t n=0;
    const char *key=libssh2_session_hostkey(r->ssh,&n,&kind);
    switch (kind) {
        case LIBSSH2_HOSTKEY_TYPE_RSA: mask|=LIBSSH2_KNOWNHOST_KEY_SSHRSA;break;
        case LIBSSH2_HOSTKEY_TYPE_ED25519: mask|=LIBSSH2_KNOWNHOST_KEY_ED25519;break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: mask|=LIBSSH2_KNOWNHOST_KEY_ECDSA_256;break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: mask|=LIBSSH2_KNOWNHOST_KEY_ECDSA_384;break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: mask|=LIBSSH2_KNOWNHOST_KEY_ECDSA_521;break;
        default: libssh2_knownhost_free(hosts);return -EACCES;
    }
    int loaded=libssh2_knownhost_readfile(hosts,c->known_hosts,LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    int match=key && loaded>=0 && libssh2_knownhost_checkp(hosts,c->host,c->port,key,n,mask,NULL)==LIBSSH2_KNOWNHOST_CHECK_MATCH;
    libssh2_knownhost_free(hosts);
    return match ? 0 : -EACCES;
}
static int ssh_connect(struct nf_remote *r,const struct nf_config *c,int shares) {
    (void)shares;
    if (libssh2_init(0)) return -EIO;
    char port[8];snprintf(port,sizeof(port),"%d",c->port);
    struct addrinfo hint={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM},*list=NULL;
    if (getaddrinfo(c->host,port,&hint,&list)) return -EHOSTUNREACH;
    r->socket=-1;
    for (struct addrinfo *a=list;a;a=a->ai_next) {
        int fd=socket(a->ai_family,a->ai_socktype|SOCK_CLOEXEC,a->ai_protocol);
        if (fd<0) continue;
        struct timeval timeout={.tv_sec=20};int yes=1;
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
        if (!connect(fd,a->ai_addr,a->ai_addrlen)) { r->socket=fd;break; }
        close(fd);
    }
    freeaddrinfo(list);
    if (r->socket<0) return -ECONNREFUSED;
    r->ssh=libssh2_session_init();
    if (!r->ssh) return -ENOMEM;
    libssh2_session_set_timeout(r->ssh,20000);
    const char *ciphers="aes128-gcm@openssh.com,aes256-gcm@openssh.com,aes128-ctr,aes256-ctr,chacha20-poly1305@openssh.com";
    libssh2_session_method_pref(r->ssh,LIBSSH2_METHOD_CRYPT_CS,ciphers);
    libssh2_session_method_pref(r->ssh,LIBSSH2_METHOD_CRYPT_SC,ciphers);
    if (libssh2_session_handshake(r->ssh,r->socket)) return -EPROTO;
    if (known_host(r,c)) return -EACCES;
    int rc=*c->key_file ? libssh2_userauth_publickey_fromfile(r->ssh,c->user,NULL,c->key_file,c->key_pass) :
                         libssh2_userauth_password(r->ssh,c->user,c->password);
    if (rc) return -EACCES;
    r->sftp=libssh2_sftp_init(r->ssh);
    return r->sftp ? 0 : -EIO;
}
static void ssh_disconnect(struct nf_remote *r) {
    if (r->sftp) { libssh2_sftp_shutdown(r->sftp);r->sftp=NULL; }
    if (r->ssh) { libssh2_session_disconnect(r->ssh,"AERA network storage disconnected");libssh2_session_free(r->ssh);r->ssh=NULL; }
    if (r->socket>=0) { close(r->socket);r->socket=-1; }
    libssh2_exit();
}
static void convert_stat(const LIBSSH2_SFTP_ATTRIBUTES *a,struct stat *st) {
    memset(st,0,sizeof(*st));
    st->st_mode=a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS ? a->permissions : S_IFREG|0666;
    st->st_nlink=S_ISDIR(st->st_mode)?2:1;
    st->st_size=a->filesize;
    st->st_uid=0;st->st_gid=0;
    st->st_blksize=NF_BLOCK;st->st_blocks=(a->filesize+511)/512;
    st->st_atime=a->atime;st->st_mtime=a->mtime;st->st_ctime=a->mtime;
}
static int ssh_stat(struct nf_remote *r,const char *p,struct stat *st,void *fh) {
    LIBSSH2_SFTP_ATTRIBUTES a={0};
    int rc=fh ? libssh2_sftp_fstat(fh,&a) : libssh2_sftp_lstat(r->sftp,p,&a);
    if (!rc) convert_stat(&a,st);
    return rc ? ssh_error(r) : 0;
}
static int ssh_list(struct nf_remote *r,const char *p,nf_entry_fn emit,void *data) {
    LIBSSH2_SFTP_HANDLE *dir=libssh2_sftp_opendir(r->sftp,p);
    if (!dir) return ssh_error(r);
    char name[4096];LIBSSH2_SFTP_ATTRIBUTES a;
    int n,rc=0;
    while ((n=libssh2_sftp_readdir(dir,name,sizeof(name)-1,&a))>0) {
        name[n]=0;struct stat st;
        convert_stat(&a,&st);
        if ((rc=emit(data,name,&st))) break;
    }
    if (n<0) rc=ssh_error(r);
    if (libssh2_sftp_closedir(dir) && !rc) rc=ssh_error(r);
    return rc;
}
static int ssh_open(struct nf_remote *r,const char *p,int flags,mode_t mode,void **out) {
    unsigned long f=0;
    if ((flags&O_ACCMODE)!=O_WRONLY) f|=LIBSSH2_FXF_READ;
    if ((flags&O_ACCMODE)!=O_RDONLY) f|=LIBSSH2_FXF_WRITE;
    if (flags&O_CREAT) f|=LIBSSH2_FXF_CREAT;
    if (flags&O_TRUNC) f|=LIBSSH2_FXF_TRUNC;
    if (flags&O_EXCL) f|=LIBSSH2_FXF_EXCL;
    *out=libssh2_sftp_open(r->sftp,p,f,mode&0777);
    return *out ? 0 : ssh_error(r);
}
static int ssh_close(struct nf_remote *r,void *f) { return libssh2_sftp_close(f) ? ssh_error(r) : 0; }
static int ssh_read(struct nf_remote *r,void *f,void *buffer,size_t n,uint64_t offset) {
    libssh2_sftp_seek64(f,offset);
    size_t done=0;
    while (done<n) {
        ssize_t got=libssh2_sftp_read(f,(char *)buffer+done,n-done);
        if (got<0) return ssh_error(r);
        if (!got) break;
        done+=(size_t)got;
    }
    return (int)done;
}
static int ssh_write(struct nf_remote *r,void *f,const void *buffer,size_t n,uint64_t offset) {
    libssh2_sftp_seek64(f,offset);
    size_t done=0;
    while (done<n) {
        ssize_t put=libssh2_sftp_write(f,(const char *)buffer+done,n-done);
        if (put<=0) return ssh_error(r);
        done+=(size_t)put;
    }
    return (int)done;
}
static int ssh_flush(struct nf_remote *r,void *f) {
    if (!libssh2_sftp_fsync(f)) return 0;
    int rc=ssh_error(r);
    /* Older SFTP servers have no fsync extension; WRITE replies still must be ACKed. */
    return rc==-EOPNOTSUPP ? 0 : rc;
}
static int ssh_truncate(struct nf_remote *r,const char *p,void *f,uint64_t n) {
    LIBSSH2_SFTP_ATTRIBUTES a={.flags=LIBSSH2_SFTP_ATTR_SIZE,.filesize=n};
    int rc=f ? libssh2_sftp_fsetstat(f,&a) : libssh2_sftp_setstat(r->sftp,p,&a);
    return rc ? ssh_error(r) : 0;
}
static int ssh_mkdir(struct nf_remote *r,const char *p,mode_t mode) { return libssh2_sftp_mkdir(r->sftp,p,mode&0777) ? ssh_error(r) : 0; }
static int ssh_unlink(struct nf_remote *r,const char *p,int dir) {
    int rc=dir ? libssh2_sftp_rmdir(r->sftp,p) : libssh2_sftp_unlink(r->sftp,p);
    return rc ? ssh_error(r) : 0;
}
static int ssh_rename(struct nf_remote *r,const char *from,const char *to,unsigned flags) {
    if (flags) return -EOPNOTSUPP;
    if (!libssh2_sftp_posix_rename(r->sftp,from,to)) return 0;
    if (ssh_error(r)!=-EOPNOTSUPP) return ssh_error(r);
    return libssh2_sftp_rename_ex(r->sftp,from,(unsigned)strlen(from),to,(unsigned)strlen(to),LIBSSH2_SFTP_RENAME_OVERWRITE) ? ssh_error(r) : 0;
}
static int ssh_chmod(struct nf_remote *r,const char *p,mode_t mode) {
    LIBSSH2_SFTP_ATTRIBUTES a={.flags=LIBSSH2_SFTP_ATTR_PERMISSIONS,.permissions=mode&07777};
    return libssh2_sftp_setstat(r->sftp,p,&a) ? ssh_error(r) : 0;
}
static int ssh_chown(struct nf_remote *r,const char *p,uid_t uid,gid_t gid) {
    (void)r;(void)p;(void)uid;(void)gid;
    /* Android storage IDs must not be applied to the server's login account. */
    return 0;
}
static int ssh_utimens(struct nf_remote *r,const char *p,const struct timespec times[2]) {
    LIBSSH2_SFTP_ATTRIBUTES a={.flags=LIBSSH2_SFTP_ATTR_ACMODTIME};
    struct stat st;
    if ((times[0].tv_nsec==UTIME_OMIT || times[1].tv_nsec==UTIME_OMIT) && ssh_stat(r,p,&st,NULL)) return -EIO;
    time_t current=time(NULL);
    a.atime=times[0].tv_nsec==UTIME_OMIT ? st.st_atime : times[0].tv_nsec==UTIME_NOW ? current : times[0].tv_sec;
    a.mtime=times[1].tv_nsec==UTIME_OMIT ? st.st_mtime : times[1].tv_nsec==UTIME_NOW ? current : times[1].tv_sec;
    return libssh2_sftp_setstat(r->sftp,p,&a) ? ssh_error(r) : 0;
}
static int ssh_statfs(struct nf_remote *r,const char *p,struct statvfs *out) {
    LIBSSH2_SFTP_STATVFS s;
    if (libssh2_sftp_statvfs(r->sftp,p,strlen(p),&s)) return ssh_error(r);
    memset(out,0,sizeof(*out));
    out->f_bsize=s.f_bsize;out->f_frsize=s.f_frsize;out->f_blocks=s.f_blocks;
    out->f_bfree=s.f_bfree;out->f_bavail=s.f_bavail;out->f_files=s.f_files;
    out->f_ffree=s.f_ffree;out->f_favail=s.f_favail;out->f_namemax=s.f_namemax;
    return 0;
}
static int ssh_readlink(struct nf_remote *r,const char *p,char *out,size_t size) {
    int n=libssh2_sftp_readlink(r->sftp,p,out,(unsigned)(size-1));
    if (n<0) return ssh_error(r);
    out[n]=0;return 0;
}
static int ssh_symlink(struct nf_remote *r,const char *target,const char *p) {
    return libssh2_sftp_symlink(r->sftp,target,(char *)p) ? ssh_error(r) : 0;
}
const struct nf_ops nf_ssh_ops={ssh_connect,ssh_disconnect,ssh_stat,ssh_list,ssh_open,ssh_close,
    ssh_read,ssh_write,ssh_flush,ssh_truncate,ssh_mkdir,ssh_unlink,ssh_rename,ssh_chmod,ssh_chown,
    ssh_utimens,ssh_statfs,ssh_readlink,ssh_symlink};
