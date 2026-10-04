/* Native source-built curl/wolfSSL disk downloader. No installer calls. */
#include "phstore_gdrive.h"
#include "phstore_config.h"
#include "phstore_gdrive_protocol.h"
#include "phstore_thread.h"
#include "crypto_control.h"
#include "embedded_ca.h"
#include "range_buffer.h"
#include <curl/curl.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define GD_WORKERS 8
#define GD_CHUNK (16u*1024u*1024u)
#define GD_DISK_BUFFER (1024u*1024u)
#define GD_ROOT "/data/phstore2/gdrive"
#define GD_CA GD_ROOT "/cacert.pem"
#define GD_PAUSE "/data/phstore2/download.pause"
#define GD_CANCEL "/data/phstore2/download.cancel"
typedef struct {
    phstore_package_info_t package;
    int fd,stop,running,error;
    uint64_t next,bytes,pwrites;char error_phase[64];
    char url[2048];FILE *log;pthread_mutex_t lock;
} job;
typedef struct {job *j;CURL *curl;uint64_t start,size,received,written;int ready,disk_error;
    ph_gdrive_headers h;range_buffer buffer;} transfer;
static pthread_once_t crypto_once=PTHREAD_ONCE_INIT;
static int crypto_error;
extern int probe_entropy_init(void);
static void crypto_init(void){crypto_error=probe_entropy_init();if(crypto_error)return;
    ph_crypto_cpu_init();ph_crypto_enabled=ph_crypto_hardware_ready();
    crypto_error=ph_crypto_selftest();if(!crypto_error)crypto_error=(int)curl_global_init(CURL_GLOBAL_DEFAULT);}
