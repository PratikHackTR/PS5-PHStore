#define _CRT_SECURE_NO_WARNINGS
#include "phstore_gdrive_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void line(ph_gdrive_headers *h,const char *s){ph_gdrive_header_line(h,s,strlen(s));}
int main(int argc,char **argv){ph_gdrive_headers h={.status=206};
    assert(phstore_gdrive_id_valid("1H_iIPrvBa8NSgsutbnkxr_a_JHMi-fN3"));assert(!phstore_gdrive_id_valid("id&x=1"));
    assert(phstore_gdrive_sha_valid("77aac9bdb0e37992535702e68bbe0c5828773cc0a3c0a4e8e6f028de7fd66f5b"));assert(!phstore_gdrive_sha_valid("abcd"));
    assert(phstore_gdrive_url_allowed("https://drive.usercontent.google.com/download?id=a"));
    assert(phstore_gdrive_url_allowed("https://a.googleusercontent.com/a"));
    assert(!phstore_gdrive_url_allowed("https://drive.usercontent.google.com.evil/a"));
    assert(!phstore_gdrive_url_allowed("https://evil@drive.google.com/a"));assert(!phstore_gdrive_url_allowed("http://drive.google.com/a"));
    line(&h,"Content-Range: bytes 65536-131071/125369652\r\n");line(&h,"Content-Length: 65536\r\n");line(&h,"Content-Encoding: identity\r\n");
    assert(ph_gdrive_headers_exact(&h,65536,65536,125369652));
    char long_header[8192];memset(long_header,'a',sizeof(long_header));
    memcpy(long_header,"Access-Control-Allow-Headers:",29);
    ph_gdrive_header_line(&h,long_header,4709);
    assert(ph_gdrive_headers_exact(&h,65536,65536,125369652));
    ph_gdrive_headers invalid=h;memcpy(long_header,"Content-Range:",14);ph_gdrive_header_line(&invalid,long_header,4709);
    assert(invalid.bad);invalid=h;memcpy(long_header,"Location:",9);ph_gdrive_header_line(&invalid,long_header,4709);assert(invalid.bad);
    assert(!ph_gdrive_headers_exact(&h,0,65536,125369652));
    assert(!ph_gdrive_headers_exact(&h,65536,65536,125369651));h.status=200;assert(!ph_gdrive_headers_exact(&h,65536,65536,125369652));h.status=206;
    line(&h,"Content-Length: 65536\r\n");assert(!ph_gdrive_headers_exact(&h,65536,65536,125369652));
    memset(&h,0,sizeof(h));line(&h,"Content-Range: bytes 0-1/18446744073709551616\r\n");assert(h.bad);
    memset(&h,0,sizeof(h));line(&h,"Location: https://evil.invalid/a\r\n");assert(h.bad);
    memset(&h,0,sizeof(h));line(&h,"Content-Encoding: gzip\r\n");assert(h.encoding);
    if(argc>1){FILE *f=fopen(argv[1],"rb");assert(f);memset(&h,0,sizeof(h));
        while(fgets(long_header,sizeof(long_header),f)){
            if(!strncmp(long_header,"HTTP/",5)){long status=0;assert(sscanf(long_header,"HTTP/%*s %ld",&status)==1);memset(&h,0,sizeof(h));h.status=status;}
            else ph_gdrive_header_line(&h,long_header,strlen(long_header));
        }fclose(f);assert(ph_gdrive_headers_exact(&h,0,1,125369652));puts("REAL_GDRIVE_HEADERS_REPLAY_PASS long_CORS exact_206_CR_CL");
    }
    puts("GDRIVE_PROTOCOL_HOST_PASS exact_range duplicate_length overflow encoding redirect_host ID SHA long_CORS critical_oversize_rejection");return 0;
}
