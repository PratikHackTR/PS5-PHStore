#include "phstore_gdrive_protocol.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
int phstore_gdrive_id_valid(const char *s) {
    size_t n=s?strlen(s):0; if(!n||n>128)return 0;
    for(size_t i=0;i<n;i++)if(!isalnum((unsigned char)s[i])&&s[i]!='_'&&s[i]!='-')return 0;
    return 1;
}
int phstore_gdrive_sha_valid(const char *s) {
    if(!s||strlen(s)!=64)return 0;
    for(size_t i=0;i<64;i++)if(!isxdigit((unsigned char)s[i]))return 0;
    return 1;
}
int phstore_gdrive_url_allowed(const char *s) {
    if(!s||strncmp(s,"https://",8))return 0;
    for(const unsigned char *c=(const unsigned char *)s;*c;c++)if(*c<=32||*c>=127||*c=='\\'||*c=='#')return 0;
    const char *p=s+8;size_t n=strcspn(p,"/?");char host[256];
    if(!n||n>=sizeof(host)||memchr(p,'@',n)||memchr(p,':',n))return 0;
    for(size_t i=0;i<n;i++)host[i]=(char)tolower((unsigned char)p[i]);host[n]=0;
    const char *suffix=".googleusercontent.com";size_t k=strlen(suffix);
    return !strcmp(host,"drive.usercontent.google.com")||!strcmp(host,"drive.google.com")||
           (n>k&&!strcmp(host+n-k,suffix));
}
int phstore_archive_url_allowed(const char *s) {
    if(!s || strncmp(s,"https://",8))return 0;
    for(const unsigned char *c=(const unsigned char *)s;*c;c++)if(*c<=32 || *c>=127 || *c=='\\' || *c=='#')return 0;
    const char *p=s+8;size_t n=strcspn(p,"/?");char host[256];
    if(!n || n>=sizeof(host) || memchr(p,'@',n) || memchr(p,':',n))return 0;
    for(size_t i=0;i<n;i++){if(!isalnum((unsigned char)p[i]) && p[i]!='.' && p[i]!='-')return 0;host[i]=(char)tolower((unsigned char)p[i]);}host[n]=0;
    const char *suffix=".archive.org";size_t k=strlen(suffix);
    return !strcmp(host,"archive.org") || (n>k && !strcmp(host+n-k,suffix));
}
static int pref(const char *s,const char *p){while(*p)if(tolower((unsigned char)*s++)!=tolower((unsigned char)*p++))return 0;return 1;}
static int number(char **p,uint64_t *v){char *e;if(!isdigit((unsigned char)**p))return 0;errno=0;*v=strtoull(*p,&e,10);if(errno||e==*p)return 0;*p=e;return 1;}
void ph_gdrive_header_line(ph_gdrive_headers *h,const char *s,size_t n) {
    char b[1024],*p;
    if(n>=sizeof(b)){
        /* Google sends multi-KiB CORS metadata. Only headers that constrain
         * transfer integrity or redirect policy need this parsing buffer. */
        if(pref(s,"Content-Range:")||pref(s,"Content-Length:")||
           pref(s,"Content-Encoding:")||pref(s,"Location:"))h->bad=1;
        return;
    }
    memcpy(b,s,n);b[n]=0;
    while(n&&(b[n-1]=='\r'||b[n-1]=='\n'||b[n-1]==' '||b[n-1]=='\t'))b[--n]=0;
    if(pref(b,"Content-Range:")){h->cr++;p=b+14;while(*p==' '||*p=='\t')p++;
        if(!pref(p,"bytes ")){h->bad=1;return;}p+=6;
        if(!number(&p,&h->start)||*p++!='-'||!number(&p,&h->end)||*p++!='/'||!number(&p,&h->total)||*p)h->bad=1;
    }else if(pref(b,"Content-Length:")){h->cl++;p=b+15;while(*p==' '||*p=='\t')p++;if(!number(&p,&h->length)||*p)h->bad=1;
    }else if(pref(b,"Content-Encoding:")){p=b+17;while(*p==' '||*p=='\t')p++;if(strcmp(p,"identity"))h->encoding=1;
    }else if(pref(b,"Location:")){p=b+9;while(*p==' '||*p=='\t')p++;if(!(h->allow_archive?phstore_archive_url_allowed(p):phstore_gdrive_url_allowed(p)))h->bad=1;}
}
int ph_gdrive_headers_exact(const ph_gdrive_headers *h,uint64_t start,uint64_t size,uint64_t total) {
    return size&&start<total&&size<=total-start&&h->status==206&&!h->bad&&!h->encoding&&h->cr==1&&h->cl==1&&
        h->start==start&&h->end==start+size-1&&h->total==total&&h->length==size;
}