static int archive_package(const phstore_package_info_t *p) {
    return p && !strcmp(p->source_type,"archive_public") && !strcmp(p->action_type,"download_file") &&
        !strncmp(p->download_url,"https://archive.org/download/",29) && phstore_archive_url_allowed(p->download_url);
}
static const char *download_identity(const phstore_package_info_t *p){return archive_package(p)?p->package_id:p->file_id;}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void logline(job *j,const char *fmt,...){char message[1536];va_list a;va_start(a,fmt);
    vsnprintf(message,sizeof(message),fmt,a);va_end(a);pthread_mutex_lock(&j->lock);
    fprintf(j->log,"t=%.3f %s\n",seconds(),message);fflush(j->log);pthread_mutex_unlock(&j->lock);
    if(strstr(message,"exact=FAIL") || !strncmp(message,"HEADER_REJECT",13) ||
       !strncmp(message,"END ",4) || !strncmp(message,"BUILD=",6) || !strncmp(message,"CACHE_REUSE=REJECT",18))
        phstore_install_detailed_log("NATIVE_DRIVE_DIAGNOSTIC",message);
}
static int stopped(job *j){return __atomic_load_n(&j->stop,__ATOMIC_RELAXED);}
static void stop_job(job *j,int error){__atomic_store_n(&j->error,error,__ATOMIC_RELAXED);__atomic_store_n(&j->stop,1,__ATOMIC_RELAXED);}
static void transfer_failure(job *j,int error,const char *phase) {
    pthread_mutex_lock(&j->lock);
    if(!stopped(j)){snprintf(j->error_phase,sizeof(j->error_phase),"%s",phase);stop_job(j,error);}
    pthread_mutex_unlock(&j->lock);
}
static int sink(void *arg,const unsigned char *data,size_t n,uint64_t offset){transfer *t=arg;size_t done=0;
    while(done<n){ssize_t k=pwrite(t->j->fd,data+done,n-done,(off_t)(offset+done));if(k<0&&errno==EINTR)continue;
        if(k<=0){t->disk_error=errno?errno:EIO;return -1;}done+=(size_t)k;__atomic_fetch_add(&t->j->pwrites,1,__ATOMIC_RELAXED);}
    t->written+=n;return 0;
}
static int job_cancelled(job *j);
static void wait_if_paused(job *j){while(access(GD_PAUSE,F_OK)==0&&!job_cancelled(j)&&!stopped(j))usleep(100000);}
static size_t body(char *data,size_t size,size_t count,void *arg){transfer *t=arg;
    wait_if_paused(t->j);
    if(size&&count>SIZE_MAX/size)return 0;size_t n=size*count;
    if(t->h.status>=300&&t->h.status<400)return n;
    if(stopped(t->j)||!t->ready||!ph_gdrive_headers_exact(&t->h,t->start,t->size,t->j->package.size_bytes)||n>t->size-t->received)return 0;
    if(range_buffer_feed(&t->buffer,(unsigned char *)data,n))return 0;t->received+=n;return n;
}
static size_t header(char *data,size_t size,size_t count,void *arg){transfer *t=arg;
    if(size&&count>SIZE_MAX/size)return 0;size_t n=size*count;
    if(n>=5&&!memcmp(data,"HTTP/",5)){char line[80];size_t k=n<79?n:79;memcpy(line,data,k);line[k]=0;
        long status=0;if(sscanf(line,"HTTP/%*s %ld",&status)!=1||t->received)return 0;
        memset(&t->h,0,sizeof(t->h));t->h.status=status;t->h.allow_archive=archive_package(&t->j->package);t->ready=0;
    }else if(n==2&&!memcmp(data,"\r\n",2)){t->ready=1;
        if(t->h.bad||(!(t->h.status>=300&&t->h.status<400)&&!ph_gdrive_headers_exact(&t->h,t->start,t->size,t->j->package.size_bytes))){
            logline(t->j,"HEADER_REJECT HTTP=%ld start=%llu end=%llu total=%llu CL=%llu cr_count=%d cl_count=%d encoding=%d malformed=%d expected_start=%llu expected_size=%llu",
                t->h.status,(unsigned long long)t->h.start,(unsigned long long)t->h.end,(unsigned long long)t->h.total,(unsigned long long)t->h.length,
                t->h.cr,t->h.cl,t->h.encoding,t->h.bad,(unsigned long long)t->start,(unsigned long long)t->size);return 0;
        }
    }else ph_gdrive_header_line(&t->h,data,n);return n;
}
static int progress(void *arg,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d){(void)a;(void)b;(void)c;(void)d;return stopped(((transfer *)arg)->j);}
static int socket_tune(void *arg,curl_socket_t fd,curlsocktype purpose){job *j=arg;if(purpose!=CURLSOCKTYPE_IPCXN)return CURL_SOCKOPT_OK;
    int request=1048576,actual=-1;int rc=setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&request,sizeof(request));int e=rc?errno:0;socklen_t n=sizeof(actual);
    getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&actual,&n);logline(j,"SO_RCVBUF requested=%d actual=%d rc=%d errno=%d",request,actual,rc,e);return CURL_SOCKOPT_OK;
}
static CURLcode tls_context(CURL *c,void *ctx,void *arg){(void)c;(void)arg;
    return wolfSSL_CTX_set_read_ahead(ctx,1)==WOLFSSL_SUCCESS&&wolfSSL_CTX_set_default_read_buffer_len(ctx,65536)==WOLFSSL_SUCCESS?CURLE_OK:CURLE_SSL_CONNECT_ERROR;
}
static int request(transfer *t,char failure[64]){CURL *c=t->curl;char range[80],error[CURL_ERROR_SIZE]={0},*effective=NULL;long status=0,redirects=0,version=0,os=0,connects=0,verify=0;curl_off_t tls=0;
    struct curl_slist *list=curl_slist_append(NULL,"Accept-Encoding: identity");CURLcode code=CURLE_OUT_OF_MEMORY;
    if(!list||range_buffer_init(&t->buffer,t->start,t->size,GD_DISK_BUFFER,sink,t))goto done;
    curl_easy_reset(c);snprintf(range,sizeof(range),"%llu-%llu",(unsigned long long)t->start,(unsigned long long)(t->start+t->size-1));
#define SET(o,v) do{code=curl_easy_setopt(c,o,v);if(code!=CURLE_OK)goto done;}while(0)
    SET(CURLOPT_URL,t->j->url);SET(CURLOPT_RANGE,range);SET(CURLOPT_HTTPHEADER,list);
    SET(CURLOPT_HTTP_VERSION,(long)CURL_HTTP_VERSION_1_1);SET(CURLOPT_HTTP_CONTENT_DECODING,0L);
    SET(CURLOPT_FOLLOWLOCATION,1L);SET(CURLOPT_MAXREDIRS,20L);SET(CURLOPT_CONNECTTIMEOUT,30L);
    SET(CURLOPT_LOW_SPEED_LIMIT,1024L);SET(CURLOPT_LOW_SPEED_TIME,300L);SET(CURLOPT_TIMEOUT,0L);
    SET(CURLOPT_TCP_KEEPALIVE,1L);SET(CURLOPT_TCP_KEEPIDLE,30L);SET(CURLOPT_TCP_KEEPINTVL,10L);SET(CURLOPT_TCP_NODELAY,1L);
    SET(CURLOPT_NOSIGNAL,1L);SET(CURLOPT_IPRESOLVE,(long)CURL_IPRESOLVE_V4);SET(CURLOPT_DNS_CACHE_TIMEOUT,60L);SET(CURLOPT_BUFFERSIZE,1048576L);
    SET(CURLOPT_SSLVERSION,(long)CURL_SSLVERSION_TLSv1_2);SET(CURLOPT_SSL_VERIFYHOST,2L);SET(CURLOPT_SSL_VERIFYPEER,1L);SET(CURLOPT_CAINFO,GD_CA);
    SET(CURLOPT_PROTOCOLS_STR,"https");SET(CURLOPT_REDIR_PROTOCOLS_STR,"https");SET(CURLOPT_PROXY,"");SET(CURLOPT_NOPROXY,"*");
    SET(CURLOPT_SSL_CTX_FUNCTION,tls_context);SET(CURLOPT_SOCKOPTFUNCTION,socket_tune);SET(CURLOPT_SOCKOPTDATA,t->j);SET(CURLOPT_MAXCONNECTS,2L);
    SET(CURLOPT_HEADERFUNCTION,header);SET(CURLOPT_HEADERDATA,t);SET(CURLOPT_WRITEFUNCTION,body);SET(CURLOPT_WRITEDATA,t);
    SET(CURLOPT_ERRORBUFFER,error);SET(CURLOPT_NOPROGRESS,0L);SET(CURLOPT_XFERINFOFUNCTION,progress);SET(CURLOPT_XFERINFODATA,t);
    code=curl_easy_perform(c);if(code==CURLE_OK&&range_buffer_flush(&t->buffer))code=CURLE_WRITE_ERROR;
done:
    curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_getinfo(c,CURLINFO_REDIRECT_COUNT,&redirects);curl_easy_getinfo(c,CURLINFO_HTTP_VERSION,&version);
    curl_easy_getinfo(c,CURLINFO_OS_ERRNO,&os);curl_easy_getinfo(c,CURLINFO_APPCONNECT_TIME_T,&tls);curl_easy_getinfo(c,CURLINFO_EFFECTIVE_URL,&effective);curl_easy_getinfo(c,CURLINFO_NUM_CONNECTS,&connects);curl_easy_getinfo(c,CURLINFO_SSL_VERIFYRESULT,&verify);
    int ok=code==CURLE_OK&&status==206&&version==CURL_HTTP_VERSION_1_1&&t->received==t->size&&t->written==t->size&&
        ph_gdrive_headers_exact(&t->h,t->start,t->size,t->j->package.size_bytes)&&(archive_package(&t->j->package)?phstore_archive_url_allowed(effective):phstore_gdrive_url_allowed(effective))&&!t->disk_error;
    CURLU *url=curl_url();char *host=NULL;if(url&&effective&&!curl_url_set(url,CURLUPART_URL,effective,0))curl_url_get(url,CURLUPART_HOST,&host,0);
    const char *phase="error:gdrive_transfer";
    if(t->disk_error)phase="error:gdrive_disk_write";
    else if(code==CURLE_PEER_FAILED_VERIFICATION||code==CURLE_SSL_CACERT_BADFILE)phase="error:gdrive_tls_trust";
    else if(code==CURLE_COULDNT_RESOLVE_HOST)phase="error:gdrive_dns";
    else if(code==CURLE_COULDNT_CONNECT)phase="error:gdrive_tcp";
    else if(code==CURLE_SSL_CONNECT_ERROR)phase="error:gdrive_tls_handshake";
    else if(status && status!=206)phase="error:gdrive_http_status";
    else if(status==206&&!ph_gdrive_headers_exact(&t->h,t->start,t->size,t->j->package.size_bytes))phase="error:gdrive_content_range";
    else if(code==CURLE_OUT_OF_MEMORY)phase="error:gdrive_memory";
    snprintf(failure,64,"%s",phase);
    logline(t->j,"RANGE start=%llu size=%llu curl=%d os_errno=%ld HTTP=%ld redirects=%ld tls_us=%lld verify_result=%ld new_connections=%ld body=%llu disk=%llu exact=%s effective_host=%s reason=%s detail=%s",
        (unsigned long long)t->start,(unsigned long long)t->size,(int)code,os,status,redirects,(long long)tls,verify,connects,(unsigned long long)t->received,(unsigned long long)t->written,ok?"PASS":"FAIL",host?host:"UNKNOWN",ok?"none":phase,error);
    curl_free(host);if(url)curl_url_cleanup(url);
    range_buffer_destroy(&t->buffer);curl_slist_free_all(list);return ok?0:t->disk_error?t->disk_error:(code==CURLE_PEER_FAILED_VERIFICATION||code==CURLE_SSL_CACERT_BADFILE?EACCES:EIO);
#undef SET
}
static void *worker(void *arg){job *j=arg;CURL *c=curl_easy_init();if(!c)transfer_failure(j,ENOMEM,"error:gdrive_memory");
    while(c&&!stopped(j)){pthread_mutex_lock(&j->lock);uint64_t start=j->next;uint64_t size=start<j->package.size_bytes?j->package.size_bytes-start:0;
        if(size>GD_CHUNK)size=GD_CHUNK;j->next+=size;pthread_mutex_unlock(&j->lock);if(!size)break;
        char failure[64]="error:gdrive_transfer";
        int result=EIO;for(int attempt=0;attempt<3&&!stopped(j);attempt++){
            transfer t={.j=j,.curl=c,.start=start,.size=size};result=request(&t,failure);if(!result||result!=EIO||stopped(j))break;
            if(attempt<2){logline(j,"RETRY start=%llu next_attempt=%d delay_seconds=%d restart_piece=YES",(unsigned long long)start,attempt+2,2<<attempt);
                for(int tick=0;tick<(20<<attempt)&&!stopped(j);tick++)usleep(100000);}
        }if(result){transfer_failure(j,result,failure);break;}
        __atomic_fetch_add(&j->bytes,size,__ATOMIC_RELAXED);
    }
    if(c)curl_easy_cleanup(c);__atomic_fetch_sub(&j->running,1,__ATOMIC_RELEASE);return NULL;
}
static int write_ca(void){int fd=open(GD_CA,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -1;size_t done=0;
    while(done<sizeof(embedded_ca)){ssize_t n=write(fd,embedded_ca+done,sizeof(embedded_ca)-done);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);return -1;}done+=(size_t)n;}
    int rc=fsync(fd);close(fd);return rc;
}
typedef struct {transfer t;unsigned char *bytes;size_t used;} prefix_transfer;
static size_t prefix_body(char *data,size_t size,size_t count,void *arg) {
    prefix_transfer *p=arg;if(size&&count>SIZE_MAX/size)return 0;size_t n=size*count;
    if(p->t.h.status>=300&&p->t.h.status<400)return n;
    if(!p->t.ready || !ph_gdrive_headers_exact(&p->t.h,0,p->t.size,p->t.j->package.size_bytes) || n>p->t.size-p->used)return 0;
    memcpy(p->bytes+p->used,data,n);p->used+=n;return n;
}
int phstore_gdrive_read_prefix(const phstore_package_info_t *package,unsigned char *bytes,size_t length) {
    if(!package||!bytes||!length||length>65536||length>package->size_bytes||!phstore_gdrive_id_valid(package->file_id))return -1;
    pthread_once(&crypto_once,crypto_init);if(crypto_error)return -1;
    mkdir("/data/phstore2",0700);mkdir(GD_ROOT,0700);if(access(GD_CA,R_OK)&&write_ca())return -1;
    job j={.package=*package,.log=stderr};pthread_mutex_init(&j.lock,NULL);
    snprintf(j.url,sizeof(j.url),"https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t",package->file_id);
    CURL *c=curl_easy_init();if(!c){pthread_mutex_destroy(&j.lock);return -1;}
    prefix_transfer p={.t={.j=&j,.curl=c,.start=0,.size=length},.bytes=bytes};char range[64];snprintf(range,sizeof(range),"0-%zu",length-1);
    struct curl_slist *headers=curl_slist_append(NULL,"Accept-Encoding: identity");CURLcode rc=CURLE_OUT_OF_MEMORY;
#define PREFIX_SET(o,v) do{rc=curl_easy_setopt(c,o,v);if(rc!=CURLE_OK)goto prefix_done;}while(0)
    if(!headers)goto prefix_done;
    PREFIX_SET(CURLOPT_URL,j.url);PREFIX_SET(CURLOPT_RANGE,range);PREFIX_SET(CURLOPT_HTTPHEADER,headers);
    PREFIX_SET(CURLOPT_HTTP_VERSION,(long)CURL_HTTP_VERSION_1_1);PREFIX_SET(CURLOPT_FOLLOWLOCATION,1L);PREFIX_SET(CURLOPT_MAXREDIRS,20L);
    PREFIX_SET(CURLOPT_PROTOCOLS_STR,"https");PREFIX_SET(CURLOPT_REDIR_PROTOCOLS_STR,"https");
    PREFIX_SET(CURLOPT_SSLVERSION,(long)CURL_SSLVERSION_TLSv1_2);PREFIX_SET(CURLOPT_SSL_VERIFYHOST,2L);PREFIX_SET(CURLOPT_SSL_VERIFYPEER,1L);PREFIX_SET(CURLOPT_CAINFO,GD_CA);
    PREFIX_SET(CURLOPT_NOSIGNAL,1L);PREFIX_SET(CURLOPT_CONNECTTIMEOUT,15L);PREFIX_SET(CURLOPT_TIMEOUT,25L);PREFIX_SET(CURLOPT_HTTP_CONTENT_DECODING,0L);
    PREFIX_SET(CURLOPT_HEADERFUNCTION,header);PREFIX_SET(CURLOPT_HEADERDATA,&p);PREFIX_SET(CURLOPT_WRITEFUNCTION,prefix_body);PREFIX_SET(CURLOPT_WRITEDATA,&p);
    rc=curl_easy_perform(c);
prefix_done:
    curl_slist_free_all(headers);curl_easy_cleanup(c);pthread_mutex_destroy(&j.lock);
    return rc==CURLE_OK&&p.used==length&&ph_gdrive_headers_exact(&p.t.h,0,length,package->size_bytes)?0:-1;
#undef PREFIX_SET
}
static int job_cancelled(job *j) {
    return access(GD_CANCEL,F_OK)==0 || (!strcmp(j->package.action_type,"install_package") && phstore_install_cancel_requested());
}
int phstore_gdrive_destination(const phstore_package_info_t *p,char *out,size_t capacity) {
    const char *ext=strrchr(p->filename,'.');
    const char *root=(ext&&(!strcasecmp(ext,".exfat")||!strcasecmp(ext,".ffpfsc")||!strcasecmp(ext,".ffpkg")||!strcasecmp(ext,".ffpfs")))?"/data/homebrew":GD_ROOT;
    int n=!strcmp(p->action_type,"install_package")?snprintf(out,capacity,GD_ROOT "/%s.pkg",p->file_id):snprintf(out,capacity,"%s/%s",root,p->filename);
    return n<0||(size_t)n>=capacity?-1:0;
}
static int hash_file(int fd,char out[65],job *j){wc_Sha256 hash;unsigned char digest[32],*buf=malloc(GD_DISK_BUFFER);if(!buf)return -1;
    if(wc_InitSha256(&hash)){free(buf);return -1;}uint64_t offset=0;int rc=0;
    while(offset<j->package.size_bytes){wait_if_paused(j);if(job_cancelled(j)){stop_job(j,ECANCELED);rc=-1;break;}
        size_t n=j->package.size_bytes-offset>GD_DISK_BUFFER?GD_DISK_BUFFER:(size_t)(j->package.size_bytes-offset);
        ssize_t k=pread(fd,buf,n,(off_t)offset);if(k<0&&errno==EINTR)continue;if(k<=0||wc_Sha256Update(&hash,buf,(word32)k)){rc=-1;break;}offset+=(size_t)k;}
    if(!rc)rc=wc_Sha256Final(&hash,digest);wc_Sha256Free(&hash);free(buf);if(!rc){for(int i=0;i<32;i++)snprintf(out+2*i,3,"%02x",digest[i]);}return rc;
}
int phstore_gdrive_download(const phstore_package_info_t *package,phstore_download_progress_fn cb){
    job j={.fd=-1};char part[512]={0},final[512]={0},actual[65]={0};int rc=-1,lock_fd=-1;pthread_t threads[GD_WORKERS];int count=0;
    if(!package||!cb||(!strcmp(package->source_type,"archive_public")?!archive_package(package):!phstore_gdrive_id_valid(package->file_id))||(package->sha256[0]&&!phstore_gdrive_sha_valid(package->sha256))||!package->size_bytes||package->size_bytes>INT64_MAX)return -1;
    j.package=*package;pthread_mutex_init(&j.lock,NULL);mkdir("/data/phstore2",0700);mkdir(GD_ROOT,0700);
    lock_fd=open(GD_ROOT "/download.lock",O_RDWR|O_CREAT,0600);struct flock filelock;
    memset(&filelock,0,sizeof(filelock));filelock.l_type=F_WRLCK;filelock.l_whence=SEEK_SET;
    if(lock_fd<0||fcntl(lock_fd,F_SETLK,&filelock)<0){if(lock_fd>=0)close(lock_fd);pthread_mutex_destroy(&j.lock);cb("error:download_locked",0,package->size_bytes,0);return -1;}
    j.log=fopen(GD_ROOT "/download.log","w");if(!j.log){phstore_install_detailed_log("NATIVE_DRIVE_LOG_OPEN_FAILED",GD_ROOT "/download.log");close(lock_fd);pthread_mutex_destroy(&j.lock);cb("error:gdrive_log_open",0,package->size_bytes,0);return -1;}
    const char *ext=strrchr(package->filename,'.');
    const char *destination=(ext && (!strcasecmp(ext,".exfat") || !strcasecmp(ext,".ffpfsc") ||
        !strcasecmp(ext,".ffpkg") || !strcasecmp(ext,".ffpfs"))) ? "/data/homebrew" : GD_ROOT;
    if(mkdir(destination,0777)<0 && errno!=EEXIST){cb("error:destination_directory",0,package->size_bytes,0);goto done;}
    if(phstore_gdrive_destination(package,final,sizeof(final))){cb("error:destination_path",0,package->size_bytes,0);goto done;}snprintf(part,sizeof(part),"%s.part",final);
    if(archive_package(package))snprintf(j.url,sizeof(j.url),"%s",package->download_url);
    else snprintf(j.url,sizeof(j.url),"https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t",package->file_id);
    pthread_once(&crypto_once,crypto_init);logline(&j,"BUILD=%s BEGIN id=%s size=%llu workers=8 chunk=16777216 disk_buffer=1048576 read_ahead=65536 verify_host=2 verify_peer=1 AESNI=%d crypto_init=%d",
        PHSTORE_BUILD_ID,download_identity(package),(unsigned long long)package->size_bytes,ph_crypto_enabled,crypto_error);
    if(crypto_error||write_ca()){cb("error:gdrive_tls_init",0,package->size_bytes,0);goto done;}
    j.fd=open(final,O_RDONLY);if(j.fd>=0) {
        struct stat st;char receipt[540],saved_id[129]={0},saved_hash[65]={0};unsigned long long saved_size=0;
        snprintf(receipt,sizeof(receipt),"%s.sha256",final);FILE *saved=fopen(receipt,"r");
        int receipt_ok=saved && fscanf(saved,"%128s %llu %64s",saved_id,&saved_size,saved_hash)==3 &&
            !strcmp(saved_id,download_identity(package)) && saved_size==package->size_bytes && phstore_gdrive_sha_valid(saved_hash);
        if(saved)fclose(saved);
        /* Without either an expected digest or a matching receipt, reading the
         * entire old cache cannot establish its origin. Re-fetch owned PKG
         * staging instead of failing every subsequent install attempt. */
        int exists_ok=(package->sha256[0]||receipt_ok) && fstat(j.fd,&st)==0 && (uint64_t)st.st_size==package->size_bytes && hash_file(j.fd,actual,&j)==0 &&
            (package->sha256[0]?!strcasecmp(actual,package->sha256):(receipt_ok&&!strcasecmp(actual,saved_hash)));
        logline(&j,"CACHE_REUSE=%s expected_hash=%s",exists_ok?"PASS":"REJECT",package->sha256[0]?"PRESENT":"ABSENT_RECEIPT_REQUIRED");
        if(exists_ok){cb("completed",package->size_bytes,package->size_bytes,0);rc=0;goto done;}
        if(job_cancelled(&j)){j.error=ECANCELED;cb("cancelled",0,package->size_bytes,0);goto done;}
        if(strcmp(package->action_type,"install_package")){cb("error:destination_exists",0,package->size_bytes,0);goto done;}
        close(j.fd);j.fd=-1;
        logline(&j,"CACHE_REPLACE=REFETCH_OWNED_PKG old_final_retained_until_verified=YES");
        phstore_install_detailed_log("NATIVE_DRIVE_CACHE_REFETCH","untrusted/corrupt owned PKG cache; old final retained until verified replacement");
    }
    else if(errno!=ENOENT){cb("error:destination_open",0,package->size_bytes,0);goto done;}
    struct statvfs space;if(statvfs(destination,&space)||!space.f_frsize||space.f_bavail<((package->size_bytes+GD_DISK_BUFFER+space.f_frsize-1)/space.f_frsize)){cb("error:disk_space",0,package->size_bytes,0);goto done;}
    j.fd=open(part,O_RDWR|O_CREAT|O_TRUNC,0600);if(j.fd<0||ftruncate(j.fd,(off_t)package->size_bytes)){cb("error:disk_open",0,package->size_bytes,0);goto done;}
    double begin=seconds();for(int i=0;i<GD_WORKERS;i++){__atomic_fetch_add(&j.running,1,__ATOMIC_RELAXED);int e=phstore_thread_create(&threads[count],worker,&j);
        if(e){__atomic_fetch_sub(&j.running,1,__ATOMIC_RELAXED);
            logline(&j,"WORKER_CREATE_FAILED index=%d code=%d active_workers=%d",i,e,count);
            phstore_install_detailed_log("NATIVE_DRIVE_WORKER_CREATE_FAILED",count?"continuing with existing workers":"no worker available");
            if(!count)transfer_failure(&j,e,"error:gdrive_worker");break;}count++;}
    while(__atomic_load_n(&j.running,__ATOMIC_ACQUIRE)){if(job_cancelled(&j))stop_job(&j,ECANCELED);
        uint64_t bytes=__atomic_load_n(&j.bytes,__ATOMIC_RELAXED);double elapsed=seconds()-begin;cb(access(GD_PAUSE,F_OK)==0?"paused":"downloading",bytes,package->size_bytes,access(GD_PAUSE,F_OK)==0?0:elapsed>0?(uint64_t)(bytes/elapsed):0);usleep(100000);}
    for(int i=0;i<count;i++)pthread_join(threads[i],NULL);
    if(j.error||j.bytes!=package->size_bytes){cb(j.error==ECANCELED?"cancelled":j.error_phase[0]?j.error_phase:j.error==EACCES?"error:gdrive_tls_trust":"error:gdrive_transfer",j.bytes,package->size_bytes,0);goto done;}
    cb("verifying",j.bytes,package->size_bytes,0);
    if(fsync(j.fd)||hash_file(j.fd,actual,&j)||(package->sha256[0]&&strcasecmp(actual,package->sha256))){cb(j.error==ECANCELED?"cancelled":"error:sha256_mismatch",j.bytes,package->size_bytes,0);goto done;}
    struct stat st;if(fstat(j.fd,&st)||(uint64_t)st.st_size!=package->size_bytes){cb("error:final_size",j.bytes,package->size_bytes,0);goto done;}
    if(rename(part,final)){cb("error:final_rename",j.bytes,package->size_bytes,0);goto done;}
    logline(&j,"FINAL_SHA256=%s sha256=%s bytes=%llu pwrite_calls=%llu seconds=%.6f MiB_s=%.6f",package->sha256[0]?"PASS":"COMPUTED_NO_EXPECTED",actual,(unsigned long long)j.bytes,(unsigned long long)j.pwrites,seconds()-begin,j.bytes/(seconds()-begin)/1048576.0);
    char receipt[540],temporary[550];snprintf(receipt,sizeof(receipt),"%s.sha256",final);snprintf(temporary,sizeof(temporary),"%s.tmp",receipt);
    FILE *saved=fopen(temporary,"w");int receipt_ok=0;
    if(saved){receipt_ok=fprintf(saved,"%s %llu %s\n",download_identity(package),(unsigned long long)package->size_bytes,actual)>0;
        if(fflush(saved)||fsync(fileno(saved)))receipt_ok=0;if(fclose(saved))receipt_ok=0;}
    if(!receipt_ok || rename(temporary,receipt)){unlink(temporary);logline(&j,"CACHE_RECEIPT=FAILED errno=%d",errno);}
    else logline(&j,"CACHE_RECEIPT=WRITTEN");
    cb("completed",j.bytes,package->size_bytes,0);rc=0;
done:
    if(j.fd>=0){close(j.fd);j.fd=-1;}
    if(final[0] && ((!strcmp(package->action_type,"install_package")&&phstore_install_cancel_requested()) || access(GD_CANCEL ".delete",F_OK)==0)) {
        char side[550];if(part[0])unlink(part);unlink(final);snprintf(side,sizeof(side),"%s.sha256",final);unlink(side);
        rc=-1;j.error=ECANCELED;cb("cancelled",0,package->size_bytes,0);
    }
    logline(&j,"END result=%d error=%d partial_resume_supported=NO",rc,j.error);if(j.fd>=0)close(j.fd);fclose(j.log);close(lock_fd);pthread_mutex_destroy(&j.lock);return rc;
}

