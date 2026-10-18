/* AERA userspace SMB/SFTP network storage manager. */
#include "NasManager.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <json/json.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <signal.h>
#include <sstream>
#include <string.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../aera_remote/backup_cancel.hpp"
#include "../aera_secrets/aera_secrets.hpp"
#include "../data.hpp"
#include "../twcommon.h"
#include "../twrp-functions.hpp"
#include "../variables.h"

const std::string NasManager::Mount_Point = TW_NAS_MOUNT_POINT;
const std::string NasManager::Config_Dir = "/tmp/aera_nas";
const std::string NasManager::Config_File = Config_Dir + "/connection.json";
const std::string NasManager::Cache_Dir = "/data/media/0/AERA/NASCache/native";
const std::string NasManager::Log_File = Config_Dir + "/network.log";
const std::string NasManager::Native_Binary = "/system/bin/aera-netfs";
std::string NasManager::last_error;

namespace {
const std::string kStats = NasManager::Config_Dir + "/status.json";
pid_t child_pid = -1;
uint64_t baseline_errors = 0, baseline_bytes = 0;
pid_t baseline_pid = -1;
struct Stats { uint64_t bytes=0,pending=0,files=0,retained=0,errors=0; pid_t pid=-1; };
bool HelperAlive(pid_t pid) {
    if (pid <= 1 || kill(pid,0)) return false;
    std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    std::getline(in,name);
    return name == "aera-netfs";
}
bool ReadStats(Stats& stats) {
    std::ifstream input(kStats);
    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string error;
    if (!Json::parseFromStream(reader,input,&root,&error) || root["version"].asInt()!=1 ||
        !root["pid"].isInt() || !root["connected"].asBool()) return false;
    for (const char *key : {"written","pending_bytes","pending_files","write_errors"})
        if (!root[key].isUInt64()) return false;
    stats.pid=root["pid"].asInt();
    stats.bytes=root["written"].asUInt64();
    stats.pending=root["pending_bytes"].asUInt64();
    stats.files=root["pending_files"].asUInt64();
    stats.retained=root["retained_files"].asUInt64();
    stats.errors=root["write_errors"].asUInt64();
    return HelperAlive(stats.pid);
}
std::string CacheMode() {
    std::string mode=DataManager::GetStrValue(TW_NAS_CACHE_MODE);
    std::transform(mode.begin(),mode.end(),mode.begin(),[](unsigned char c){return std::tolower(c);});
    mode=(mode=="data" || mode=="cache" || mode=="writes" || mode=="1" || mode=="true" || mode=="yes") ? "data" : "off";
    DataManager::SetValue(TW_NAS_CACHE_MODE,mode);
    return mode;
}
bool EmptyDirectory(const std::string& path) {
    DIR *dir=opendir(path.c_str());
    if (!dir) return errno==ENOENT;
    bool empty=true;
    while (dirent *e=readdir(dir)) if (strcmp(e->d_name,".") && strcmp(e->d_name,"..")) {empty=false;break;}
    closedir(dir);
    return empty;
}
}

