#include "xwa_remaster/vr_hud_occlusion.h"
#include <SDL3/SDL_stdinc.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aeron/scene/draw_list2d.h"

/* Mask width in texels; the height follows the HUD image's aspect. The
 * mask is bilinear-filtered up to the HUD size, which softens the cut
 * edge by about a texel (a fraction of a degree). */
enum { HO_MASK_W = 384, HO_MASK_H_MAX = 384, HO_MAX_VERTS = 65536 };

/* Eye-plane clip: geometry closer than this ahead of an eye is ignored
 * (the parts beside the head cannot cover the quad ahead). */
#define HO_NEAR_M 0.02f

static struct {
	int enabled_checked;
	int enabled;
	/* This frame's cockpit. */
	int have;
	const AeronSceneMesh* mesh;
	float model[16];
	int has_table;
	AeronSceneMeshTable table;
	float rows[9];
	float cam[3];
	float units;
	/* Scratch. */
	float* lp;  /* LOCAL positions, 3 per vertex */
	float* ln;  /* LOCAL normals */
	uint8_t* hidden;
	int cap;
	uint8_t px[HO_MASK_W * HO_MASK_H_MAX * 4];
	int mw, mh;
	AeronTexture* tex;
	AeronDrawList2D* list;
	int ready;
	/* What the last mask was made for: the cockpit only moves against the
	 * world HUD with the eyes' positions (not with the head turning), so the
	 * mask is kept while those stay within a millimetre or two. */
	int cached;
	const AeronSceneMesh* cached_mesh;
	float cached_model[16];
	AeronSceneMeshTable cached_table;
	int cached_has_table;
	float cached_eye[2][3];
	AeronXrHudQuad cached_quad;
	int cached_w, cached_h;
} s_ho;

static int ho_enabled(void) {
	if (!s_ho.enabled_checked) {
		const char* env = SDL_getenv("XWA_XR_HUD_OCCLUDE");
		s_ho.enabled = !(env && env[0] == '0');
		s_ho.enabled_checked = 1;
	}
	return s_ho.enabled;
}

void XwaVrHudOcclusion_Reset(void) {
	s_ho.have = 0;
	s_ho.ready = 0;
}

void XwaVrHudOcclusion_SetCockpit(const AeronSceneMesh* mesh, const float model[16], const AeronSceneMeshTable* table,
								  const float camera_rows[9], const float camera_pos[3], float units_per_m) {
	if (!ho_enabled() || !mesh || !mesh->cpu_vertices || !mesh->cpu_indices || !model || !camera_rows ||
		!camera_pos || units_per_m <= 0.0f) {
		return;
	}
	s_ho.mesh = mesh;
	memcpy(s_ho.model, model, sizeof s_ho.model);
	s_ho.has_table = table != NULL;
	if (table) {
		s_ho.table = *table;
	}
	memcpy(s_ho.rows, camera_rows, sizeof s_ho.rows);
	memcpy(s_ho.cam, camera_pos, sizeof s_ho.cam);
	s_ho.units = units_per_m;
	s_ho.have = 1;
}

/* One triangle (mask texel space) into the mask's alpha, coverage at texel
 * centres, either winding. */
static void ho_tri(float x0, float y0, float x1, float y1, float x2, float y2) {
	const int mw = s_ho.mw, mh = s_ho.mh;
	const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
	if (fabsf(area) < 1e-6f) {
		return;
	}
	if (area < 0.0f) {
		float t = x1;
		x1 = x2;
		x2 = t;
		t = y1;
		y1 = y2;
		y2 = t;
	}
	const float fx0 = fminf(x0, fminf(x1, x2)), fy0 = fminf(y0, fminf(y1, y2));
	const float fx1 = fmaxf(x0, fmaxf(x1, x2)), fy1 = fmaxf(y0, fmaxf(y1, y2));
	if (fx1 < 0.0f || fy1 < 0.0f || fx0 > (float)mw || fy0 > (float)mh) {
		return;
	}
	int ix0 = (int)fmaxf(fx0, 0.0f), iy0 = (int)fmaxf(fy0, 0.0f);
	int ix1 = (int)fminf(fx1, (float)mw) + 1, iy1 = (int)fminf(fy1, (float)mh) + 1;
	ix1 = ix1 > mw ? mw : ix1;
	iy1 = iy1 > mh ? mh : iy1;
	if (ix0 >= ix1 || iy0 >= iy1) {
		return;
	}
	const float a0 = -(y1 - y0), b0 = x1 - x0, a1 = -(y2 - y1), b1 = x2 - x1, a2 = -(y0 - y2), b2 = x0 - x2;
	const float sx = (float)ix0 + 0.5f, sy = (float)iy0 + 0.5f;
	float r0 = a0 * (sx - x0) + b0 * (sy - y0);
	float r1 = a1 * (sx - x1) + b1 * (sy - y1);
	float r2 = a2 * (sx - x2) + b2 * (sy - y2);
	for (int y = iy0; y < iy1; ++y) {
		float e0 = r0, e1 = r1, e2 = r2;
		uint8_t* row = &s_ho.px[(y * mw) * 4 + 3];
		for (int x = ix0; x < ix1; ++x) {
			if (e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f) {
				row[x * 4] = 255;
			}
			e0 += a0;
			e1 += a1;
			e2 += a2;
		}
		r0 += b0;
		r1 += b1;
		r2 += b2;
	}
}

