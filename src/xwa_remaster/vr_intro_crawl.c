#include "xwa_remaster/vr_intro_crawl.h"
#include "xwa_remaster/vr_intro_hoth.h"

#include "aeron/aeron.h"
#include "aeron/asset/opt_model.h"
#include "aeron/log.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/present.h"
#include "aeron/xr.h"
#include "xwa_runtime/runtime/movie_task.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ---- the film, measured ------------------------------------------------
 *
 * MOVIES/INTRO_FINAL.SNM: 640x400 at 15 fps. Film pixels: x right, y down
 * from the top left corner, the picture's centre at (320, 200).
 *
 * Black until frame 52 (3.467 s); there the stars and the logo cut in, the
 * logo filling the picture, and it recedes, slowing, about its centre. Its
 * outline's height in film pixels follows 1 / (cubic in the time since the
 * cut), within 0.4 px over frames 52..151; its width is 2.26 times that.
 * It fades out (linearly in the picture's values) from 9.87 to 10.75 s
 * (the VR scene keeps its own, shorter timetable: CRAWL_LOGO_END). */
#define CRAWL_FILM_CX 320.0f
#define CRAWL_FILM_CY 200.0f
#define CRAWL_FILM_W 640.0f
#define CRAWL_START 3.4667f
static const double k_logo_inv_h[4] = { 0.002576953439566483, 0.003376103021604383, -0.00019466942423813978,
										1.0588799955058198e-05 };
#define CRAWL_LOGO_ASPECT 2.26f
/* The VR scene's own timetable, in seconds from the logo's cut (the film's
 * logo stays to 10.75 s and its text slides in from below the picture, out
 * of the screen's frame but in view round it): the logo recedes for
 * LOGO_END seconds, fading out over the last LOGO_FADE; nothing of the text
 * shows before TEXT_FROM, where it fades in over TEXT_FADE_IN while it
 * already crawls (at the film's speed). The text does not start where the
 * film has it then (mid-view): its scroll runs late by however much puts
 * the first line's top TEXT_ENTRY_DEG below the line of sight to the logo
 * (about where the headset's view ends below) at TEXT_ENTRY_AT, so it
 * fades in on the edge of the view and crawls up into it. Tunable with
 * XWA_XR_CRAWL_LOGO_END / _LOGO_FADE / _TEXT_FROM / _TEXT_FADE_IN /
 * _TEXT_ENTRY_DEG / _TEXT_ENTRY_AT. */
#define CRAWL_LOGO_END 7.5f
#define CRAWL_LOGO_FADE 1.0f
#define CRAWL_TEXT_FROM 7.5f
#define CRAWL_TEXT_FADE_IN 1.5f
#define CRAWL_TEXT_ENTRY_DEG 35.0f
#define CRAWL_TEXT_ENTRY_AT 9.0f

/* The crawl: text on a plane seen in perspective, its vanishing line
 * CRAWL_YV (film y), scrolling at a constant speed. A point of the text is
 * (xn, s): s its place along the scroll in seconds (the first line's top at
 * 0, line i's baseline at i * PITCH + BASE), xn across, so that at film
 * time t it is at film y = YV + K / (t + T0 - s) and x = CX + xn * (y - YV)
 * (fitted to every line's track through the film, 0.3 px rms, then with
 * the rest to whole frames rendered offline).
 *
 * The lines (bold italic with Arial's metrics, leaning less than its
 * italic): an em is EM_Y seconds tall and
 * EM_X across; every line but a paragraph's last is scaled across to fill
 * XL..XR exactly (the film's justification: long lines squeezed, short
 * ones widened), the last lines start at XL at their natural width. The
 * text's colour is 255,247,132; it fades out (linearly in the picture's
 * values) from 46.67 to 48.8 s, the stars staying. They stand still from
 * the logo's cut until the planet rises; vr/intro_stars.rgba holds them. */
#define CRAWL_YV -154.643f
#define CRAWL_K 13535.74f
#define CRAWL_T0 14.1932f
#define CRAWL_PITCH 1.36445f
#define CRAWL_BASE 0.9502f
#define CRAWL_EM_Y 1.3343f
#define CRAWL_EM_X 0.0748f
#define CRAWL_XL (-0.6090f)
#define CRAWL_XR 0.6000f
/* The film's letters lean less than the font's italic: sheared back by
 * this (x per unit of height, in ems). */
#define CRAWL_SHEAR (-0.174f)
#define CRAWL_FADE_FROM 46.67f
#define CRAWL_FADE_TO 48.8f
static const uint8_t k_text_srgb[3] = { 255, 247, 132 };
/* The logo's (as vr/starwars_logo.rgba carries it). */
static const uint8_t k_logo_srgb[3] = { 255, 223, 107 };

/* Forward: the colours through the tonemap (below). */
static void crawl_tonemap(const float in[3], float out[3]);
static void crawl_untonemap(const float srgb[3], float out[3]);

/* After the crawl the film shows only its stars until the planet rises
 * (~51.6 s), the very stars drawn here straight ahead: the screen comes
 * back at HANDOVER_FROM at once (the compositor's screen is opaque: a fade
 * would first black the stars out), and the scene round it eases to the
 * menus' look by HANDOVER_TO (the eye brightness, the menus' own stars). */
#define CRAWL_HANDOVER_FROM 49.0f
#define CRAWL_HANDOVER_TO 50.5f

/* The text texture: xn from TEX_X0 to -TEX_X0 across its width; s at a
 * scale that keeps a texel about as long as it is wide where the text is
 * (TEX_ANISO film pixels from the vanishing line: mid-picture), so plain
 * trilinear filtering stays sharp. */
#define CRAWL_TEX_X0 (-0.66f)
#define CRAWL_TEX_S0 (-0.45f)
#define CRAWL_TEX_ANISO 330.0f

typedef struct CrawlTex {
	AeronTexture* tex;
	int w, h;
} CrawlTex;

static struct {
	int started; /* the intro is playing and its assets were tried */
	int ok;      /* ...and loaded */
	int active;  /* the scene plays (until the handover is over) */
	float t;     /* the film's clock, seconds */
	CrawlTex logo, text, stars;
	float logo_box[4]; /* the logo's outline in the image, uv: u0 v0 u1 v1 */
	float text_s1;     /* s at the text texture's bottom edge */
	float logo_tint[3], text_tint[3], star_tint[3]; /* HDR colours at load (see the LUTs) */
	int release_in;    /* frames until the textures are freed */
	int hoth_loaded;   /* the Hoth shot's loading is done (vr_intro_hoth.h) */
} s_crawl;

static float crawl_env(const char* name, float fallback) {
	const char* v = SDL_getenv(name);
	return v && v[0] ? (float)atof(v) : fallback;
}

static int crawl_enabled(void) { return crawl_env("XWA_XR_INTRO_CRAWL", 1.0f) != 0.0f; }

static float crawl_clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

static float crawl_smooth(float x) {
	x = crawl_clamp01(x);
	return x * x * (3.0f - 2.0f * x);
}

static float crawl_srgb_to_linear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float crawl_linear_to_srgb(float c) {
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* An APK asset (or a file beside the game): vr/<name>. */
static void* crawl_load(const char* name, size_t* size) {
	char path[2][96];
	snprintf(path[0], sizeof path[0], "vr/%s", name);
	snprintf(path[1], sizeof path[1], "assets/vr/%s", name);
	for (int i = 0; i < 2; ++i) {
		void* data = SDL_LoadFile(path[i], size);
		if (data) {
			return data;
		}
	}
	Aeron_LogInfo("xwa.remaster", "VR intro crawl: no vr/%s", name);
	return NULL;
}

/* ---- textures ----------------------------------------------------------- */

static int crawl_levels(int w, int h) {
	int n = 1;
	while (w > 1 || h > 1) {
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
		++n;
	}
	return n;
}

/* Uploads `levels` premultiplied sRGB RGBA8 images (consecutive in `px`,
 * each half the last) as one mipmapped texture. */
static int crawl_upload(AeronCommandBuffer* cmd, CrawlTex* out, const uint8_t* px, int w, int h, int levels,
						const char* label) {
	AeronTexture* t = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = w,
		.height = h,
		.mip_count = levels,
		.format = AERON_TEXTURE_FORMAT_RGBA8_SRGB,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = label,
	});
	if (!t) {
		return 0;
	}
	AeronTextureUploadDesc up[16];
	size_t offset = 0;
	int lw = w, lh = h;
	for (int l = 0; l < levels && l < 16; ++l) {
		const size_t size = (size_t)lw * (size_t)lh * 4u;
		memset(&up[l], 0, sizeof up[l]);
		up[l].texture = t;
		up[l].mip_level = l;
		up[l].width = lw;
		up[l].height = lh;
		up[l].raw_data = px + offset;
		up[l].raw_size = (uint32_t)size;
		offset += size;
		lw = lw > 1 ? lw / 2 : 1;
		lh = lh > 1 ? lh / 2 : 1;
	}
	if (!Aeron_UploadTextureBatchCmd(cmd, up, levels < 16 ? levels : 16)) {
		Aeron_DestroyTexture(t);
		return 0;
	}
	out->tex = t;
	out->w = w;
	out->h = h;
	return 1;
}

