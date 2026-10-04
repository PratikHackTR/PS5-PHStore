#ifndef PH_CRYPTO_CONTROL_H
#define PH_CRYPTO_CONTROL_H
#include <stdint.h>
extern int ph_crypto_enabled;
void ph_crypto_cpu_init(void);
int ph_crypto_hardware_ready(void);
unsigned ph_crypto_cpu_ecx(void);
void ph_crypto_note_gcm(int hardware,unsigned size);
void ph_crypto_reset_stats(void);
void ph_crypto_stats(uint64_t *sw_calls,uint64_t *sw_bytes,uint64_t *hw_calls,uint64_t *hw_bytes);
int ph_crypto_selftest(void);
int ph_crypto_benchmark(double *seconds,double *mib_s);
#endif
