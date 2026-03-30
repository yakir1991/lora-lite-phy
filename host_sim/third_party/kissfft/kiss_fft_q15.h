/* kiss_fft_q15.h — C header for the Q15 (FIXED_POINT=16) KissFFT build.
 * Exposes the same kiss_fft API but with int16_t scalars and _q15 names. */

#ifndef KISS_FFT_Q15_H
#define KISS_FFT_Q15_H

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t r;
    int16_t i;
} kiss_fft_q15_cpx;

typedef struct kiss_fft_state* kiss_fft_q15_cfg;

kiss_fft_q15_cfg kiss_fft_q15_alloc(int nfft, int inverse_fft,
                                     void* mem, size_t* lenmem);
void kiss_fft_q15(kiss_fft_q15_cfg cfg,
                  const kiss_fft_q15_cpx* fin,
                  kiss_fft_q15_cpx* fout);
void kiss_fft_q15_cleanup(void);

#define kiss_fft_q15_free free

#ifdef __cplusplus
}
#endif

#endif /* KISS_FFT_Q15_H */