static int ho_scratch(int n) {
	if (n <= s_ho.cap) {
		return 1;
	}
	free(s_ho.lp);
	free(s_ho.ln);
	free(s_ho.hidden);
	s_ho.lp = (float*)malloc((size_t)n * 3 * sizeof(float));
	s_ho.ln = (float*)malloc((size_t)n * 3 * sizeof(float));
	s_ho.hidden = (uint8_t*)malloc((size_t)n);
	if (!s_ho.lp || !s_ho.ln || !s_ho.hidden) {
		free(s_ho.lp);
		free(s_ho.ln);
		free(s_ho.hidden);
		s_ho.lp = s_ho.ln = NULL;
		s_ho.hidden = NULL;
		s_ho.cap = 0;
		return 0;
	}
	s_ho.cap = n;
	return 1;
}

/* Mesh vertices -> LOCAL space (metres; x right, y up, -z ahead). */
static void ho_transform(void) {
	const AeronSceneMesh* mesh = s_ho.mesh;
	const float* m = s_ho.model;
	const float* r = s_ho.rows;
	const float inv_u = 1.0f / s_ho.units;
	for (uint32_t i = 0; i < mesh->vertex_count; ++i) {
		const AeronSceneMeshCpuVertex* v = &mesh->cpu_vertices[i];
		float p[3] = { v->pos[0], v->pos[1], v->pos[2] };
		float n[3] = { v->normal[0], v->normal[1], v->normal[2] };
		s_ho.hidden[i] = 0;
		if (s_ho.has_table) {
			const int mi = (int)(v->mesh_index + 0.5f);
			if (mi >= 0 && mi < AERON_MAX_MESH_SLOTS) {
				if (s_ho.table.visibility_packed[mi >> 2][mi & 3] <= 0.0f) {
					s_ho.hidden[i] = 1;
					continue;
				}
				const float (*t)[4] = s_ho.table.rows[mi];
				float q[3], qn[3];
				for (int a = 0; a < 3; ++a) {
					q[a] = t[a][0] * p[0] + t[a][1] * p[1] + t[a][2] * p[2] + t[a][3];
					qn[a] = t[a][0] * n[0] + t[a][1] * n[1] + t[a][2] * n[2];
				}
				memcpy(p, q, sizeof p);
				memcpy(n, qn, sizeof n);
			}
		}
		float w[3], wn[3];
		for (int a = 0; a < 3; ++a) {
			w[a] = m[a * 4 + 0] * p[0] + m[a * 4 + 1] * p[1] + m[a * 4 + 2] * p[2] + m[a * 4 + 3] - s_ho.cam[a];
			wn[a] = m[a * 4 + 0] * n[0] + m[a * 4 + 1] * n[1] + m[a * 4 + 2] * n[2];
		}
		/* World -> ship eye (x right, y down, z ahead) -> LOCAL. */
		float* o = &s_ho.lp[i * 3];
		float* on = &s_ho.ln[i * 3];
		o[0] = (r[0] * w[0] + r[1] * w[1] + r[2] * w[2]) * inv_u;
		o[1] = -(r[3] * w[0] + r[4] * w[1] + r[5] * w[2]) * inv_u;
		o[2] = -(r[6] * w[0] + r[7] * w[1] + r[8] * w[2]) * inv_u;
		on[0] = r[0] * wn[0] + r[1] * wn[1] + r[2] * wn[2];
		on[1] = -(r[3] * wn[0] + r[4] * wn[1] + r[5] * wn[2]);
		on[2] = -(r[6] * wn[0] + r[7] * wn[1] + r[8] * wn[2]);
	}
}

/* The cockpit's front faces seen from `eye`, projected onto the quad's
 * plane, into the mask. */