static size_t crawl_chain_size(int w, int h, int levels) {
	size_t n = 0;
	for (int l = 0; l < levels; ++l) {
		n += (size_t)w * (size_t)h * 4u;
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
	}
	return n;
}

/* Halves a float image (box filter; an odd edge row/column folds in). */
static void crawl_halve(const float* src, int w, int h, int ch, float* dst) {
	const int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
	for (int y = 0; y < dh; ++y) {
		const int y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : y0;
		for (int x = 0; x < dw; ++x) {
			const int x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : x0;
			for (int c = 0; c < ch; ++c) {
				dst[((size_t)y * dw + x) * ch + c] =
					0.25f * (src[((size_t)y0 * w + x0) * ch + c] + src[((size_t)y0 * w + x1) * ch + c] +
							 src[((size_t)y1 * w + x0) * ch + c] + src[((size_t)y1 * w + x1) * ch + c]);
			}
		}
	}
}

/* The texel colour for each coverage: the HDR level whose tonemapped image
 * of colour `srgb8` is `coverage` times that colour in display values, so
 * anti-aliased edges (and mip levels) blend as in the film, not brightened
 * by the tonemap's curve. lut[i] for coverage i / 1023; `tint` gets the HDR
 * colour at full coverage. */
static void crawl_coverage_lut(const uint8_t srgb8[3], float lut[1024], float tint[3]) {
	const float d[3] = { (float)srgb8[0] / 255.0f, (float)srgb8[1] / 255.0f, (float)srgb8[2] / 255.0f };
	crawl_untonemap(d, tint);
	float full[3];
	crawl_tonemap(tint, full);
	const float want = full[0] + full[1] + full[2];
	lut[0] = 0.0f;
	for (int i = 1; i < 1024; ++i) {
		const float target = want * (float)i / 1023.0f;
		float lo = 0.0f, hi = 1.0f;
		for (int it = 0; it < 22; ++it) {
			const float mid = 0.5f * (lo + hi);
			const float x[3] = { tint[0] * mid, tint[1] * mid, tint[2] * mid };
			float o[3];
			crawl_tonemap(x, o);
			if (o[0] + o[1] + o[2] < target) {
				lo = mid;
			} else {
				hi = mid;
			}
		}
		lut[i] = 0.5f * (lo + hi);
	}
}

/* Coverage (0..1) -> premultiplied mip chain: alpha the coverage, colour
 * lut(coverage) in white (the tint colours it). Takes `cov` (malloc'd)
 * and frees it. */
static int crawl_texture_coverage(AeronCommandBuffer* cmd, CrawlTex* out, float* cov, int w, int h,
								  const float lut[1024], const char* label) {
	const int levels = crawl_levels(w, h);
	uint8_t* chain = (uint8_t*)malloc(crawl_chain_size(w, h, levels));
	float* a = cov;
	float* b = (float*)malloc((size_t)(w / 2 + 1) * (h / 2 + 1) * sizeof(float));
	int ok = 0;
	if (chain && a && b) {
		uint8_t enc[1024];
		for (int i = 0; i < 1024; ++i) {
			enc[i] = (uint8_t)(crawl_linear_to_srgb(crawl_clamp01(lut[i])) * 255.0f + 0.5f);
		}
		size_t offset = 0;
		int lw = w, lh = h;
		for (int l = 0; l < levels; ++l) {
			for (size_t i = 0; i < (size_t)lw * lh; ++i) {
				const float c = crawl_clamp01(a[i]);
				uint8_t* p = chain + offset + i * 4;
				p[0] = p[1] = p[2] = enc[(int)(c * 1023.0f + 0.5f)];
				p[3] = (uint8_t)(c * 255.0f + 0.5f);
			}
			offset += (size_t)lw * lh * 4u;
			if (l + 1 < levels) {
				crawl_halve(a, lw, lh, 1, b);
				float* tmp = a;
				a = b;
				b = tmp;
				lw = lw > 1 ? lw / 2 : 1;
				lh = lh > 1 ? lh / 2 : 1;
			}
		}
		ok = crawl_upload(cmd, out, chain, w, h, levels, label);
	}
	free(chain);
	free(a);
	free(b);
	return ok;
}

/* vr/starwars_logo.rgba: XWAL (straight RGBA, the yellow outline, the rest
 * clear; only its alpha is used). The outline's extent (coverage over 40 %, as measured in the
 * film) is what the film's size table describes. */
