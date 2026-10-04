#ifndef ICON_BLURHASH_H
#define ICON_BLURHASH_H

#include <stddef.h>
#include <stdint.h>

/* BlurHash component counts (6x4; ~52-char hash for visibly sharper
   placeholders than the 4x3 upstream default). */
#define BLURHASH_X_COMP 6
#define BLURHASH_Y_COMP 4

/* Max hash string length incl. NUL for 6x4 (1 size + 1 maxAC + 4 DC + 46 AC + NUL). */
#define BLURHASH_MAX_LEN 64

/* Hashes shorter than this are from an older, lower-resolution generation and
   are treated as upgradeable (re-hashed on next scan/serve). */
#define BLURHASH_UPGRADE_MIN_LEN 40

#ifdef __cplusplus
extern "C" {
#endif

/* Compute a BlurHash string for PNG image bytes (e.g. icon0.png).
 * Transparent pixels are blended onto a dark backdrop to match the UI cards.
 * Returns 0 on success, -1 on failure (bad input, decode error, tiny buffer).
 */
int icon_compute_blurhash(const uint8_t *png_data, size_t png_len,
                           char *out_hash, size_t out_max);

/* Encode raw 8-bit RGB pixels (row-major, no padding) as BlurHash.
 * x_comp/y_comp in [1,9]. out must hold 4 + 2*x_comp*y_comp chars + NUL.
 * Returns 0 on success, -1 on failure.
 */
int blurhash_encode_rgb(const uint8_t *rgb, int width, int height,
                        int x_comp, int y_comp,
                        char *out, size_t out_max);

#ifdef __cplusplus
}
#endif

#endif /* ICON_BLURHASH_H */