static void ho_eye(const float eye[3], const AeronXrHudQuad* quad) {
	const AeronSceneMesh* mesh = s_ho.mesh;
	const float plane_z = quad->position[2];
	const float x0 = quad->position[0] - 0.5f * quad->size[0];
	const float y1 = quad->position[1] + 0.5f * quad->size[1];
	const float sx = (float)s_ho.mw / quad->size[0];
	const float sy = (float)s_ho.mh / quad->size[1];
	const float near_z = eye[2] - HO_NEAR_M;
	if (plane_z >= near_z) {
		return;
	}
	const uint32_t count = mesh->opaque_index_count < mesh->index_count ? mesh->opaque_index_count : mesh->index_count;
	for (uint32_t t = 0; t + 2 < count; t += 3) {
		const uint32_t k[3] = { mesh->cpu_indices[t], mesh->cpu_indices[t + 1], mesh->cpu_indices[t + 2] };
		if (k[0] >= mesh->vertex_count || k[1] >= mesh->vertex_count || k[2] >= mesh->vertex_count ||
			s_ho.hidden[k[0]] || s_ho.hidden[k[1]] || s_ho.hidden[k[2]]) {
			continue;
		}
		const float* p[3] = { &s_ho.lp[k[0] * 3], &s_ho.lp[k[1] * 3], &s_ho.lp[k[2] * 3] };
		/* Entirely behind the eye's near plane. */
		if (p[0][2] >= near_z && p[1][2] >= near_z && p[2][2] >= near_z) {
			continue;
		}
		/* Back faces: the cockpit is drawn with back-face culling. */
		int facing = 0;
		for (int e = 0; e < 3 && !facing; ++e) {
			const float* n = &s_ho.ln[k[e] * 3];
			facing = n[0] * (eye[0] - p[e][0]) + n[1] * (eye[1] - p[e][1]) + n[2] * (eye[2] - p[e][2]) > 0.0f;
		}
		if (!facing) {
			continue;
		}
		/* Clip against the near plane, then project through the eye. */
		float mx[4], my[4];
		int n = 0;
		for (int e = 0; e < 3; ++e) {
			const float* a = p[e];
			const float* b = p[(e + 1) % 3];
			const int ina = a[2] < near_z, inb = b[2] < near_z;
			float q[3];
			for (int c = 0; c < 2; ++c) {
				const float* src;
				if (c == 0) {
					if (!ina) {
						continue;
					}
					src = a;
				} else {
					if (ina == inb || n >= 4) {
						continue;
					}
					const float u = (near_z - a[2]) / (b[2] - a[2]);
					q[0] = a[0] + (b[0] - a[0]) * u;
					q[1] = a[1] + (b[1] - a[1]) * u;
					q[2] = near_z;
					src = q;
				}
				const float dz = src[2] - eye[2];
				const float s = (plane_z - eye[2]) / dz;
				const float hx = eye[0] + (src[0] - eye[0]) * s;
				const float hy = eye[1] + (src[1] - eye[1]) * s;
				mx[n] = (hx - x0) * sx;
				my[n] = (y1 - hy) * sy;
				n++;
			}
		}
		for (int j = 1; j + 1 < n; ++j) {
			ho_tri(mx[0], my[0], mx[j], my[j], mx[j + 1], my[j + 1]);
		}
	}
}

