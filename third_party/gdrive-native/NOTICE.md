# Source-built native HTTPS dependencies

curl 8.22.0 and wolfSSL 5.9.4-stable source trees, settings and native PS5 static libraries are supplied here. curl includes the pending/drain adapter. wolfSSL includes read-ahead and runtime-gated AES-NI instrumentation; alternate certificate chains are enabled. Native PH HTTPS uses peer and hostname verification.

Rebuild with `tools/rebuild_native_deps.py`; instructions and external SDK requirements are in `BUILDING.md`. This native downloader contains no Spectrum ELF/code blob. The legacy SP helper is a separate binary component with its own incomplete source/license provenance.

curl: CURL_COPYING and vendor notices. wolfSSL: WOLFSSL_COPYING and vendor notices. Vendored public test credentials/certificates are upstream test fixtures.

Original source archive references (before local source modifications):
- curl https://curl.se/download/curl-8.22.0.tar.xz ; SHA-256 f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
- wolfSSL https://github.com/wolfSSL/wolfssl/archive/refs/tags/v5.9.4-stable.tar.gz ; SHA-256 7256bfc89b183a75183806c7debfa203443873b0b4a562e1b80d68e01b45ac57

These archive hashes are provenance references, not hashes of the modified vendored directories.
