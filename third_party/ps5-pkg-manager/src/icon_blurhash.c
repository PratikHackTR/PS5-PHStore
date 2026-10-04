/*
 * PKG Manager - Icon BlurHash Generator
 *
 * Computes compact BlurHash placeholder strings from package icon0.png.
 */

#include "icon_blurhash.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* PNG-only, memory-API-only stb_image build. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#ifndef STBI_MAX_DIMENSIONS
#define STBI_MAX_DIMENSIONS 2048
#endif
#define STB_IMAGE_IMPLEMENTATION
/* PNG-only build leaves some stb helpers unused; silence locally. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* Dark backdrop for alpha blending (matches UI cards #141520). */
#define BLEND_R 20
#define BLEND_G 21
#define BLEND_B 32

static const char k_base83_chars[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~";

#define BH_PI 3.14159265358979323846f

static float s_srgb_lut[256];
static int s_lut_ready = 0;

static void init_srgb_lut(void) {
    if (__builtin_expect(s_lut_ready, 1)) return;
    for (int i = 0; i < 256; i++) {
        float v = (float)i / 255.0f;
        s_srgb_lut[i] = (v <= 0.04045f) ? (v / 12.92f) : powf((v + 0.055f) / 1.055f, 2.4f);
    }
    s_lut_ready = 1;
}

static int linear_to_srgb(float value) {
    float v = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    float srgb;
    if (v <= 0.0031308f) srgb = v * 12.92f;
    else srgb = 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    return (int)(srgb * 255.0f + 0.5f);
}

static float sign_pow(float value, float exp) {
    float s = value < 0.0f ? -1.0f : 1.0f;
    float a = value < 0.0f ? -value : value;
    return s * powf(a, exp);
}

static void encode_base83(char *out, size_t *pos, unsigned int value, int length) {
    char tmp[8];
    int i;
    for (i = 0; i < length; i++) {
        tmp[i] = k_base83_chars[value % 83];
        value /= 83;
    }
    for (i = length - 1; i >= 0; i--) {
        out[(*pos)++] = tmp[i];
    }
}

int blurhash_encode_rgb(const uint8_t *rgb, int width, int height,
                        int x_comp, int y_comp,
                        char *out, size_t out_max) {
    int x, y, i, j, n;
    float *factors;
    float *cos_x = NULL;
    float *cos_y = NULL;
    float max_ac = 0.0f;
    size_t pos = 0;
    size_t need;

    if (!rgb || !out || width <= 0 || height <= 0 ||
        x_comp < 1 || x_comp > 9 || y_comp < 1 || y_comp > 9) {
        return -1;
    }

    /* 1 size char + 1 max-AC char + 4 DC chars + 2 chars per AC + NUL */
    need = (size_t)(4 + 2 * x_comp * y_comp) + 1;
    if (out_max < need) return -1;

    init_srgb_lut();

    factors = (float *)malloc((size_t)x_comp * (size_t)y_comp * 3 * sizeof(float));
    if (!factors) return -1;

    cos_x = (float *)malloc((size_t)x_comp * (size_t)width * sizeof(float));
    cos_y = (float *)malloc((size_t)y_comp * (size_t)height * sizeof(float));
    if (!cos_x || !cos_y) {
        free(factors);
        if (cos_x) free(cos_x);
        if (cos_y) free(cos_y);
        return -1;
    }

    for (i = 0; i < x_comp; i++) {
        for (x = 0; x < width; x++) {
            cos_x[i * width + x] = cosf(BH_PI * (float)i * (float)x / (float)width);
        }
    }
    for (j = 0; j < y_comp; j++) {
        for (y = 0; y < height; y++) {
            cos_y[j * height + y] = cosf(BH_PI * (float)j * (float)y / (float)height);
        }
    }

    for (j = 0; j < y_comp; j++) {
        const float *cy = &cos_y[j * height];
        for (i = 0; i < x_comp; i++) {
            const float *cx = &cos_x[i * width];
            float r = 0.0f, g = 0.0f, b = 0.0f;
            float norm = (i == 0 && j == 0) ? 1.0f : 2.0f;
            for (y = 0; y < height; y++) {
                float basis_y = cy[y];
                const uint8_t *row = &rgb[y * width * 3];
                for (x = 0; x < width; x++) {
                    float basis = cx[x] * basis_y;
                    r += basis * s_srgb_lut[row[x * 3]];
                    g += basis * s_srgb_lut[row[x * 3 + 1]];
                    b += basis * s_srgb_lut[row[x * 3 + 2]];
                }
            }
            norm *= (float)(width * height);
            n = (j * x_comp + i) * 3;
            factors[n]     = r / norm;
            factors[n + 1] = g / norm;
            factors[n + 2] = b / norm;
        }
    }

    free(cos_x);
    free(cos_y);

    /* DC is factors[0..2]; find max AC magnitude for quantization. */
    for (n = 3; n < x_comp * y_comp * 3; n++) {
        float a = factors[n] < 0 ? -factors[n] : factors[n];
        if (a > max_ac) max_ac = a;
    }

    {
        int quant_max = (int)(max_ac * 166.0f - 0.5f);
        if (quant_max < 0) quant_max = 0;
        if (quant_max > 82) quant_max = 82;

        encode_base83(out, &pos, (unsigned int)(x_comp - 1 + (y_comp - 1) * 9), 1);
        encode_base83(out, &pos, (unsigned int)quant_max, 1);

        /* DC (direct linear -> sRGB, no normalization) */
        {
            int dc_r = linear_to_srgb(factors[0]);
            int dc_g = linear_to_srgb(factors[1]);
            int dc_b = linear_to_srgb(factors[2]);
            encode_base83(out, &pos, (unsigned int)((dc_r << 16) + (dc_g << 8) + dc_b), 4);
        }

        /* AC components, sqrt-symmetric quantization using spec-aligned actual_max */
        if (quant_max > 0) {
            float actual_max = ((float)quant_max + 1) / 166.0f;
            for (n = 3; n < x_comp * y_comp * 3; n += 3) {
                int q[3];
                int k;
                for (k = 0; k < 3; k++) {
                    int qv = (int)(sign_pow(factors[n + k] / actual_max, 0.5f) * 9.0f + 9.5f);
                    if (qv < 0) qv = 0;
                    if (qv > 18) qv = 18;
                    q[k] = qv;
                }
                encode_base83(out, &pos, (unsigned int)(q[0] * 19 * 19 + q[1] * 19 + q[2]), 2);
            }
        } else {
            for (n = 3; n < x_comp * y_comp * 3; n += 3) {
                encode_base83(out, &pos, (unsigned int)(9 * 19 * 19 + 9 * 19 + 9), 2);
            }
        }
    }

    out[pos] = '\0';
    free(factors);
    return 0;
}

int icon_compute_blurhash(const uint8_t *png_data, size_t png_len,
                          char *out_hash, size_t out_max) {
    int w = 0, h = 0;
    uint8_t *rgba = NULL;
    uint8_t *rgb = NULL;
    int rc = -1;
    int x, y;

    if (!png_data || png_len == 0 || !out_hash || out_max < BLURHASH_MAX_LEN) {
        return -1;
    }
    out_hash[0] = '\0';

    rgba = stbi_load_from_memory(png_data, (int)png_len, &w, &h, NULL, 4);
    if (!rgba || w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        if (rgba) stbi_image_free(rgba);
        return -1;
    }

    /* Box-downscale to at most 128px on the long edge: placeholder DCT needs
       averages, not full resolution, and this cuts encode cost ~16x for
       typical 512px icons while keeping quality. */
    int biggest = (w > h) ? w : h;
    int step = (biggest + 127) / 128;
    if (step < 1) step = 1;
    int sw = (w + step - 1) / step;
    int sh = (h + step - 1) / step;

    rgb = (uint8_t *)malloc((size_t)sw * (size_t)sh * 3);
    if (!rgb) {
        stbi_image_free(rgba);
        return -1;
    }

    /* Downscale and composite alpha directly onto dark UI backdrop.
       Compositing each source pixel onto the backdrop before/during averaging
       prevents edge attenuation and color bleed on anti-aliased transparency,
       while eliminating the intermediate RGBA buffer allocation. */
    for (y = 0; y < sh; y++) {
        for (x = 0; x < sw; x++) {
            unsigned r_acc = 0, g_acc = 0, b_acc = 0, n = 0;
            int ys, xs;
            for (ys = y * step; ys < (y + 1) * step && ys < h; ys++) {
                const uint8_t *src_row = &rgba[(size_t)ys * (size_t)w * 4];
                for (xs = x * step; xs < (x + 1) * step && xs < w; xs++) {
                    const uint8_t *p = &src_row[xs * 4];
                    int a = p[3];
                    int inv_a = 255 - a;
                    r_acc += (unsigned)((p[0] * a + BLEND_R * inv_a + 127) / 255);
                    g_acc += (unsigned)((p[1] * a + BLEND_G * inv_a + 127) / 255);
                    b_acc += (unsigned)((p[2] * a + BLEND_B * inv_a + 127) / 255);
                    n++;
                }
            }
            size_t di = ((size_t)y * (size_t)sw + (size_t)x) * 3;
            rgb[di]     = (uint8_t)((r_acc + n / 2) / n);
            rgb[di + 1] = (uint8_t)((g_acc + n / 2) / n);
            rgb[di + 2] = (uint8_t)((b_acc + n / 2) / n);
        }
    }
    stbi_image_free(rgba);

    rc = blurhash_encode_rgb(rgb, sw, sh, BLURHASH_X_COMP, BLURHASH_Y_COMP,
                             out_hash, out_max);
    free(rgb);
    return rc;
}
