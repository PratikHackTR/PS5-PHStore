#define _CRT_SECURE_NO_WARNINGS
#include "phstore_gdrive_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    char data[65536];size_t n=fread(data,1,sizeof(data),f);assert(!ferror(f));fclose(f);
    assert(n==2009);assert(ph_gdrive_response_is_quota(200,data,n));
    assert(ph_gdrive_response_is_quota(403,data,n));assert(!ph_gdrive_response_is_quota(206,data,n));
    assert(!ph_gdrive_response_is_quota(200,NULL,n));assert(!ph_gdrive_response_is_quota(200,data,0));
    assert(!ph_gdrive_response_is_quota(200,data,65537));
    const char api[]="{\"error\":\"downloadQuotaExceeded\"}";
    assert(ph_gdrive_response_is_quota(429,api,sizeof(api)-1));
    const char normal[]="<html><title>Google Drive - Download</title><form id=download-form></form></html>";
    assert(!ph_gdrive_response_is_quota(200,normal,sizeof(normal)-1));
    assert(!ph_gdrive_response_is_quota(200,"Google Drive - Quota exceeded",8));
    ph_gdrive_headers h={0};const char ct[]="Content-Type: text/html; charset=utf-8\r\n";
    ph_gdrive_header_line(&h,ct,sizeof(ct)-1);assert(h.html);
    const char bin[]="Content-Type: application/octet-stream\r\n";
    ph_gdrive_header_line(&h,bin,sizeof(bin)-1);assert(!h.html);
    const char invalid[]="Content-Type: text/htmlsomething\r\n";
    ph_gdrive_header_line(&h,invalid,sizeof(invalid)-1);assert(!h.html);
    puts("GDRIVE_QUOTA_PASS real 2009-byte Google page; status, length, type and non-quota boundaries");return 0;
}