/* Small public cover requests have their own curl handle/file and never touch
 * installer sockets, download markers or the large-file transfer gate. */
typedef struct {FILE *file;uint64_t bytes,limit;ph_gdrive_headers headers;} cover_transfer;
static int cover_url_allowed(const char *url) {
    if(!url || strncmp(url,"https://",8))return 0;
    for(const unsigned char *p=(const unsigned char *)url;*p;p++)if(*p<=32||*p>=127||*p=='\\'||*p=='#')return 0;
    const char *p=url+8;size_t n=strcspn(p,"/?");
    return (n==strlen("image.api.playstation.com")&&!strncasecmp(p,"image.api.playstation.com",n)) ||
           (n==strlen("cdn.prosperopatches.com")&&!strncasecmp(p,"cdn.prosperopatches.com",n));
}
static size_t cover_header(char *data,size_t a,size_t b,void *arg) {
    cover_transfer *t=arg;if(a&&b>SIZE_MAX/a)return 0;size_t n=a*b;
    if(n>=5&&!memcmp(data,"HTTP/",5)) {
        char line[80];size_t k=n<79?n:79;memcpy(line,data,k);line[k]=0;long status=0;
        if(sscanf(line,"HTTP/%*s %ld",&status)!=1||t->bytes)return 0;
        memset(&t->headers,0,sizeof(t->headers));t->headers.status=status;
    } else if(n>=9&&!strncasecmp(data,"Location:",9)) {
        char url[2048];size_t start=9,end=n;while(start<end&&(data[start]==' '||data[start]=='\t'))start++;
        while(end>start&&(data[end-1]=='\r'||data[end-1]=='\n'||data[end-1]==' '||data[end-1]=='\t'))end--;
        if(end-start>=sizeof(url))return 0;memcpy(url,data+start,end-start);url[end-start]=0;
        if(!cover_url_allowed(url))return 0;
    } else if((n>=15&&!strncasecmp(data,"Content-Length:",15)) ||
              (n>=17&&!strncasecmp(data,"Content-Encoding:",17)))ph_gdrive_header_line(&t->headers,data,n);
    return n;
}
static size_t cover_body(char *data,size_t a,size_t b,void *arg) {
    cover_transfer *t=arg;if(a&&b>SIZE_MAX/a)return 0;size_t n=a*b;
    if(t->headers.status>=300&&t->headers.status<400)return n;
    if(t->headers.status!=200 || t->headers.bad || t->headers.encoding || t->headers.cl>1 ||
       n>t->limit-t->bytes || (t->headers.cl&&t->headers.length>t->limit))return 0;
    size_t written=fwrite(data,1,n,t->file);t->bytes+=written;return written;
}
int phstore_public_cover_fetch(const char *url,const char *path,uint64_t limit,uint64_t *bytes,int *http_status) {
    *bytes=0;*http_status=0;if(!cover_url_allowed(url)||!limit)return -EINVAL;
    pthread_once(&crypto_once,crypto_init);if(crypto_error)return -EIO;
    cover_transfer t={.limit=limit};t.file=fopen(path,"wb");if(!t.file)return -errno;
    CURL *c=curl_easy_init();if(!c){fclose(t.file);unlink(path);return -ENOMEM;}
    char detail[CURL_ERROR_SIZE]={0};struct curl_blob ca={(void *)embedded_ca,sizeof(embedded_ca),CURL_BLOB_COPY};
    struct curl_slist *list=curl_slist_append(NULL,"Accept-Encoding: identity");CURLcode code=CURLE_OUT_OF_MEMORY;
    if(!list)goto end;
#define COVER_SET(o,v) do{code=curl_easy_setopt(c,o,v);if(code!=CURLE_OK)goto end;}while(0)
    COVER_SET(CURLOPT_USERAGENT,"PHStore2");COVER_SET(CURLOPT_URL,url);COVER_SET(CURLOPT_HTTP_VERSION,(long)CURL_HTTP_VERSION_1_1);
    COVER_SET(CURLOPT_FOLLOWLOCATION,1L);COVER_SET(CURLOPT_MAXREDIRS,10L);COVER_SET(CURLOPT_CONNECTTIMEOUT,20L);COVER_SET(CURLOPT_TIMEOUT,60L);
    COVER_SET(CURLOPT_HTTPHEADER,list);COVER_SET(CURLOPT_HTTP_CONTENT_DECODING,0L);COVER_SET(CURLOPT_FAILONERROR,1L);
    COVER_SET(CURLOPT_SSL_VERIFYHOST,2L);COVER_SET(CURLOPT_SSL_VERIFYPEER,1L);COVER_SET(CURLOPT_SSLVERSION,(long)CURL_SSLVERSION_TLSv1_2);
    COVER_SET(CURLOPT_CAINFO_BLOB,&ca);COVER_SET(CURLOPT_PROTOCOLS_STR,"https");COVER_SET(CURLOPT_REDIR_PROTOCOLS_STR,"https");
    COVER_SET(CURLOPT_NOSIGNAL,1L);COVER_SET(CURLOPT_IPRESOLVE,(long)CURL_IPRESOLVE_V4);COVER_SET(CURLOPT_PROXY,"");COVER_SET(CURLOPT_NOPROXY,"*");
    COVER_SET(CURLOPT_HEADERFUNCTION,cover_header);COVER_SET(CURLOPT_HEADERDATA,&t);COVER_SET(CURLOPT_WRITEFUNCTION,cover_body);COVER_SET(CURLOPT_WRITEDATA,&t);COVER_SET(CURLOPT_ERRORBUFFER,detail);
    code=curl_easy_perform(c);
end:;
    long status=0;char *effective=NULL;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_getinfo(c,CURLINFO_EFFECTIVE_URL,&effective);*http_status=(int)status;
    int ok=code==CURLE_OK && status==200 && cover_url_allowed(effective) && t.bytes>0 &&
        !t.headers.bad && !t.headers.encoding && t.headers.cl<=1 && (!t.headers.cl || t.headers.length==t.bytes);
    if(fflush(t.file)||fsync(fileno(t.file)))ok=0;if(fclose(t.file))ok=0;
    char log[512];snprintf(log,sizeof(log),"curl=%d HTTP=%ld bytes=%llu verify_host=2 verify_peer=1 detail=%.256s",(int)code,status,(unsigned long long)t.bytes,detail);
    phstore_install_detailed_log("PH_PS5_COVER_HTTPS",log);curl_slist_free_all(list);curl_easy_cleanup(c);
    if(!ok){unlink(path);return status&&status!=200?-(int)status:-EIO;}*bytes=t.bytes;return 0;
#undef COVER_SET
}
