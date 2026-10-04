#define PHSTORE_CATALOG_HOST_TEST 1
#include "../src/phstore_catalog.c"
#include "../src/phstore_url.c"
static char *fixture;
char *phstore_catalog_copy(size_t *n){
    size_t size=strlen(fixture);char *copy=malloc(size+1);
    if(copy){memcpy(copy,fixture,size+1);if(n)*n=size;}return copy;
}
int main(int argc,char **argv){
    if(argc!=2)return 2;FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
    if(n<=0){fclose(f);return 2;}fixture=malloc((size_t)n+1);if(!fixture){fclose(f);return 2;}
    if(fread(fixture,1,(size_t)n,f)!=(size_t)n){fclose(f);free(fixture);return 2;}fclose(f);fixture[n]=0;
    uint32_t schema=0,version=0;size_t count=0;char generated[64];const char *error=NULL;
    if(!validate_catalog(fixture,(size_t)n,&schema,&version,&count,generated,&error)){fprintf(stderr,"catalog_invalid:%s\n",error);free(fixture);return 1;}
    json_parser_t p={.json=fixture,.length=(size_t)n};int root=-1;parse_value(&p,0,&root);
    int games=object_get(&p,root,"games");size_t checked=0,sp=0,ph=0;
    for(int game=p.tokens[games].first_child;game>=0;game=p.tokens[game].next){
        int packages=object_get(&p,game,"packages");
        for(int item=p.tokens[packages].first_child;item>=0;item=p.tokens[item].next){
            char id[96],diagnostic[64];phstore_package_info_t package;
            if(!copy_raw_string(&p,object_get(&p,item,"id"),id,sizeof(id)))return 1;
            if(phstore_catalog_find_install_package(id,&package,diagnostic)!=1){fprintf(stderr,"lookup_failed:%s:%s\n",id,diagnostic);return 1;}
            if(!strncmp(id,"sp-",3)){
                sp++;const char *ext=strrchr(package.filename,'.');
                if(ext && !strcmp(ext,".pkg")){
                    if(phstore_catalog_resolve_package_source(&package,diagnostic) || strncmp(package.download_url,"http://148.135.181.3/phstore2/pkg/",31))return 1;
                }
            }else {
                ph++;
                if(!strcmp(package.source_type,"google_drive_public")) {
                    if(strcmp(package.action_type,"download_file")||strcmp(package.file_id,"1H_iIPrvBa8NSgsutbnkxr_a_JHMi-fN3")||
                       strcmp(package.sha256,"77aac9bdb0e37992535702e68bbe0c5828773cc0a3c0a4e8e6f028de7fd66f5b")||
                       phstore_catalog_resolve_package_source(&package,diagnostic)||
                       strcmp(package.download_url,"https://drive.usercontent.google.com/download?id=1H_iIPrvBa8NSgsutbnkxr_a_JHMi-fN3&export=download&confirm=t"))return 1;
                }
            }
            checked++;
        }
    }
    printf("catalog_validate=PASS games=%zu packages=%zu ph=%zu sp=%zu\n",count,checked,ph,sp);
    free(p.tokens);free(fixture);return 0;
}
