"""Build supplied modified curl/wolfSSL sources for an external Windows PS5 SDK."""
import argparse,os,shutil,subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
NATIVE=ROOT/'third_party/gdrive-native'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',type=Path,default=os.environ.get('PS5_PAYLOAD_SDK'))
    p.add_argument('--llvm',type=Path,default=os.environ.get('LLVM_BIN',r'C:\Program Files\LLVM\bin'))
    p.add_argument('--cmake',default='cmake');p.add_argument('--ninja',default='ninja')
    p.add_argument('--jobs',type=int,default=8)
    p.add_argument('--no-install',action='store_true',help='Build only; keep supplied .a files intact')
    a=p.parse_args()
    if not a.sdk:p.error('Pass --sdk or set PS5_PAYLOAD_SDK')
    a.sdk=a.sdk.resolve();a.llvm=a.llvm.resolve()
    if not 1<=a.jobs<=64:p.error('--jobs must be 1..64')
    cmake=shutil.which(a.cmake);ninja=shutil.which(a.ninja)
    if not cmake or not ninja:p.error('CMake and Ninja must be installed or passed as explicit paths')
    for tool in [a.llvm/'clang.exe',a.llvm/'llvm-ar.exe',a.llvm/'llvm-ranlib.exe',a.sdk/'win/prospero-lld.exe',a.sdk/'target/lib/crt1.o']:
        if not tool.is_file():p.error('Required external tool missing: '+str(tool))
    build=ROOT/'build/native-deps';logs=build/'logs';logs.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy();env['PS5_PAYLOAD_SDK']=str(a.sdk);env['PATH']=str(a.llvm)+os.pathsep+str(a.sdk/'win')+os.pathsep+env.get('PATH','')
    def run(args,name):
        log=logs/(name+'.log')
        with log.open('w',encoding='utf-8') as f:
            r=subprocess.run(list(map(str,args)),cwd=ROOT,env=env,stdout=f,stderr=subprocess.STDOUT)
        if r.returncode:
            print(log.read_text(encoding='utf-8',errors='replace')[-10000:])
            raise SystemExit(f'{name} failed; see {log}')
        print(name+'=PASS',flush=True)
    entropy=build/'entropy-link.o';crypto=build/'crypto-link.o'
    for src,obj in [('entropy.c',entropy),('crypto_control.c',crypto)]:
        run([a.llvm/'clang.exe','-target','x86_64-sie-ps5','-isysroot',a.sdk,'-isystem',a.sdk/'target/include','-fPIE','-DWOLFSSL_USER_SETTINGS','-I'+str(NATIVE/'config'),'-I'+str(NATIVE/'include'),'-I'+str(NATIVE/'src'),'-c',NATIVE/'src'/src,'-o',obj],'compile-'+src)
    # Quotes in flags preserve SDK/settings paths containing spaces.
    flags=f'-isystem "{a.sdk.as_posix()}/target/include" -fPIE -ffunction-sections -fdata-sections -DWOLFSSL_USER_SETTINGS -I"{(NATIVE/"config").as_posix()}"'
    toolchain=NATIVE/'config/ps5-toolchain.cmake'
    common=[cmake,'-G','Ninja',f'-DCMAKE_MAKE_PROGRAM={ninja}',f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',f'-DSDK={a.sdk.as_posix()}',f'-DLLVM={a.llvm.as_posix()}',f'-DPH_ENTROPY_OBJECT={entropy.as_posix()} {crypto.as_posix()}','-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_C_FLAGS={flags}',f'-DCMAKE_ASM_FLAGS={flags}']
    wolf=build/'wolfssl';curl=build/'curl'
    run(common+['-S',NATIVE/'vendor/wolfssl-5.9.4-stable','-B',wolf,'-DBUILD_SHARED_LIBS=OFF','-DWOLFSSL_USER_SETTINGS=yes','-DWOLFSSL_EXAMPLES=no','-DWOLFSSL_CRYPT_TESTS=no','-DWOLFSSL_OPENSSLALL=yes','-DWOLFSSL_ALT_CERT_CHAINS=yes','-DWOLFSSL_AESNI=ON'],'wolfssl-configure')
    run([cmake,'--build',wolf,'--parallel',a.jobs],'wolfssl-build')
    wolflib=next(wolf.rglob('libwolfssl.a'))
    run(common+['-S',NATIVE/'vendor/curl-8.22.0','-B',curl,'-DBUILD_SHARED_LIBS=OFF','-DBUILD_STATIC_LIBS=ON','-DBUILD_CURL_EXE=OFF','-DBUILD_TESTING=OFF','-DCURL_USE_WOLFSSL=ON','-DCURL_USE_OPENSSL=OFF',f'-DWOLFSSL_INCLUDE_DIR={NATIVE}/vendor/wolfssl-5.9.4-stable',f'-DWOLFSSL_LIBRARY={wolflib}','-DCURL_USE_PKGCONFIG=OFF','-DCURL_USE_CMAKECONFIG=OFF','-DCURL_USE_LIBPSL=OFF','-DCURL_USE_LIBSSH2=OFF','-DCURL_ZLIB=OFF','-DCURL_BROTLI=OFF','-DCURL_ZSTD=OFF','-DUSE_NGHTTP2=OFF','-DUSE_LIBIDN2=OFF','-DENABLE_THREADED_RESOLVER=OFF','-DENABLE_ARES=OFF','-DCURL_DISABLE_LDAP=ON','-DCURL_DISABLE_LDAPS=ON','-DHTTP_ONLY=ON','-DCURL_CA_BUNDLE=none','-DCURL_CA_PATH=none',f'-DCMAKE_C_FLAGS={flags} -DWOLFSSL_NO_OPTIONS_H -I"{wolf.as_posix()}"'],'curl-configure')
    run([cmake,'--build',curl,'--parallel',a.jobs],'curl-build')
    curllib=next(curl.rglob('libcurl.a'))
    if not a.no_install:
        for source,name in [(wolflib,'libwolfssl.a'),(curllib,'libcurl.a')]:
            shutil.copy2(source,NATIVE/'lib'/name)
        print('INSTALLED libraries in this checkout; run tools/build.ps1 next')
    else:print('BUILD_ONLY=PASS; supplied libraries unchanged')
    print('PS5_RUNTIME=NOT_TESTED')

if __name__=='__main__':main()
