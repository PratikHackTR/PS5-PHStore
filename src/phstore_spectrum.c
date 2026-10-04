/* Adapter around the supplied Spectrum 1.4.8 disk helper.
 * Host compilation is not proof of elfldr/process ABI on a PS5.
 * Catalogue and saved disk manifests are supplied by the generated ELF assets.
 */
#include "phstore_spectrum.h"
#include "phstore_assets.h"
#include "phstore_install.h"
#include "phstore_install_request.h"
#include "phstore_thread.h"
#include "phstore_catalog_config.h"
#include "phstore_gdrive.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CANCEL_PATH "/data/phstore2/download.cancel"
#define PROGRESS_PORT 9877
extern const unsigned char spectrum_helper_elf[], spectrum_helper_elf_end[];
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static int active;
static int delete_requested;
#define PAUSE_PATH "/data/phstore2/download.pause"
static phstore_package_info_t current;
static char phase[64] = "idle";
static uint64_t downloaded, total, speed;
static int percent;

static int native_file(const phstore_package_info_t *p) {
    return !strcmp(p->source_type,"google_drive_public") || !strcmp(p->source_type,"archive_public");
}
int phstore_spectrum_active(void) {
    pthread_mutex_lock(&gate); int value = active; pthread_mutex_unlock(&gate); return value;
}
const char *phstore_spectrum_install_start_json(const char *body, size_t length) {
    char id[PHSTORE_PACKAGE_ID_MAX];
    if(!phstore_install_request_parse(body,length,id)) return "invalid_install_request";
    phstore_package_info_t selected; char selected_error[64];
    if(phstore_catalog_find_install_package(id,&selected,selected_error)==1 && !strcmp(selected.action_type,"download_file"))
        return "file_download_use_download";
    if(!strncmp(id,"sp-",3)) {
        phstore_package_info_t package; char diagnostic[64];
        if(phstore_catalog_find_install_package(id,&package,diagnostic)!=1) return "package_not_found";
        const char *ext=strrchr(package.filename,'.');
        if(!ext || strcmp(ext,".pkg")) return "disk_format_use_download";
    }
    pthread_mutex_lock(&gate);
    const char *error = active ? "download_in_progress" : phstore_install_start_json(body, length);
    pthread_mutex_unlock(&gate); return error;
}
static void set_phase(const char *value) {
    pthread_mutex_lock(&gate); snprintf(phase, sizeof(phase), "%s", value); pthread_mutex_unlock(&gate);
}
static int patch_value(unsigned char *elf, size_t size, const char *marker, size_t slot, const char *value) {
    size_t key = strlen(marker), n = strlen(value);
    if (key >= slot || n >= slot-key || size < slot) return -1;
    unsigned char *found = NULL;
    for (size_t i=0; i+slot<=size; i++) if (!memcmp(elf+i, marker, key)) {
        if (found) return -1; found = elf+i;
    }
    if (!found) return -1;
    memset(found+key, 0, slot-key); memcpy(found+key, value, n); return 0;
}
static int terminal(const char *value) {
    return !strcmp(value, "completed") || !strcmp(value, "cancelled") || !strncmp(value, "error:", 6);
}
static int receive_progress(const char *buf) {
    unsigned long long d=0,t=0,s=0;
    int p=0,used=0; char value[64];
    if (sscanf(buf,"{\"p\":%d,\"s\":%llu,\"d\":%llu,\"t\":%llu,\"st\":\"%63[A-Za-z0-9:_-]\"}%n",
               &p,&s,&d,&t,value,&used)!=5 || !used || buf[used] || p<0 || p>100 || (t && d>t)) return 0;
    pthread_mutex_lock(&gate);
    percent=p; speed=s; downloaded=d; total=t;
    snprintf(phase,sizeof(phase),"%s",value);
    pthread_mutex_unlock(&gate);
    return terminal(value);
}
static int loader_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_port=htons(9021); addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    struct timeval timeout={.tv_sec=20,.tv_usec=0};
    (void)setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    int flags=fcntl(fd,F_GETFL,0);
    if (flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) {close(fd);return -1;}
    int rc=connect(fd,(struct sockaddr *)&addr,sizeof(addr));
    if(rc<0 && errno!=EINPROGRESS){close(fd);return -1;}
    if(rc<0){
        struct pollfd p={.fd=fd,.events=POLLOUT}; int error=0; socklen_t n=sizeof(error);
        if(poll(&p,1,20000)<=0 || getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&n)<0 || error){close(fd);return -1;}
    }
    if(fcntl(fd,F_SETFL,flags)<0){close(fd);return -1;}
    return fd;
}
static void native_progress(const char *value,uint64_t d,uint64_t t,uint64_t s) {
    pthread_mutex_lock(&gate);snprintf(phase,sizeof(phase),"%s",value);
    downloaded=d;total=t;speed=s;percent=t?(int)((double)d*100.0/(double)t):0;pthread_mutex_unlock(&gate);
}
static int remove_download_files(const phstore_package_info_t *p) {
    char path[512],sidecar[550];
    if(native_file(p)) {
        if(phstore_gdrive_destination(p,path,sizeof(path)))return -1;
    } else if(snprintf(path,sizeof(path),"/data/homebrew/%s",p->filename)>=(int)sizeof(path))return -1;
    int failed=0;
    if(unlink(path)&&errno!=ENOENT)failed=1;
    const char *suffixes[]={".part",".sha256",".sha256.tmp",".resume.json",".resume.json.tmp"};
    for(size_t i=0;i<sizeof(suffixes)/sizeof(suffixes[0]);i++) {
        snprintf(sidecar,sizeof(sidecar),"%s%s",path,suffixes[i]);if(unlink(sidecar)&&errno!=ENOENT)failed=1;
    }
    return failed?-1:0;
}
static void finish_download(void) {
    pthread_mutex_lock(&gate);
    if(delete_requested) {
        snprintf(phase,sizeof(phase),"%s",remove_download_files(&current)?"error:delete_failed":"deleted");
        downloaded=0;percent=0;speed=0;
    }
    active=0;pthread_mutex_unlock(&gate);
}
static void *download_worker(void *unused) {
    (void)unused;
    int udp=-1,loader=-1,log=-1; unsigned char *elf=NULL;
    const char *failure="helper_start_failed";
    phstore_package_info_t package;
    pthread_mutex_lock(&gate); package=current; pthread_mutex_unlock(&gate);
    if(native_file(&package)) {
        int result=phstore_gdrive_download(&package,native_progress);
        pthread_mutex_lock(&gate);if(result && !strncmp(phase,"starting",8))snprintf(phase,sizeof(phase),"error:gdrive_start");
        pthread_mutex_unlock(&gate);finish_download();return NULL;
    }
    size_t size=(size_t)(spectrum_helper_elf_end-spectrum_helper_elf);
    elf=malloc(size);
    if(!elf) goto failed;
    memcpy(elf,spectrum_helper_elf,size);
    char dest[512];
    int n=snprintf(dest,sizeof(dest),"/data/homebrew/%s",package.filename);
    if(n<0 || (size_t)n>=sizeof(dest)) goto failed;
    char manifest[512],local_manifest[512];
    n=snprintf(local_manifest,sizeof(local_manifest),"/data/phstore2/manifests/%s.json",package.package_id);
    if(n<0 || (size_t)n>=sizeof(local_manifest))goto failed;
    /* Always restore the matching build snapshot atomically, avoiding stale uploads. */
    for(size_t i=0;i<phstore_embedded_manifest_count;i++) {
        const phstore_embedded_asset_t *asset=&phstore_embedded_manifests[i];
        if(strcmp(asset->path,package.package_id))continue;
        if((mkdir("/data/phstore2",0777)<0 && errno!=EEXIST) ||
           (mkdir("/data/phstore2/manifests",0777)<0 && errno!=EEXIST)) {
            failure="manifest_directory_failed";goto failed;
        }
        char temporary[540];snprintf(temporary,sizeof(temporary),"%s.tmp",local_manifest);
        FILE *file=fopen(temporary,"wb");
        if(!file){failure="manifest_write_failed";goto failed;}
        int ok=fwrite(asset->data,1,asset->size,file)==asset->size;
        if(fflush(file) || fsync(fileno(file)))ok=0;
        if(fclose(file))ok=0;
        if(!ok || rename(temporary,local_manifest)){unlink(temporary);failure="manifest_write_failed";goto failed;}
        break;
    }
    if(access(local_manifest,R_OK)==0)snprintf(manifest,sizeof(manifest),"%s",local_manifest);
    else snprintf(manifest,sizeof(manifest),"http://" PHSTORE_CATALOG_HOST "/phstore2/manifest/%s.json",package.package_id);
    if(patch_value(elf,size,"SL_EXFAT_FFPKG_MANIFEST=",1024,manifest) ||
       patch_value(elf,size,"SL_EXFAT_FFPKG_DEST=",512,dest) ||
       patch_value(elf,size,"SL_EXFAT_FFPKG_PORT=",32,"9877") ||
       patch_value(elf,size,"SL_EXFAT_FFPKG_CANCEL=",512,CANCEL_PATH) ||
       patch_value(elf,size,"SL_EXFAT_FFPKG_PARTS=",32,"4") ||
       patch_value(elf,size,"SL_EXFAT_FFPKG_TITLE=",256,"PHStore2")) {
        failure="helper_patch_failed"; goto failed;
    }
    udp=socket(AF_INET,SOCK_DGRAM,0);
    if(udp<0) goto failed;
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET;addr.sin_port=htons(PROGRESS_PORT);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(udp,(struct sockaddr *)&addr,sizeof(addr))<0){failure="progress_port_busy";goto failed;}
    loader=loader_connect();
    if(loader<0){failure="elfldr_9021_unreachable";goto failed;}
    size_t sent=0;
    while(sent<size) {
        ssize_t count=send(loader,elf+sent,size-sent,0);
        if(count<0 && errno==EINTR) continue;
        if(count<=0){failure="helper_send_unconfirmed";goto unconfirmed;}
        sent+=(size_t)count;
    }
    shutdown(loader,SHUT_WR);
    (void)fcntl(loader,F_SETFL,O_NONBLOCK);
    free(elf);elf=NULL;
    log=open("/data/phstore2/helper-output.log",O_WRONLY|O_CREAT|O_TRUNC,0600);
    time_t start=time(NULL); int seen=0;
    for(;;) {
        struct pollfd fds[2]={{.fd=udp,.events=POLLIN},{.fd=loader,.events=POLLIN}};
        if(poll(fds,2,1000)<0 && errno!=EINTR) {set_phase("progress_monitor_unconfirmed");continue;}
        if(fds[0].revents&POLLIN) {
            char buf[520];ssize_t count=recv(udp,buf,sizeof(buf)-1,0);
            if(count>0){buf[count]='\0';seen=1;if(receive_progress(buf))break;}
        }
        if(loader>=0 && fds[1].revents&(POLLIN|POLLHUP|POLLERR)) {
            char buf[4096];ssize_t count=recv(loader,buf,sizeof(buf),0);
            if(count>0 && log>=0)(void)write(log,buf,(size_t)count);
            else if(count==0){close(loader);loader=-1;}
        }
        if(!seen && time(NULL)-start>=30) set_phase("helper_launch_unconfirmed");
    }
    /* Helper sleeps two seconds before exit; do not launch its replacement early. */
    struct timespec delay={.tv_sec=2,.tv_nsec=500000000};
    while(nanosleep(&delay,&delay)<0 && errno==EINTR) {}
    if(udp>=0)close(udp);if(loader>=0)close(loader);if(log>=0)close(log);
    finish_download();
    return NULL;