int XwaVrHudOcclusion_Prepare(AeronCommandBuffer* cmd, const AeronXrView views[2], const AeronXrHudQuad* quad,
							  int target_w, int target_h) {
	s_ho.ready = 0;
	if (!s_ho.have || !cmd || !views || !quad || !quad->visible || target_w <= 0 || target_h <= 0 ||
		quad->size[0] <= 0.0f || quad->size[1] <= 0.0f || quad->head_locked) {
		return 0;
	}
	const AeronSceneMesh* mesh = s_ho.mesh;
	if (mesh->vertex_count == 0 || mesh->vertex_count > HO_MAX_VERTS || !ho_scratch((int)mesh->vertex_count)) {
		return 0;
	}
	/* The same view of the same cockpit as last frame: the same mask. */
	if (s_ho.cached && s_ho.cached_mesh == mesh && s_ho.cached_w == target_w && s_ho.cached_h == target_h &&
		memcmp(s_ho.cached_model, s_ho.model, sizeof s_ho.model) == 0 && s_ho.cached_has_table == s_ho.has_table &&
		(!s_ho.has_table || memcmp(&s_ho.cached_table, &s_ho.table, sizeof s_ho.table) == 0) &&
		memcmp(&s_ho.cached_quad, quad, sizeof *quad) == 0) {
		float moved = 0.0f;
		for (int e = 0; e < 2; ++e) {
			const float dx = views[e].position[0] - s_ho.cached_eye[e][0];
			const float dy = views[e].position[1] - s_ho.cached_eye[e][1];
			const float dz = views[e].position[2] - s_ho.cached_eye[e][2];
			moved = fmaxf(moved, dx * dx + dy * dy + dz * dz);
		}
		if (moved < 0.0015f * 0.0015f) {
			s_ho.ready = s_ho.cached > 0;
			return s_ho.ready;
		}
	}
	s_ho.cached = 0;
	s_ho.cached_mesh = mesh;
	memcpy(s_ho.cached_model, s_ho.model, sizeof s_ho.model);
	s_ho.cached_has_table = s_ho.has_table;
	if (s_ho.has_table) {
		s_ho.cached_table = s_ho.table;
	}
	s_ho.cached_quad = *quad;
	s_ho.cached_w = target_w;
	s_ho.cached_h = target_h;
	for (int e = 0; e < 2; ++e) {
		memcpy(s_ho.cached_eye[e], views[e].position, sizeof s_ho.cached_eye[e]);
	}
	s_ho.mw = HO_MASK_W;
	s_ho.mh = (int)((float)HO_MASK_W * (float)target_h / (float)target_w + 0.5f);
	s_ho.mh = s_ho.mh < 16 ? 16 : (s_ho.mh > HO_MASK_H_MAX ? HO_MASK_H_MAX : s_ho.mh);
	memset(s_ho.px, 0, (size_t)s_ho.mw * (size_t)s_ho.mh * 4);
	ho_transform();
	for (int e = 0; e < 2; ++e) {
		ho_eye(views[e].position, quad);
	}
	int any = 0;
	for (int i = 3; i < s_ho.mw * s_ho.mh * 4 && !any; i += 4) {
		any = s_ho.px[i] != 0;
	}
	if (!any) {
		s_ho.cached = -1; /* nothing to cut, for this view */
		return 0;
	}
	if (!s_ho.tex) {
		s_ho.tex = Aeron_CreateTexture(&(AeronTextureDesc) {
			.width = HO_MASK_W,
			.height = HO_MASK_H_MAX,
			.mip_count = 1,
			.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
			.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
			.debug_name = "xwa.vr_hud.cockpit_mask",
		});
		if (!s_ho.tex) {
			return 0;
		}
	}
	if (!s_ho.list) {
		s_ho.list = AeronDrawList_Create(4);
		if (!s_ho.list) {
			return 0;
		}
	}
	if (!Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
			.texture = s_ho.tex,
			.width = s_ho.mw,
			.height = s_ho.mh,
			.pixels = s_ho.px,
			.pitch = s_ho.mw * 4,
			.pixel_format = AERON_PIXEL_FORMAT_RGBA8888,
			.cycle = 1,
		})) {
		return 0;
	}
	AeronDrawList_Begin(s_ho.list, NULL, target_w, target_h, AERON_DRAWLIST2D_LOAD, NULL);
	AeronDrawList2DSprite sp;
	memset(&sp, 0, sizeof sp);
	sp.texture = s_ho.tex;
	sp.src_u0 = 0.0f;
	sp.src_v0 = 0.0f;
	sp.src_u1 = (float)s_ho.mw / (float)HO_MASK_W;
	sp.src_v1 = (float)s_ho.mh / (float)HO_MASK_H_MAX;
	sp.dst_x = 0.0f;
	sp.dst_y = 0.0f;
	sp.dst_w = (float)target_w;
	sp.dst_h = (float)target_h;
	sp.tint[0] = sp.tint[1] = sp.tint[2] = sp.tint[3] = 1.0f;
	sp.blend = AERON_BLIT2D_BLEND_ERASE;
	sp.filter = AERON_BLIT2D_FILTER_LINEAR;
	AeronDrawList_AddSprite(s_ho.list, &sp);
	s_ho.ready = AeronDrawList_Prepare(s_ho.list, cmd);
	s_ho.cached = s_ho.ready;
	return s_ho.ready;
}

void XwaVrHudOcclusion_Render(AeronCommandBuffer* cmd, AeronRenderPass* pass, AeronRenderTarget* target) {
	if (s_ho.ready && s_ho.list && cmd && pass && target) {
		AeronDrawList_RenderIntoPass(s_ho.list, cmd, pass, target);
	}
}

void XwaVrHudOcclusion_Shutdown(void) {
	if (s_ho.list) {
		AeronDrawList_Destroy(s_ho.list);
	}
	if (s_ho.tex) {
		Aeron_DestroyTexture(s_ho.tex);
	}
	free(s_ho.lp);
	free(s_ho.ln);
	free(s_ho.hidden);
	memset(&s_ho, 0, sizeof s_ho);
}
