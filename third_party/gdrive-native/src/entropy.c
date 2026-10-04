#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
/* PS5 SDK hello_dlfcn sample API. Never seed TLS from time/rand(). */
static void *module;
static int (*random_bytes)(uint8_t *,size_t);
int probe_entropy_init(void) {
    module=dlopen("libSceRandom.sprx",RTLD_LAZY);
    if(!module)return -1;
    random_bytes=(int (*)(uint8_t *,size_t))dlsym(module,"sceRandomGetRandomNumber");
    return random_bytes?0:-2;
}
int probe_entropy(unsigned char *output,unsigned int size) {
    if(!random_bytes)return -1;
    return random_bytes(output,(size_t)size)==0?0:-1;
}
void probe_entropy_term(void) {
    random_bytes=NULL;if(module){dlclose(module);module=NULL;}
}