static int crawl_load_logo(AeronCommandBuffer* cmd) {
	size_t size = 0;
	uint8_t* data = (uint8_t*)crawl_load("starwars_logo.rgba", &size);
	int ok = 0;
	if (data && size >= 12 && memcmp(data, "XWAL", 4) == 0) {
		uint32_t w, h;
		memcpy(&w, data + 4, 4);
		memcpy(&h, data + 8, 4);
		if (w && h && w <= 4096 && h <= 4096 && size >= 12 + (size_t)w * h * 4u) {
			const uint8_t* px = data + 12;
			int x0 = (int)w, y0 = (int)h, x1 = -1, y1 = -1;
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					if (px[((size_t)y * w + x) * 4 + 3] >= 102) {
						x0 = (int)x < x0 ? (int)x : x0;
						x1 = (int)x > x1 ? (int)x : x1;
						y0 = (int)y < y0 ? (int)y : y0;
						y1 = (int)y > y1 ? (int)y : y1;
					}
				}
			}
			if (x1 > x0 && y1 > y0) {
				s_crawl.logo_box[0] = (float)x0 / (float)w;
				s_crawl.logo_box[1] = (float)y0 / (float)h;
				s_crawl.logo_box[2] = (float)(x1 + 1) / (float)w;
				s_crawl.logo_box[3] = (float)(y1 + 1) / (float)h;
				/* One colour: its coverage, coloured by the tint. */
				float* cov = (float*)malloc((size_t)w * h * sizeof(float));
				if (cov) {
					for (size_t i = 0; i < (size_t)w * h; ++i) {
						cov[i] = (float)px[i * 4 + 3] / 255.0f;
					}
					float lut[1024];
					crawl_coverage_lut(k_logo_srgb, lut, s_crawl.logo_tint);
					ok = crawl_texture_coverage(cmd, &s_crawl.logo, cov, (int)w, (int)h, lut,
												"xwa.vr_intro_crawl.logo");
				}
			}
		}
	}
	SDL_free(data);
	return ok;
}

/* vr/intro_stars.rgba: XWAL, the film's star field (640x400, white, alpha
 * the brightness). Optional: without it the menus' own stars show. */
static void crawl_load_stars(AeronCommandBuffer* cmd) {
	size_t size = 0;
	uint8_t* data = (uint8_t*)crawl_load("intro_stars.rgba", &size);
	if (data && size >= 12 && memcmp(data, "XWAL", 4) == 0) {
		uint32_t w, h;
		memcpy(&w, data + 4, 4);
		memcpy(&h, data + 8, 4);
		if (w && h && w <= 4096 && h <= 4096 && size >= 12 + (size_t)w * h * 4u) {
			float* cov = (float*)malloc((size_t)w * h * sizeof(float));
			if (cov) {
				for (size_t i = 0; i < (size_t)w * h; ++i) {
					cov[i] = (float)data[12 + i * 4 + 3] / 255.0f;
				}
				static const uint8_t white[3] = { 255, 255, 255 };
				float lut[1024];
				crawl_coverage_lut(white, lut, s_crawl.star_tint);
				(void)crawl_texture_coverage(cmd, &s_crawl.stars, cov, (int)w, (int)h, lut,
											 "xwa.vr_intro_crawl.stars");
			}
		}
	}
	SDL_free(data);
}

/* ---- the crawl's text ---------------------------------------------------- */

/* One UTF-8 code point (Latin-1 is all the font needs here). */
static int crawl_utf8(const char** p) {
	const unsigned char* s = (const unsigned char*)*p;
	int c = s[0], n = 0;
	if (c < 0x80) {
		n = 0;
	} else if ((c & 0xE0) == 0xC0) {
		c &= 0x1F;
		n = 1;
	} else if ((c & 0xF0) == 0xE0) {
		c &= 0x0F;
		n = 2;
	} else if ((c & 0xF8) == 0xF0) {
		c &= 0x07;
		n = 3;
	} else {
		*p += 1;
		return '?';
	}
	for (int i = 1; i <= n; ++i) {
		if ((s[i] & 0xC0) != 0x80) {
			*p += i;
			return '?';
		}
		c = (c << 6) | (s[i] & 0x3F);
	}
	*p += n + 1;
	return c;
}

#define CRAWL_MAX_LINES 48
#define CRAWL_MAX_GLYPHS 160

typedef struct CrawlLine {
	int cp[CRAWL_MAX_GLYPHS];
	float pen[CRAWL_MAX_GLYPHS]; /* glyph origins, font units */
	int count;
	float ink0, ink1; /* ink extent, font units */
	int last;         /* a paragraph's last line (not justified) */
} CrawlLine;

/* Lays the text out in font units: glyph origins with kerning, and each
 * line's ink extent. Returns the number of lines (blank ones included). */
static int crawl_layout(const stbtt_fontinfo* font, char* text, CrawlLine* lines) {
	int n = 0;
	char* p = text;
	while (*p && n < CRAWL_MAX_LINES) {
		char* end = strchr(p, '\n');
		char* next = end ? end + 1 : p + strlen(p);
		if (end) {
			*end = '\0';
		}
		CrawlLine* ln = &lines[n++];
		memset(ln, 0, sizeof *ln);
		float pen = 0.0f;
		int prev = 0;
		ln->ink0 = 1e9f;
		ln->ink1 = -1e9f;
		for (const char* q = p; *q && ln->count < CRAWL_MAX_GLYPHS;) {
			const int c = crawl_utf8(&q);
			if (c == '\r') {
				continue;
			}
			if (prev) {
				pen += (float)stbtt_GetCodepointKernAdvance(font, prev, c);
			}
			int adv, lsb, x0, y0, x1, y1;
			stbtt_GetCodepointHMetrics(font, c, &adv, &lsb);
			if (stbtt_GetCodepointBox(font, c, &x0, &y0, &x1, &y1)) {
				/* The box sheared like the glyph (y up). */
				ln->ink0 = fminf(ln->ink0, pen + (float)x0 + CRAWL_SHEAR * (float)(CRAWL_SHEAR < 0.0f ? y1 : y0));
				ln->ink1 = fmaxf(ln->ink1, pen + (float)x1 + CRAWL_SHEAR * (float)(CRAWL_SHEAR < 0.0f ? y0 : y1));
			}
			ln->cp[ln->count] = c;
			ln->pen[ln->count] = pen;
			ln->count++;
			pen += (float)adv;
			prev = c;
		}
		if (ln->ink1 < ln->ink0) {
			ln->count = 0; /* blank: a paragraph break */
		}
		p = next;
	}
	while (n > 0 && lines[n - 1].count == 0) {
		--n;
	}
	for (int i = 0; i < n; ++i) {
		lines[i].last = i + 1 >= n || lines[i + 1].count == 0;
	}
	return n;
}

/* Rasterizes the crawl (vr/intro_crawl.txt in vr/LiberationSans-
 * BoldItalic.ttf) into the text texture, in (xn, s) texture space. */
