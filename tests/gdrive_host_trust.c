#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <wolfssl/ssl.h>
#include "crypto_control.h"
#include "embedded_ca.h"
int probe_entropy(unsigned char *p,unsigned int n){return BCryptGenRandom(NULL,p,n,BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0?0:-1;}
int main(int argc,char **argv){const char *mode=argc>1?argv[1]:"tls12";WSADATA w;WSAStartup(MAKEWORD(2,2),&w);ph_crypto_cpu_init();ph_crypto_enabled=0;wolfSSL_Init();
    WOLFSSL_CTX *ctx=wolfSSL_CTX_new(!strcmp(mode,"tls13")?wolfTLSv1_3_client_method():wolfTLSv1_2_client_method());if(!ctx)return 2;
    int ca=!strcmp(mode,"empty_ca")?0:wolfSSL_CTX_load_verify_buffer(ctx,embedded_ca,sizeof(embedded_ca),WOLFSSL_FILETYPE_PEM);printf("HOST_CA_LOAD=%d mode=%s\n",ca,mode);
    wolfSSL_CTX_set_verify(ctx,WOLFSSL_VERIFY_PEER,NULL);struct addrinfo hint={0},*addresses=NULL;hint.ai_family=AF_INET;hint.ai_socktype=SOCK_STREAM;
    const char *host="drive.usercontent.google.com";if(getaddrinfo(host,"443",&hint,&addresses))return 2;
    SOCKET fd=INVALID_SOCKET;for(struct addrinfo *a=addresses;a;a=a->ai_next){fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol);DWORD timeout=15000;
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,(char *)&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,(char *)&timeout,sizeof(timeout));
        if(!connect(fd,a->ai_addr,(int)a->ai_addrlen))break;closesocket(fd);fd=INVALID_SOCKET;}
    freeaddrinfo(addresses);if(fd==INVALID_SOCKET)return 2;
    WOLFSSL *ssl=wolfSSL_new(ctx);wolfSSL_UseSNI(ssl,WOLFSSL_SNI_HOST_NAME,host,(unsigned short)strlen(host));wolfSSL_check_domain_name(ssl,!strcmp(mode,"wrong_host")?"wrong.invalid":host);wolfSSL_set_fd(ssl,(int)fd);
    int rc=wolfSSL_connect(ssl),e=wolfSSL_get_error(ssl,rc);char detail[160];wolfSSL_ERR_error_string_n((unsigned long)e,detail,sizeof(detail));
    printf("HOST_TLS_VERIFY_PEER_AND_HOST=%s rc=%d error=%d detail=%s version=%s\n",rc==WOLFSSL_SUCCESS?"PASS":"FAIL",rc,e,detail,wolfSSL_get_version(ssl));
    wolfSSL_free(ssl);wolfSSL_CTX_free(ctx);closesocket(fd);WSACleanup();return rc==WOLFSSL_SUCCESS?0:1;
}
