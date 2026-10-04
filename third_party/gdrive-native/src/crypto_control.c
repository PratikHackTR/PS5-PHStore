#include "crypto_control.h"
#include <cpuid.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
int ph_crypto_enabled;
static unsigned cpu_ecx;
static int hw_ready;
static uint64_t sw_calls,sw_bytes,hw_calls,hw_bytes;
void ph_crypto_cpu_init(void){unsigned a,b,c,d;cpu_ecx=0;if(__get_cpuid(1,&a,&b,&c,&d))cpu_ecx=c;hw_ready=(cpu_ecx&((1u<<25)|(1u<<1)|(1u<<9)))==((1u<<25)|(1u<<1)|(1u<<9));}
int ph_crypto_hardware_ready(void){return hw_ready;}
unsigned ph_crypto_cpu_ecx(void){return cpu_ecx;}
void ph_crypto_note_gcm(int hardware,unsigned n){if(hardware){__atomic_fetch_add(&hw_calls,1,__ATOMIC_RELAXED);__atomic_fetch_add(&hw_bytes,n,__ATOMIC_RELAXED);}else{__atomic_fetch_add(&sw_calls,1,__ATOMIC_RELAXED);__atomic_fetch_add(&sw_bytes,n,__ATOMIC_RELAXED);}}
void ph_crypto_reset_stats(void){sw_calls=sw_bytes=hw_calls=hw_bytes=0;}
void ph_crypto_stats(uint64_t *sc,uint64_t *sb,uint64_t *hc,uint64_t *hb){*sc=__atomic_load_n(&sw_calls,__ATOMIC_RELAXED);*sb=__atomic_load_n(&sw_bytes,__ATOMIC_RELAXED);*hc=__atomic_load_n(&hw_calls,__ATOMIC_RELAXED);*hb=__atomic_load_n(&hw_bytes,__ATOMIC_RELAXED);}
static const unsigned char c128[16]={0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78};
static const unsigned char t128[16]={0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf};
static const unsigned char c256[16]={0xce,0xa7,0x40,0x3d,0x4d,0x60,0x6b,0x6e,0x07,0x4e,0xc5,0xd3,0xba,0xf3,0x9d,0x18};
static const unsigned char t256[16]={0xd0,0xd1,0xc8,0xa7,0x99,0x99,0x6b,0xf0,0x26,0x5b,0x98,0xb5,0xd4,0x8a,0xb9,0x19};
int ph_crypto_selftest(void){unsigned char key[32]={0},iv[12]={0},plain[16]={0},out[16],tag[16],decoded[16];
 for(unsigned n=16;n<=32;n+=16){Aes aes;int rc=wc_AesInit(&aes,NULL,INVALID_DEVID);if(rc)return rc;rc=wc_AesGcmSetKey(&aes,key,n);
  if(!rc&&aes.use_aesni!=(ph_crypto_enabled&&hw_ready))rc=-1001;
  if(!rc)rc=wc_AesGcmEncrypt(&aes,out,plain,16,iv,12,tag,16,NULL,0);
  if(!rc&&(memcmp(out,n==16?c128:c256,16)||memcmp(tag,n==16?t128:t256,16)))rc=-1002;
  if(!rc)rc=wc_AesGcmDecrypt(&aes,decoded,out,16,iv,12,tag,16,NULL,0);
  if(!rc&&memcmp(decoded,plain,16))rc=-1003;
  if(!rc){tag[0]^=1;if(wc_AesGcmDecrypt(&aes,decoded,out,16,iv,12,tag,16,NULL,0)!=AES_GCM_AUTH_E)rc=-1004;}
  wc_AesFree(&aes);if(rc)return rc;
 }return 0;
}
static double tick(void){
#ifdef _WIN32
 LARGE_INTEGER t,f;if(!QueryPerformanceCounter(&t)||!QueryPerformanceFrequency(&f))return -1;return (double)t.QuadPart/f.QuadPart;
#else
 struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;return (double)t.tv_sec+t.tv_nsec/1e9;
#endif
}
int ph_crypto_benchmark(double *seconds,double *rate){unsigned char key[16]={0},iv[12]={0},plain[16384],cipher[16384],out[16384],tag[16];Aes aes;memset(plain,0x6d,sizeof(plain));*seconds=*rate=-1;
 int rc=wc_AesInit(&aes,NULL,INVALID_DEVID);if(rc)return rc;rc=wc_AesGcmSetKey(&aes,key,16);if(!rc)rc=wc_AesGcmEncrypt(&aes,cipher,plain,sizeof(plain),iv,12,tag,16,NULL,0);
 double begin=tick();if(begin<0&&!rc)rc=-1005;
 for(int i=0;i<4096&&!rc;i++)rc=wc_AesGcmDecrypt(&aes,out,cipher,sizeof(cipher),iv,12,tag,16,NULL,0);
 double end=tick();if(!rc&&memcmp(out,plain,sizeof(out)))rc=-1006;
 if(!rc&&end>begin){*seconds=end-begin;*rate=64.0/(*seconds);}wc_AesFree(&aes);return rc;
}