void NasManager::SetError(const std::string& error) {
    last_error=error;
    DataManager::SetValue(TW_NAS_LAST_ERROR,error);
    DataManager::SetValue(TW_NAS_STATUS_TEXT,error);
    LOGERR("NAS: %s\n",error.c_str());
}
std::string NasManager::GetLastError() {return last_error;}
std::string NasManager::Trim(const std::string& input) {
    size_t a=input.find_first_not_of(" \t\r\n"),b=input.find_last_not_of(" \t\r\n");
    return a==std::string::npos ? "" : input.substr(a,b-a+1);
}
std::string NasManager::GetValue(const std::string& key,const std::string& fallback) {
    std::string value=Trim(DataManager::GetStrValue(key));
    return value.empty() ? fallback : value;
}
bool NasManager::EnsureDirectory(const std::string& path,mode_t mode) {
    struct stat st{};
    if (lstat(path.c_str(),&st)==0) {
        if (!S_ISDIR(st.st_mode)) {SetError("Network storage path is not a directory: "+path);return false;}
        return chmod(path.c_str(),mode)==0;
    }
    if (errno==ENOENT && TWFunc::Recursive_Mkdir(path) && chmod(path.c_str(),mode)==0) return true;
    SetError("Could not create network storage directory: "+path);
    return false;
}
bool NasManager::WriteFile(const std::string& path,const std::string& data,mode_t mode) {
    int fd=open(path.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,mode);
    if (fd<0) {SetError("Could not create private network storage configuration.");return false;}
    size_t written=0;
    while (written<data.size()) {
        ssize_t n=write(fd,data.data()+written,data.size()-written);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) break;
        written+=static_cast<size_t>(n);
    }
    bool okay=written==data.size() && fchmod(fd,mode)==0 && fsync(fd)==0;
    if (close(fd)) okay=false;
    if (!okay) SetError("Could not write private network storage configuration.");
    return okay;
}
bool NasManager::ReadFile(const std::string& path,std::string& out) {
    std::ifstream input(path,std::ios::binary);
    if (!input) return false;
    char buffer[8192];input.read(buffer,sizeof(buffer));
    out.assign(buffer,static_cast<size_t>(input.gcount()));
    return true;
}
bool NasManager::RunDetached(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const std::string& arg:args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid=fork();
    if (pid<0) {SetError("Could not start native network storage.");return false;}
    if (!pid) {
        setsid();
        int fd=open("/dev/null",O_RDWR);
        if (fd>=0) {dup2(fd,0);dup2(fd,1);dup2(fd,2);if(fd>2)close(fd);}
        execv(argv[0],argv.data());
        _exit(127);
    }
    child_pid=pid;
    return true;
}
bool NasManager::BuildNativeConfig() {
    Json::Value config;
    config["version"]=1;
    std::string type=GetValue(TW_NAS_TYPE,"sftp");
    std::transform(type.begin(),type.end(),type.begin(),[](unsigned char c){return std::tolower(c);});
    if (type!="smb" && type!="sftp") {SetError("Unsupported network storage protocol.");return false;}
    config["type"]=type;
    config["host"]=GetValue(TW_NAS_HOST,"");
    if (config["host"].asString().empty()) {SetError("NAS host/IP is empty.");return false;}
    config["user"]=DataManager::GetStrValue(TW_NAS_USER);
    config["password"]=DataManager::GetStrValue(TW_NAS_PASS);
    config["domain"]=GetValue(TW_NAS_DOMAIN,"WORKGROUP");
    config["share"]=type=="smb" ? GetValue(TW_NAS_SHARE,"Backups") : "";
    config["path"]=GetValue(TW_NAS_PATH,"");
    const std::string port=GetValue(TW_NAS_PORT,"22");
    char *end=nullptr;
    long value=strtol(port.c_str(),&end,10);
    if (type=="sftp" && (!end || *end || value<1 || value>65535)) {SetError("Invalid SFTP port.");return false;}
    config["port"]=type=="sftp" ? static_cast<int>(value) : 445;
    config["status_file"]=kStats;
    config["log_file"]=Log_File;
    config["cache_dir"]=CacheMode()=="data" ? Cache_Dir : "";
    if (type=="sftp" && GetValue("aera_nas_auth","password")=="key") {
        std::string key;
        if (!AeraSecrets::GetNasPrivateKey(key)) {SetError("No saved SFTP private key. Import a private key first.");return false;}
        const std::string key_path=Config_Dir+"/sftp.key";
        bool saved=WriteFile(key_path,key,0600);
        OPENSSL_cleanse(key.data(),key.size());
        if (!saved) return false;
        config["key_file"]=key_path;
        config["key_pass"]=config["password"];
        config["password"]="";
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"]="";
    std::string json=Json::writeString(writer,config);
    bool saved=WriteFile(Config_File,json,0600);
    OPENSSL_cleanse(json.data(),json.size());
    return saved;
}
bool NasManager::IsFuseAvailable() {
    if (TWFunc::Path_Exists("/dev/fuse")) return true;
    SetError("/dev/fuse is missing. Kernel FUSE support is required.");
    return false;
}
bool NasManager::IsMounted() {
    std::ifstream mounts("/proc/mounts");std::string line;
    while (std::getline(mounts,line)) if (line.find(" "+Mount_Point+" ")!=std::string::npos) return true;
    return false;
}
bool NasManager::Mount() {
    last_error.clear();DataManager::SetValue(TW_NAS_LAST_ERROR,"");
    if (IsMounted()) return RefreshStatus();
    if (!TWFunc::Path_Exists(Native_Binary)) {SetError("Native SMB/SFTP support is missing from this recovery build.");return false;}
    if (!IsFuseAvailable() || !EnsureDirectory(Config_Dir,0700) || !EnsureDirectory(Mount_Point,0777)) return false;
    if (CacheMode()=="data") {
        if (!TWFunc::Path_Exists("/data/media/0") || !EnsureDirectory(Cache_Dir,0700)) return false;
        if (!EmptyDirectory(Cache_Dir)) {
            SetError("Unfinished NAS cache files remain in "+Cache_Dir+". Preserve or recover them before enabling the write cache again.");
            return false;
        }
    }
    unlink(kStats.c_str());unlink(Log_File.c_str());
    if (!BuildNativeConfig() || !RunDetached({Native_Binary,"mount","--config",Config_File,Mount_Point})) return false;
    for (int i=0;i<300;++i) {
        Stats stats;
        if (IsMounted() && ReadStats(stats)) {
            baseline_errors=stats.errors;baseline_bytes=stats.bytes;baseline_pid=stats.pid;
            return RefreshStatus();
        }
        int status;
        if (waitpid(child_pid,&status,WNOHANG)==child_pid) {child_pid=-1;break;}
        usleep(100000);
    }
    if (child_pid>0) {
        kill(child_pid,SIGTERM);
        for (int i=0;i<30 && waitpid(child_pid,nullptr,WNOHANG)==0;++i) usleep(100000);
    }
    std::string log;
    ReadFile(Log_File,log);
    SetError(log.empty() ? "Native NAS mount did not become ready. Check server, credentials and Wi-Fi." : Trim(log));
    return false;
}
bool NasManager::ResetTransferStats() {
    last_error.clear();DataManager::SetValue(TW_NAS_LAST_ERROR,"");
    if (!IsMounted()) return true;
    Stats stats;
    if (!ReadStats(stats)) {SetError("Native NAS backend is not responding.");return false;}
    if (stats.pending || stats.files) {SetError("A previous NAS upload has not completed.");return false;}
    baseline_errors=stats.errors;baseline_bytes=stats.bytes;baseline_pid=stats.pid;
    return true;
}
bool NasManager::WaitForPendingUploads(int max_seconds,uint64_t expected_upload_bytes,const std::atomic<bool>* cancel) {
    last_error.clear();DataManager::SetValue(TW_NAS_LAST_ERROR,"");
    const auto start=std::chrono::steady_clock::now();
    for (;;) {
        if (cancel && (cancel->load() || aera::pc::BackupCancelRequested())) {SetError("NAS upload cancelled.");return false;}
        Stats stats;
        if (!IsMounted() || !ReadStats(stats)) {SetError("NAS disconnected before all writes were confirmed.");return false;}
        if (stats.pid!=baseline_pid || stats.errors>baseline_errors) {SetError("NAS reported a write/flush failure. Do not use the unfinished backup.");return false;}
        if (!stats.pending && !stats.files) {
            DataManager::SetValue(TW_NAS_STATUS_TEXT,"NAS writes confirmed");
            return true;
        }
        uint64_t sent=stats.bytes>=baseline_bytes ? stats.bytes-baseline_bytes : 0;
        std::ostringstream progress;
        progress<<"NAS upload: "<<(sent/1000000)<<" MB";
        if (expected_upload_bytes) progress<<" of "<<(expected_upload_bytes/1000000)<<" MB";
        progress<<", "<<(stats.pending/1000000)<<" MB pending";
        DataManager::SetValue(TW_NAS_STATUS_TEXT,progress.str());
        DataManager::SetValue("tw_size_progress",progress.str());
        if (max_seconds>0 && std::chrono::steady_clock::now()-start>=std::chrono::seconds(max_seconds)) {
            SetError("NAS upload did not finish in time. Cached files were preserved.");return false;
        }
        usleep(200000);
    }
}
bool NasManager::Unmount() {
    last_error.clear();DataManager::SetValue(TW_NAS_LAST_ERROR,"");
    if (!IsMounted()) {DataManager::SetValue(TW_NAS_MOUNTED,0);return true;}
    Stats stats;
    if (ReadStats(stats) && stats.files>stats.retained) {
        SetError("NAS still has open writes. Close open files before disconnecting.");return false;
    }
    if (umount2(Mount_Point.c_str(),0)) {
        SetError("NAS is busy. Close open files before disconnecting.");return false;
    }
    if (HelperAlive(stats.pid)) kill(stats.pid,SIGTERM);
    if (child_pid>0) waitpid(child_pid,nullptr,WNOHANG);
    child_pid=-1;
    unlink(Config_File.c_str());unlink((Config_Dir+"/sftp.key").c_str());unlink(kStats.c_str());
    DataManager::SetValue(TW_NAS_MOUNTED,0);
    DataManager::SetValue(TW_NAS_STATUS_TEXT,"NAS unmounted");
    return true;
}
bool NasManager::SelectAsStorage() {
    if (!IsMounted()) {SetError("NAS is not mounted. Connect before selecting it as storage.");return false;}
    DataManager::SetValue("tw_storage_path",Mount_Point);
    DataManager::SetValue(TW_ZIP_LOCATION_VAR,Mount_Point);
    DataManager::SetValue(TW_USE_EXTERNAL_STORAGE,1);
    DataManager::SetValue(TW_NAS_USE_AS_STORAGE,1);
    DataManager::SetValue(TW_NAS_STATUS_TEXT,"NAS selected as current storage");
    return true;
}
bool NasManager::RefreshStatus() {
    bool mounted=IsMounted();
    DataManager::SetValue(TW_NAS_MOUNTED,mounted?1:0);
    DataManager::SetValue(TW_NAS_STATUS_TEXT,mounted ? "NAS mounted at "+Mount_Point+(CacheMode()=="data" ? " using write cache" : " without disk cache") : "NAS not mounted");
    return mounted;
}
