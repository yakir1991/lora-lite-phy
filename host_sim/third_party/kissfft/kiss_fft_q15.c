/* kiss_fft_q15.c — KissFFT compiled in Q15 (FIXED_POINT=16) mode.
 * This is a thin wrapper that includes the upstream kiss_fft.c with
 * FIXED_POINT=16, producing int16_t-typed FFT routines under a
 * distinct symbol namespace (kiss_fft_q15_*).  The float build in
 * kiss_fft.c is left untouched. */

/* Rename public symbols so they don't clash with the float build. */
#define kiss_fft_alloc   kiss_fft_q15_alloc
#define kiss_fft         kiss_fft_q15
#define kiss_fft_stride  kiss_fft_q15_stride
#define kiss_fft_cleanup kiss_fft_q15_cleanup
#define kiss_fft_next_fast_size kiss_fft_q15_next_fast_size

/* Tell KissFFT to use int16_t scalars. */
#define FIXED_POINT 16

/* Pull in the actual implementation. */
#include "kiss_fft.c"