unconfirmed:
    /* Partial upload is not proof of absence of a live helper. Preserve the gate. */
    set_phase(failure); free(elf);if(udp>=0)close(udp);if(loader>=0)close(loader);if(log>=0)close(log);
    return NULL;
failed:
    free(elf);if(udp>=0)close(udp);if(loader>=0)close(loader);if(log>=0)close(log);
    pthread_mutex_lock(&gate);snprintf(phase,sizeof(phase),"%s",failure);active=0;pthread_mutex_unlock(&gate);
    return NULL;
}
const char *phstore_spectrum_start_json(const char *body,size_t length) {
    char id[PHSTORE_PACKAGE_ID_MAX],error[64];phstore_package_info_t package;
    if(!phstore_install_request_parse(body,length,id))return "invalid_install_request";
    if(phstore_catalog_find_install_package(id,&package,error)!=1)return "package_not_found";
    int native=native_file(&package)&&!strcmp(package.action_type,"download_file");
    if(!native&&strncmp(id,"sp-",3))return "spectrum_package_required";
    const char *ext=strrchr(package.filename,'.');
    if(!native && (!ext || (strcmp(ext,".exfat") && strcmp(ext,".ffpfsc") && strcmp(ext,".ffpkg") && strcmp(ext,".ffpfs"))))return "disk_format_required";
    if(!package.installable || (!native&&!package.download_url[0]))return "download_unavailable";
    if(native&&!strcmp(package.source_type,"google_drive_public")&&(!phstore_gdrive_id_valid(package.file_id)||(package.sha256[0]&&!phstore_gdrive_sha_valid(package.sha256))))return "invalid_gdrive_fields";
    pthread_mutex_lock(&gate);
    phstore_install_status_t status;phstore_install_get_status(&status);
    if(active || status.state==PHSTORE_INSTALL_STARTING || status.state==PHSTORE_INSTALL_INSTALLING || status.state==PHSTORE_INSTALL_CANCELING){pthread_mutex_unlock(&gate);return "download_in_progress";}
    (void)mkdir("/data/phstore2",0700);(void)mkdir("/data/homebrew",0777);
    unlink(CANCEL_PATH);unlink(CANCEL_PATH ".delete");unlink(PAUSE_PATH);delete_requested=0;
    current=package;active=1;downloaded=0;total=package.size_bytes;percent=0;speed=0;
    snprintf(phase,sizeof(phase),"starting");
    pthread_t thread;int rc=phstore_thread_create(&thread,download_worker,NULL);
    if(rc){active=0;snprintf(phase,sizeof(phase),"thread_create_failed");}
    else pthread_detach(thread);
    pthread_mutex_unlock(&gate);return rc?"thread_create_failed":NULL;
}
int phstore_spectrum_pause(void) {
    pthread_mutex_lock(&gate);
    if(!active){pthread_mutex_unlock(&gate);return -1;}
    int fd=open(native_file(&current)?PAUSE_PATH:CANCEL_PATH,O_WRONLY|O_CREAT|O_TRUNC,0600);
    int result=fd<0?-1:0;if(fd>=0)close(fd);
    pthread_mutex_unlock(&gate);return result;
}
int phstore_spectrum_resume(void) {
    pthread_mutex_lock(&gate);
    if(delete_requested || !current.package_id[0]){pthread_mutex_unlock(&gate);return -1;}
    if(active && native_file(&current)) {
        int rc=unlink(PAUSE_PATH);pthread_mutex_unlock(&gate);return rc&&errno!=ENOENT?-1:0;
    }
    if(active || strcmp(phase,"cancelled")){pthread_mutex_unlock(&gate);return -1;}
    char body[180];int n=snprintf(body,sizeof(body),"{\"package_id\":\"%s\"}",current.package_id);
    pthread_mutex_unlock(&gate);return phstore_spectrum_start_json(body,(size_t)n)?-1:0;
}
int phstore_spectrum_cancel_delete(void) {
    pthread_mutex_lock(&gate);
    if(!current.package_id[0] || !strcmp(phase,"completed")){pthread_mutex_unlock(&gate);return -1;}
    delete_requested=1;
    int fd=open(CANCEL_PATH ".delete",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0){delete_requested=0;pthread_mutex_unlock(&gate);return -1;}close(fd);
    fd=open(CANCEL_PATH,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0){delete_requested=0;pthread_mutex_unlock(&gate);return -1;}close(fd);
    unlink(PAUSE_PATH);
    if(!active){snprintf(phase,sizeof(phase),"%s",remove_download_files(&current)?"error:delete_failed":"deleted");downloaded=0;percent=0;}
    pthread_mutex_unlock(&gate);return 0;
}
size_t phstore_spectrum_status_json(char *body,size_t capacity) {
    pthread_mutex_lock(&gate);
    char filename[512];size_t escaped=0;
    for(size_t i=0;current.filename[i]&&escaped+2<sizeof(filename);i++) {
        unsigned char c=(unsigned char)current.filename[i];if(c=='"'||c=='\\')filename[escaped++]='\\';filename[escaped++]=c;
    }
    filename[escaped]=0;
    int n=snprintf(body,capacity,"{\"ok\":true,\"active\":%s,\"package_id\":\"%s\",\"filename\":\"%s\",\"state\":\"%s\",\"percent\":%d,\"downloaded_bytes\":%llu,\"total_bytes\":%llu,\"speed_bytes\":%llu,\"native_gdrive\":%s}",
        active?"true":"false",current.package_id,filename,(active&&native_file(&current)&&access(PAUSE_PATH,F_OK)==0)?"paused":phase,percent,(unsigned long long)downloaded,(unsigned long long)total,(unsigned long long)speed,native_file(&current)?"true":"false");
    pthread_mutex_unlock(&gate);
    return n>0 && (size_t)n<capacity?(size_t)n:0;
}