static int crawl_load_text(AeronCommandBuffer* cmd) {
	size_t font_size = 0, text_size = 0;
	unsigned char* ttf = (unsigned char*)crawl_load("LiberationSans-BoldItalic.ttf", &font_size);
	char* raw = (char*)crawl_load("intro_crawl.txt", &text_size);
	CrawlLine* lines = (CrawlLine*)calloc(CRAWL_MAX_LINES, sizeof(CrawlLine));
	float* cov = NULL;
	unsigned char* glyph = NULL;
	int ok = 0;
	stbtt_fontinfo font;
	if (!ttf || !raw || !lines || !stbtt_InitFont(&font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
		goto done;
	}
	char* text = (char*)malloc(text_size + 1);
	if (!text) {
		goto done;
	}
	memcpy(text, raw, text_size);
	text[text_size] = '\0';
	const int n = crawl_layout(&font, text, lines);
	free(text);
	if (n <= 0) {
		goto done;
	}
	/* Texture scale: TW texels across xn TEX_X0..-TEX_X0; along s, texels
	 * as long as wide TEX_ANISO px below the vanishing line. */
	int tw = (int)crawl_env("XWA_XR_CRAWL_TEXTURE", 4096.0f);
	tw = tw < 1024 ? 1024 : tw > 4096 ? 4096 : tw;
	const float ta = (float)tw / (-2.0f * CRAWL_TEX_X0); /* texels per xn */
	const float tb = ta * CRAWL_TEX_ANISO / CRAWL_K;     /* texels per second of scroll */
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
	const float em = stbtt_ScaleForMappingEmToPixels(&font, 1.0f); /* em per font unit */
	const float s1 = (float)(n - 1) * CRAWL_PITCH + CRAWL_BASE - (float)desc * em * CRAWL_EM_Y + 0.2f;
	int th = (int)ceilf((s1 - CRAWL_TEX_S0) * tb);
	th = th > 4096 ? 4096 : th;
	cov = (float*)calloc((size_t)tw * th, sizeof(float));
	glyph = (unsigned char*)malloc(512 * 512);
	if (!cov || !glyph) {
		goto done;
	}
	const float sy = tb * CRAWL_EM_Y * em; /* texels per font unit, down */
	for (int i = 0; i < n; ++i) {
		const CrawlLine* ln = &lines[i];
		if (!ln->count) {
			continue;
		}
		const float sx = ln->last ? ta * CRAWL_EM_X * em : ta * (CRAWL_XR - CRAWL_XL) / (ln->ink1 - ln->ink0);
		const float ox = (CRAWL_XL - CRAWL_TEX_X0) * ta - ln->ink0 * sx; /* texel x of the pen's origin */
		const float base = ((float)i * CRAWL_PITCH + CRAWL_BASE - CRAWL_TEX_S0) * tb;
		const int by = (int)floorf(base);
		const float fy = base - (float)by;
		for (int g = 0; g < ln->count; ++g) {
			const float gxf = ox + ln->pen[g] * sx;
			const int gx = (int)floorf(gxf);
			const float fx = gxf - (float)gx;
			int x0, y0, x1, y1;
			stbtt_GetCodepointBitmapBoxSubpixel(&font, ln->cp[g], sx, sy, fx, fy, &x0, &y0, &x1, &y1);
			const int gw = x1 - x0, gh = y1 - y0;
			if (gw <= 0 || gh <= 0 || gw > 512 || gh > 512) {
				continue;
			}
			stbtt_MakeCodepointBitmapSubpixel(&font, glyph, gw, gh, gw, sx, sy, fx, fy, ln->cp[g]);
			for (int y = 0; y < gh; ++y) {
				const int ty = by + y0 + y;
				if (ty < 0 || ty >= th) {
					continue;
				}
				/* The shear: this row's height above the baseline (font
				 * units) moves it across. */
				const float above = (base - ((float)ty + 0.5f)) / sy;
				const float shift = CRAWL_SHEAR * above * sx;
				const int si = (int)floorf(shift);
				const float sf = shift - (float)si;
				for (int x = -1; x < gw; ++x) {
					const int tx = gx + x0 + x + si + 1;
					if (tx < 0 || tx >= tw) {
						continue;
					}
					/* texel tx takes glyph x (weight 1 - sf) and x + 1 (sf)... */
					const float g0 = x >= 0 ? (float)glyph[y * gw + x] : 0.0f;
					const float g1 = x + 1 < gw ? (float)glyph[y * gw + x + 1] : 0.0f;
					const float v = (g0 * sf + g1 * (1.0f - sf)) / 255.0f;
					float* d = &cov[(size_t)ty * tw + tx];
					*d = *d > v ? *d : v;
				}
			}
		}
	}
	s_crawl.text_s1 = CRAWL_TEX_S0 + (float)th / tb;
	float lut[1024];
	crawl_coverage_lut(k_text_srgb, lut, s_crawl.text_tint);
	ok = crawl_texture_coverage(cmd, &s_crawl.text, cov, tw, th, lut, "xwa.vr_intro_crawl.text");
	cov = NULL; /* freed with the mip chain */
	if (ok) {
		Aeron_LogInfo("xwa.remaster", "VR intro crawl: %d lines, text %dx%d", n, tw, th);
	}
done:
	free(cov);
	free(glyph);
	free(lines);
	SDL_free(ttf);
	SDL_free(raw);
	return ok;
}

static void crawl_release(void) {
	if (s_crawl.logo.tex) {
		Aeron_DestroyTexture(s_crawl.logo.tex);
	}
	if (s_crawl.text.tex) {
		Aeron_DestroyTexture(s_crawl.text.tex);
	}
	if (s_crawl.stars.tex) {
		Aeron_DestroyTexture(s_crawl.stars.tex);
	}
	memset(&s_crawl.logo, 0, sizeof s_crawl.logo);
	memset(&s_crawl.text, 0, sizeof s_crawl.text);
	memset(&s_crawl.stars, 0, sizeof s_crawl.stars);
	XwaVrIntroHoth_Release();
	s_crawl.hoth_loaded = 0;
}

/* The Hoth shot follows the crawl (vr_intro_hoth.h) when its models are
 * there; XWA_XR_INTRO_HOTH=0 ends the scene after the crawl as before. */
static int crawl_hoth(void) {
	return s_crawl.ok && XwaVrIntroHoth_Ready() && crawl_env("XWA_XR_INTRO_HOTH", 1.0f) != 0.0f;
}

/* ---- per frame ------------------------------------------------------------ */

void XwaVrIntroCrawl_Update(AeronCommandBuffer* cmd) {
	const int intro = XwaMovieTask_IsActive() && SDL_strcasecmp(XwaMovieTask_Name(), "intro_final") == 0;
	if (!intro || !crawl_enabled()) {
		if (s_crawl.started) {
			s_crawl.started = 0;
			s_crawl.active = 0;
			s_crawl.release_in = 3; /* the last frames may still sample them */
		}
		if (s_crawl.release_in > 0 && --s_crawl.release_in == 0) {
			crawl_release();
		}
		return;
	}
	if (!s_crawl.started) {
		s_crawl.started = 1;
		s_crawl.release_in = 0;
		crawl_release();
		const uint64_t t0 = SDL_GetTicksNS();
		s_crawl.ok = cmd && crawl_load_logo(cmd) && crawl_load_text(cmd);
		if (s_crawl.ok) {
			crawl_load_stars(cmd);
		}
		if (!s_crawl.ok) {
			crawl_release();
			Aeron_LogInfo("xwa.remaster", "VR intro crawl: assets missing, the film plays on the screen");
		} else {
			Aeron_LogInfo("xwa.remaster", "VR intro crawl: ready in %.0f ms",
						  (double)(SDL_GetTicksNS() - t0) * 1e-6);
		}
		s_crawl.active = s_crawl.ok;
	}
	if (!s_crawl.ok) {
		return;
	}
	/* The Hoth shot's planet, sprites and models, one piece a frame in the
	 * film's black opening. */
	if (s_crawl.active && !s_crawl.hoth_loaded) {
		s_crawl.hoth_loaded = XwaVrIntroHoth_Load(cmd);
	}
	const float hold = crawl_env("XWA_XR_CRAWL_TIME", -1.0f);
	s_crawl.t = hold >= 0.0f ? hold : (float)((double)XwaMovieTask_PositionUs() * 1e-6);
	/* With the Hoth shot the scene stays (held at half brightness round the
	 * screen) until the film ends. */
	if (s_crawl.active && s_crawl.hoth_loaded && !crawl_hoth() && s_crawl.t >= CRAWL_HANDOVER_TO) {
		/* The film has the screen again: free the textures. */
		s_crawl.active = 0;
		s_crawl.release_in = 3;
	}
	if (!s_crawl.active && s_crawl.release_in > 0 && --s_crawl.release_in == 0) {
		crawl_release();
	}
}

int XwaVrIntroCrawl_Active(void) { return s_crawl.active; }

/* With the Hoth shot the screen comes back, solid and at once, on the
 * film's cut to the X-wing cockpit. */
int XwaVrIntroCrawl_ScreenHidden(void) {
	if (!s_crawl.active) {
		return 0;
	}
	if (!crawl_hoth()) {
		return s_crawl.t < CRAWL_HANDOVER_FROM;
	}
	/* Put away again for the film's logo card, back with its cut into the
	 * hyperspace tunnel. */
	return s_crawl.t < XWA_VR_HOTH_CUT || (s_crawl.t >= XWA_VR_HOTH_LOGO_IN && s_crawl.t < XWA_VR_HOTH_HYPER);
}

float XwaVrIntroCrawl_ScreenFade(void) { return XwaVrIntroCrawl_ScreenHidden() ? 0.0f : 1.0f; }

float XwaVrIntroCrawl_Level(void) {
	if (!s_crawl.active) {
		return 0.0f;
	}
	if (crawl_hoth()) {
		return 1.0f;
	}
	return 1.0f - crawl_smooth((s_crawl.t - CRAWL_HANDOVER_FROM) / (CRAWL_HANDOVER_TO - CRAWL_HANDOVER_FROM));
}

float XwaVrIntroCrawl_Brightness(void) {
	if (!s_crawl.active || !crawl_hoth()) {
		return 1.0f;
	}
	const float t = s_crawl.t;
	if (t >= XWA_VR_HOTH_LOGO_IN && t < XWA_VR_HOTH_HYPER) {
		/* The logo card: the screen away, the scene full again. */
		return 0.5f + 0.5f * crawl_smooth((t - XWA_VR_HOTH_LOGO_IN) / 0.33f);
	}
	return 1.0f - 0.5f * crawl_smooth((t - XWA_VR_HOTH_CUT) / XWA_VR_HOTH_DIM_SECONDS);
}

int XwaVrIntroCrawl_Hyperspace(float* tunnel_seconds, float* level) {
	const float end = XWA_VR_HOTH_HYPER + XWA_VR_HOTH_TUNNEL_SECONDS;
	if (!s_crawl.active || !crawl_hoth() || s_crawl.t < XWA_VR_HOTH_HYPER || s_crawl.t >= end) {
		return 0;
	}
	if (tunnel_seconds) {
		*tunnel_seconds = s_crawl.t - XWA_VR_HOTH_HYPER;
	}
	if (level) {
		/* A short spell only, then quickly to black (StarLevel: the stars
		 * with the film's drop out of hyperspace). */
		*level = 1.0f - crawl_smooth((s_crawl.t - (end - XWA_VR_HOTH_TUNNEL_FADE)) / XWA_VR_HOTH_TUNNEL_FADE);
	}
	return 1;
}

int XwaVrIntroCrawl_Lighting(XwaVrEnvLighting* out) {
	return s_crawl.active && crawl_hoth() && XwaVrIntroHoth_Lighting(s_crawl.t, out);
}

/* The film's own stars (vr/intro_stars.rgba) stand in for the menus' until
 * the screen is back; without them the menus' stars come with the logo. */
static float crawl_film_stars(void) {
	if (!s_crawl.active || !s_crawl.stars.tex || s_crawl.t < CRAWL_START) {
		return 0.0f;
	}
	if (crawl_hoth() && s_crawl.t >= XWA_VR_HOTH_HYPER) {
		return 0.0f; /* the tunnel, then the startup films' stars */
	}
	return XwaVrIntroCrawl_Level();
}

float XwaVrIntroCrawl_StarLevel(void) {
	if (!s_crawl.active) {
		return 1.0f;
	}
	if (s_crawl.t < CRAWL_START) {
		return 0.0f;
	}
	if (crawl_hoth() && s_crawl.t >= XWA_VR_HOTH_HYPER) {
		/* Out of the tunnel with the film (147-148 s): the startup stars. */
		return crawl_smooth((s_crawl.t - XWA_VR_HOTH_HYPER_FADE) / (XWA_VR_HOTH_HYPER_END - XWA_VR_HOTH_HYPER_FADE));
	}
	return s_crawl.stars.tex ? 1.0f - XwaVrIntroCrawl_Level() : 1.0f;
}

/* ---- the colours through the tonemap -------------------------------------
 *
 * The VR scene is tonemapped (AeronScenePresent's operator, also in direct
 * output) before the eye gain: to show the film's colours the billboards
 * are tinted with the HDR colour the operator maps onto them, found by
 * inverting it numerically (a few Gauss-Newton steps on log colour). */

static const float k_agx_inset[3][3] = { { 0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f },
										 { 0.0423282422610123f, 0.878468636469772f, 0.0784336f },
										 { 0.0423756549057051f, 0.0784336f, 0.879142973793104f } };
static const float k_agx_outset[3][3] = { { 1.19687900512017f, -0.0980208811401368f, -0.0990297440797205f },
										  { -0.0528968517574562f, 1.15190312990417f, -0.0989611768448433f },
										  { -0.0529716355144438f, -0.0980434501171241f, 1.15107367264116f } };
static const float k_aces_in[3][3] = { { 0.59719f, 0.35458f, 0.04823f },
									   { 0.07600f, 0.90834f, 0.01566f },
									   { 0.02840f, 0.13383f, 0.83777f } };
static const float k_aces_out[3][3] = { { 1.60475f, -0.53108f, -0.07367f },
										{ -0.10208f, 1.10813f, -0.00605f },
										{ -0.00327f, -0.07276f, 1.07602f } };

static void crawl_mul(const float m[3][3], const float v[3], float out[3]) {
	float r[3];
	for (int i = 0; i < 3; ++i) {
		r[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
	}
	memcpy(out, r, sizeof r);
}

/* scene_tonemap.frag.hlsl / scene_direct_output.hlsli: linear HDR -> the
 * displayed sRGB value (0..1), exposure 1, no bloom. */
static void crawl_tonemap(const float in[3], float out[3]) {
	float c[3] = { fmaxf(in[0], 0.0f), fmaxf(in[1], 0.0f), fmaxf(in[2], 0.0f) };
	if (AeronScenePresent_TonemapOp() == AERON_SCENE_TONEMAP_ACES) {
		const float e = AeronScenePresent_AcesExposure();
		for (int k = 0; k < 3; ++k) {
			c[k] *= e;
		}
		crawl_mul(k_aces_in, c, c);
		for (int k = 0; k < 3; ++k) {
			const float a = c[k] * (c[k] + 0.0245786f) - 0.000090537f;
			const float b = c[k] * (0.983729f * c[k] + 0.4329510f) + 0.238081f;
			c[k] = a / b;
		}
		crawl_mul(k_aces_out, c, c);
	} else {
		static const float min_ev = -12.47393f, max_ev = 4.026069f;
		crawl_mul(k_agx_inset, c, c);
		for (int k = 0; k < 3; ++k) {
			const float x = crawl_clamp01((log2f(fmaxf(c[k], 1e-10f)) - min_ev) / (max_ev - min_ev));
			const float x2 = x * x, x4 = x2 * x2;
			c[k] = crawl_clamp01(15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 +
								 0.1191f * x - 0.00232f);
		}
		if (AeronScenePresent_AgxLook() == AERON_SCENE_AGX_LOOK_PUNCHY) {
			const float pw = AeronScenePresent_AgxPunchyPower(), sat = AeronScenePresent_AgxPunchySaturation();
			for (int k = 0; k < 3; ++k) {
				c[k] = powf(fmaxf(c[k], 0.0f), pw);
			}
			const float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
			for (int k = 0; k < 3; ++k) {
				c[k] = l + sat * (c[k] - l);
			}
		}
		crawl_mul(k_agx_outset, c, c);
		const float eotf = AeronScenePresent_EotfExponent();
		for (int k = 0; k < 3; ++k) {
			c[k] = powf(fmaxf(c[k], 0.0f), eotf);
		}
	}
	for (int k = 0; k < 3; ++k) {
		out[k] = crawl_linear_to_srgb(crawl_clamp01(c[k]));
	}
}

/* The linear HDR colour the tonemap shows as `srgb` (0..1, display). */
static void crawl_untonemap(const float srgb[3], float out[3]) {
	float y[3], target[3];
	for (int k = 0; k < 3; ++k) {
		target[k] = srgb[k];
		y[k] = logf(crawl_srgb_to_linear(srgb[k]) + 1e-3f);
	}
	for (int it = 0; it < 40; ++it) {
		float x[3], f0[3], r[3], jac[3][3];
		for (int k = 0; k < 3; ++k) {
			x[k] = expf(y[k]);
		}
		crawl_tonemap(x, f0);
		float err = 0.0f;
		for (int k = 0; k < 3; ++k) {
			r[k] = f0[k] - target[k];
			err += r[k] * r[k];
		}
		if (err < 1e-9f) {
			break;
		}
		for (int j = 0; j < 3; ++j) {
			float yj[3] = { y[0], y[1], y[2] }, xj[3], fj[3];
			yj[j] += 1e-3f;
			for (int k = 0; k < 3; ++k) {
				xj[k] = expf(yj[k]);
			}
			crawl_tonemap(xj, fj);
			for (int k = 0; k < 3; ++k) {
				jac[k][j] = (fj[k] - f0[k]) / 1e-3f;
			}
		}
		/* (J^T J + lambda) dy = -J^T r */
		float a[3][3], g[3];
		for (int i = 0; i < 3; ++i) {
			g[i] = 0.0f;
			for (int j = 0; j < 3; ++j) {
				a[i][j] = 0.0f;
				for (int k = 0; k < 3; ++k) {
					a[i][j] += jac[k][i] * jac[k][j];
				}
			}
			for (int k = 0; k < 3; ++k) {
				g[i] -= jac[k][i] * r[k];
			}
			a[i][i] += 1e-4f + 1e-2f * a[i][i];
		}
		const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
						  a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
						  a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
		if (fabsf(det) < 1e-20f) {
			break;
		}
		float dy[3];
		for (int i = 0; i < 3; ++i) {
			float m[3][3];
			memcpy(m, a, sizeof m);
			for (int k = 0; k < 3; ++k) {
				m[k][i] = g[k];
			}
			dy[i] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
					 m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) /
					det;
		}
		for (int k = 0; k < 3; ++k) {
			y[k] = fminf(fmaxf(y[k] + fminf(fmaxf(dy[k], -2.0f), 2.0f), -12.0f), 4.0f);
		}
	}
	for (int k = 0; k < 3; ++k) {
		out[k] = expf(y[k]) < 1e-5f ? 0.0f : expf(y[k]);
	}
}

/* The tint that shows `srgb8` faded to `fade` (display values scale with
 * it, as the film fades). */
static void crawl_tint(const uint8_t srgb8[3], float fade, float out[3]) {
	const float d[3] = { (float)srgb8[0] / 255.0f * fade, (float)srgb8[1] / 255.0f * fade,
						 (float)srgb8[2] / 255.0f * fade };
	crawl_untonemap(d, out);
}

/* ---- drawing ------------------------------------------------------------------ */

/* The film's picture on the virtual screen (LOCAL metres: x right, y up,
 * -z ahead): the 4:3 frame fitted into the screen, the 640x400 film across
 * its full width, centred. A film pixel (x, y) is seen along
 * ((x - CX) * m, lift - (y - CY) * m, -dist). */
typedef struct CrawlView {
	float dist, lift, m;
	float f; /* the film's focal length in film pixels seen from the eye */
	float k; /* world units per metre */
	float org[3], right[3], up[3], fwd[3];
} CrawlView;

static int crawl_view(CrawlView* v, const float view_pos[3], const float rows[9]) {
	float dist = 2.1f, sw = 2.1f, sh = 1.575f, lift = 0.0f;
	if (!Aeron_XrVirtualScreenGeometry(&dist, &sw, &sh, &lift)) {
		dist = crawl_env("AERON_XR_SCREEN_DISTANCE", dist);
		sw = crawl_env("AERON_XR_SCREEN_WIDTH", sw);
		sh = 0.75f * sw;
		lift = crawl_env("AERON_XR_SCREEN_HEIGHT_OFFSET", lift);
	}
	const float w = fminf(sw, sh * 4.0f / 3.0f);
	if (dist <= 0.05f || w <= 0.01f) {
		return 0;
	}
	v->dist = dist;
	v->lift = lift;
	v->m = w / CRAWL_FILM_W;
	v->f = dist / v->m;
	v->k = AERON_OPT_UNITS_PER_METER * crawl_env("XWA_XR_WORLD_SCALE", 1.0f);
	for (int i = 0; i < 3; ++i) {
		v->org[i] = view_pos[i];
		v->right[i] = rows[i];
		v->up[i] = -rows[3 + i];
		v->fwd[i] = rows[6 + i];
	}
	return 1;
}

/* LOCAL metres -> world. */
static void crawl_world(const CrawlView* v, float x, float y, float z, float out[3]) {
	for (int i = 0; i < 3; ++i) {
		out[i] = v->org[i] + v->k * (x * v->right[i] + y * v->up[i] - z * v->fwd[i]);
	}
}

/* A quad of a coverage texture made for colour `srgb8` (tint `full` at
 * full strength), faded to `fade` (the picture's values scale with it). */
static void crawl_quad(AeronScene3D* scene, AeronTexture* tex, const float c[4][3], const float uv[4][2],
					   const uint8_t srgb8[3], const float full[3], float fade) {
	float rgb[3];
	if (fade >= 0.999f) {
		memcpy(rgb, full, sizeof rgb);
	} else {
		/* The texture's levels are for full strength: scale the tint so
		 * the full-coverage colour shows faded (edges follow closely). */
		crawl_tint(srgb8, fade, rgb);
	}
	AeronSceneBillboardDesc d;
	memset(&d, 0, sizeof d);
	d.texture = tex;
	d.blend = AERON_SCENE_BILLBOARD_BLEND_PMA;
	d.stage = AERON_SCENE_BILLBOARD_STAGE_OVERLAY;
	for (int i = 0; i < 4; ++i) {
		memcpy(d.corners[i], c[i], sizeof d.corners[i]);
		memcpy(d.uv[i], uv[i], sizeof d.uv[i]);
		/* Premultiplied: `rgb` is the colour as shown (its fade in it). */
		for (int k = 0; k < 3; ++k) {
			d.colors[i][k] = rgb[k];
		}
		d.colors[i][3] = fade;
	}
	AeronScene_AddBillboard(scene, &d);
}

/* The logo: square to the line of sight through the picture's centre, at
 * the distance that gives the film's size; it starts
 * XWA_XR_CRAWL_LOGO_DISTANCE metres away. */
static void crawl_submit_logo(AeronScene3D* scene, const CrawlView* v) {
	const float t = s_crawl.t;
	const float logo_end = CRAWL_START + fmaxf(crawl_env("XWA_XR_CRAWL_LOGO_END", CRAWL_LOGO_END), 0.1f);
	const float logo_fade = fminf(fmaxf(crawl_env("XWA_XR_CRAWL_LOGO_FADE", CRAWL_LOGO_FADE), 0.01f),
								  logo_end - CRAWL_START);
	if (!s_crawl.logo.tex || t < CRAWL_START || t >= logo_end) {
		return;
	}
	const double u = (double)(t - CRAWL_START);
	const double inv_h = k_logo_inv_h[0] + u * (k_logo_inv_h[1] + u * (k_logo_inv_h[2] + u * k_logo_inv_h[3]));
	const float d0 = fmaxf(crawl_env("XWA_XR_CRAWL_LOGO_DISTANCE", 5.0f), 0.5f);
	const float dist = d0 * (float)(inv_h / k_logo_inv_h[0]);
	/* The outline's height in metres there (the same all the way). */
	const float oh = (float)(1.0 / k_logo_inv_h[0]) * v->m * d0 / v->dist;
	const float ow = oh * CRAWL_LOGO_ASPECT;
	const float* b = s_crawl.logo_box;
	const float qw = ow / (b[2] - b[0]), qh = oh / (b[3] - b[1]);
	/* The outline's centre on the line of sight to the film's centre. */
	const float cx = 0.0f, cy = v->lift * dist / v->dist;
	const float x0 = cx - (0.5f * (b[0] + b[2])) * qw, x1 = x0 + qw;
	const float y1 = cy + (0.5f * (b[1] + b[3])) * qh, y0 = y1 - qh; /* image v runs down */
	float c[4][3];
	crawl_world(v, x0, y0, -dist, c[0]);
	crawl_world(v, x1, y0, -dist, c[1]);
	crawl_world(v, x1, y1, -dist, c[2]);
	crawl_world(v, x0, y1, -dist, c[3]);
	static const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	const float fade = 1.0f - crawl_clamp01((t - (logo_end - logo_fade)) / logo_fade);
	crawl_quad(scene, s_crawl.logo.tex, c, uv, k_logo_srgb, s_crawl.logo_tint, fade);
}

/* The crawl's plane. The film's camera looks down at it: its vanishing
 * line YV lies f * tan(phi) above the picture's centre, phi the camera's
 * pitch, h (XWA_XR_CRAWL_HEIGHT) its height above the plane. A text point
 * (xn, s) is then, in the film camera's frame (x right, y up, z ahead),
 * at depth z = f h (t + T0 - s) / (K cos phi), x = xn h / cos phi and
 * y = z tan phi - h / cos phi; the eye sees the screen as that camera's
 * picture, so in LOCAL space the point lies at (x, y + z lift / dist, -z).
 * That is linear in xn and s: one quad. */
/* How late the scroll runs (TEXT_ENTRY_DEG, above): a point (xn, s) is
 * seen at tan(elevation) = tan(phi) + lift / dist - K / (f (t + T0 - s)),
 * the plane's height cancelling, so the first line's top (s = 0) is at the
 * wanted elevation at scroll time K / (f (tan(phi) + lift / dist -
 * tan(elevation))) - T0. */
static float crawl_scroll_delay(const CrawlView* v) {
	const float tan_phi = (CRAWL_FILM_CY - CRAWL_YV) / v->f;
	const float entry = fminf(fmaxf(crawl_env("XWA_XR_CRAWL_TEXT_ENTRY_DEG", CRAWL_TEXT_ENTRY_DEG), 0.0f), 80.0f);
	const float at = CRAWL_START + crawl_env("XWA_XR_CRAWL_TEXT_ENTRY_AT", CRAWL_TEXT_ENTRY_AT);
	const float logo_el = atanf(v->lift / v->dist);
	const float tan_el = tanf(logo_el - entry * 0.017453293f);
	const float den = tan_phi + v->lift / v->dist - tan_el;
	if (entry <= 0.0f || den <= 1e-3f) {
		return 0.0f;
	}
	return fmaxf(at - (CRAWL_K / (v->f * den) - CRAWL_T0), 0.0f);
}

static void crawl_plane_point(const CrawlView* v, float xn, float s, float out[3]) {
	const float h = fmaxf(crawl_env("XWA_XR_CRAWL_HEIGHT", 12.0f), 1.0f);
	const float tan_phi = (CRAWL_FILM_CY - CRAWL_YV) / v->f;
	const float inv_cos = sqrtf(1.0f + tan_phi * tan_phi);
	const float t = s_crawl.t - crawl_scroll_delay(v);
	const float z = v->f * h * (t + CRAWL_T0 - s) * inv_cos / CRAWL_K;
	const float x = (xn + 0.0f) * h * inv_cos;
	const float y = z * tan_phi - h * inv_cos;
	crawl_world(v, x, y + z * v->lift / v->dist, -z, out);
}

static void crawl_submit_text(AeronScene3D* scene, const CrawlView* v) {
	const float t = s_crawl.t;
	const float text_from = CRAWL_START + fmaxf(crawl_env("XWA_XR_CRAWL_TEXT_FROM", CRAWL_TEXT_FROM), 0.0f);
	const float fade_in = fmaxf(crawl_env("XWA_XR_CRAWL_TEXT_FADE_IN", CRAWL_TEXT_FADE_IN), 0.01f);
	if (!s_crawl.text.tex || t < text_from || t >= CRAWL_FADE_TO) {
		return;
	}
	/* The film's centre column sits at xn 0 (x = CX). Lines still to come
	 * (t + T0 - s <= 0) would lie on the plane behind the player: the quad
	 * stops a little short of them. */
	const float xa = CRAWL_TEX_X0, xb = -CRAWL_TEX_X0;
	const float sa = CRAWL_TEX_S0, s1 = s_crawl.text_s1;
	const float s_here = t - crawl_scroll_delay(v) + CRAWL_T0 - 0.25f;
	const float sb = fminf(s1, s_here);
	if (sb <= sa) {
		return;
	}
	const float vb = (sb - sa) / (s1 - sa);
	float c[4][3];
	crawl_plane_point(v, xa, sb, c[0]);
	crawl_plane_point(v, xb, sb, c[1]);
	crawl_plane_point(v, xb, sa, c[2]);
	crawl_plane_point(v, xa, sa, c[3]);
	const float uv[4][2] = { { 0, vb }, { 1, vb }, { 1, 0 }, { 0, 0 } };
	const float fade = fminf(crawl_clamp01((t - text_from) / fade_in),
							 1.0f - crawl_clamp01((t - CRAWL_FADE_FROM) / (CRAWL_FADE_TO - CRAWL_FADE_FROM)));
	crawl_quad(scene, s_crawl.text.tex, c, uv, k_text_srgb, s_crawl.text_tint, fade);
}

/* The film's star field all round: straight ahead exactly where the film
 * has it (so the screen's stars land on them at the handover), and the
 * same patch, flipped about, tiled over the sky in bands of its height
 * (gnomonic tiles; they overlap a little). Sky stage: under everything. */
static void crawl_star_tile(AeronScene3D* scene, const CrawlView* v, const float dir[3], const float right[3],
							const float up[3], float hw, float hh, int flip, float fade) {
	const float r = 1000.0f; /* metres: any far distance gives the direction */
	float c[4][3];
	const float sx[4] = { -1.0f, 1.0f, 1.0f, -1.0f }, sy[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
	for (int i = 0; i < 4; ++i) {
		float l[3];
		for (int k = 0; k < 3; ++k) {
			l[k] = r * (dir[k] + sx[i] * hw * right[k] + sy[i] * hh * up[k]);
		}
		crawl_world(v, l[0], l[1], l[2], c[i]);
	}
	float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	for (int i = 0; i < 4; ++i) {
		if (flip & 1) {
			uv[i][0] = 1.0f - uv[i][0];
		}
		if (flip & 2) {
			uv[i][1] = 1.0f - uv[i][1];
		}
	}
	AeronSceneBillboardDesc d;
	memset(&d, 0, sizeof d);
	d.texture = s_crawl.stars.tex;
	d.blend = AERON_SCENE_BILLBOARD_BLEND_PMA;
	d.stage = AERON_SCENE_BILLBOARD_STAGE_SKY;
	for (int i = 0; i < 4; ++i) {
		memcpy(d.corners[i], c[i], sizeof d.corners[i]);
		memcpy(d.uv[i], uv[i], sizeof d.uv[i]);
		for (int k = 0; k < 3; ++k) {
			d.colors[i][k] = s_crawl.star_tint[k] * fade;
		}
		d.colors[i][3] = fade;
	}
	AeronScene_AddBillboard(scene, &d);
}

static void crawl_submit_stars(AeronScene3D* scene, const CrawlView* v) {
	const float fade = crawl_film_stars();
	if (fade <= 0.0f) {
		return;
	}
	/* The film's picture: half extents as tangents from the eye. */
	const float hw = 0.5f * CRAWL_FILM_W * v->m / v->dist, hh = 200.0f * v->m / v->dist;
	{
		/* Straight ahead, exactly over the picture (its centre lifted). */
		const float dir[3] = { 0.0f, v->lift / v->dist, -1.0f };
		static const float right[3] = { 1.0f, 0.0f, 0.0f }, up[3] = { 0.0f, 1.0f, 0.0f };
		crawl_star_tile(scene, v, dir, right, up, hw, hh, 0, fade);
	}
	const float tile_w = 2.0f * atanf(hw), tile_h = 2.0f * atanf(hh); /* radians */
	const float pi = 3.14159265358979f;
	int index = 1;
	for (int band = -2; band <= 2; ++band) {
		const float pitch = (float)band * tile_h;
		/* Enough tiles round the band's widest edge. */
		const float widest = fmaxf(fabsf(pitch) - 0.5f * tile_h, 0.0f);
		const int n = (int)ceilf(2.0f * pi * cosf(widest) / tile_w);
		for (int k = 0; k < n; ++k) {
			if (band == 0 && k == 0) {
				continue; /* the film's own, above */
			}
			const float yaw = 2.0f * pi * (float)k / (float)n + (band & 1 ? 0.5f * tile_w : 0.0f);
			const float cp = cosf(pitch), sp = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw);
			const float dir[3] = { sy * cp, sp, -cy * cp };
			const float right[3] = { cy, 0.0f, sy };
			const float up[3] = { -sy * sp, cp, cy * sp };
			crawl_star_tile(scene, v, dir, right, up, hw, hh, index++ & 3, fade);
		}
	}
	/* The poles. */
	for (int pole = -1; pole <= 1; pole += 2) {
		const float dir[3] = { 0.0f, (float)pole, 0.0f };
		static const float right[3] = { 1.0f, 0.0f, 0.0f };
		const float up[3] = { 0.0f, 0.0f, (float)pole };
		crawl_star_tile(scene, v, dir, right, up, hw, hh, index++ & 3, fade);
	}
}

void XwaVrIntroCrawl_Submit(AeronScene3D* scene, const float view_pos[3], const float rows[9]) {
	CrawlView v;
	if (!scene || !s_crawl.active || !crawl_view(&v, view_pos, rows)) {
		return;
	}
	crawl_submit_stars(scene, &v);
	if (crawl_hoth()) {
		XwaVrIntroHoth_Submit(scene, v.org, v.right, v.up, v.fwd, v.k, s_crawl.t, v.dist, v.m * CRAWL_FILM_W, v.lift);
	}
	crawl_submit_text(scene, &v);
	crawl_submit_logo(scene, &v);
}
