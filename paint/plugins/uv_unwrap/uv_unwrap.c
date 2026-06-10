
#include "uv_unwrap.h"
#include "iron_system.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cosine of 66 degrees - angle threshold for chart grouping
#define UV_ANGLE_THRESHOLD 0.4067f
#define UV_PACK_MARGIN     0.001f
#define UV_PACK_EPS        1e-6f

// Position hash map entry for canonical vertex deduplication
typedef struct {
	int16_t x, y, z;
	int     id;
	bool    occupied;
} uv_pos_entry_t;

// Edge hash map entry for face adjacency
typedef struct {
	int  v0, v1;
	int  face0, face1;
	bool occupied;
} uv_edge_entry_t;

// 2D point for convex hull / min-area rectangle
typedef struct {
	float u, v;
} uv_pt_t;

// Chart sort key for packing order
typedef struct {
	float key;
	int   id;
} uv_sort_t;

static uint32_t uv_hash_pos(int16_t x, int16_t y, int16_t z) {
	uint32_t h = (uint32_t)(x + 32768);
	h          = h * 2654435761u ^ (uint32_t)(y + 32768);
	h          = h * 2654435761u ^ (uint32_t)(z + 32768);
	return h;
}

static uint32_t uv_hash_edge(int v0, int v1) {
	return (uint32_t)v0 * 2654435761u ^ (uint32_t)v1 * 2246822519u;
}

static int uv_pt_cmp(const void *a, const void *b) {
	const uv_pt_t *p = (const uv_pt_t *)a;
	const uv_pt_t *q = (const uv_pt_t *)b;
	if (p->u != q->u) {
		return p->u < q->u ? -1 : 1;
	}
	if (p->v != q->v) {
		return p->v < q->v ? -1 : 1;
	}
	return 0;
}

static int uv_sort_cmp(const void *a, const void *b) {
	float ka = ((const uv_sort_t *)a)->key;
	float kb = ((const uv_sort_t *)b)->key;
	if (ka != kb) {
		return ka > kb ? -1 : 1; // Descending
	}
	return 0;
}

static float uv_cross(uv_pt_t o, uv_pt_t a, uv_pt_t b) {
	return (a.u - o.u) * (b.v - o.v) - (a.v - o.v) * (b.u - o.u);
}

// Reduce hull candidates for large point sets: keep only per-column v-extremes.
// All discarded points are interior in v within their column, so the hull of the
// kept points closely matches the true hull. Returns new count.
#define UV_HULL_COLS 256
static int uv_hull_prefilter(uv_pt_t *pts, int n) {
	if (n <= UV_HULL_COLS * 2) {
		return n;
	}
	float min_u = FLT_MAX;
	float max_u = -FLT_MAX;
	for (int i = 0; i < n; i++) {
		if (pts[i].u < min_u)
			min_u = pts[i].u;
		if (pts[i].u > max_u)
			max_u = pts[i].u;
	}
	if (max_u - min_u < 1e-12f) {
		return n;
	}
	float   col_scale = (UV_HULL_COLS - 1) / (max_u - min_u);
	uv_pt_t col_min[UV_HULL_COLS];
	uv_pt_t col_max[UV_HULL_COLS];
	bool    col_used[UV_HULL_COLS];
	memset(col_used, 0, sizeof(col_used));
	for (int i = 0; i < n; i++) {
		int col = (int)((pts[i].u - min_u) * col_scale);
		if (!col_used[col]) {
			col_used[col] = true;
			col_min[col]  = pts[i];
			col_max[col]  = pts[i];
		}
		else {
			if (pts[i].v < col_min[col].v || (pts[i].v == col_min[col].v && pts[i].u < col_min[col].u))
				col_min[col] = pts[i];
			if (pts[i].v > col_max[col].v || (pts[i].v == col_max[col].v && pts[i].u > col_max[col].u))
				col_max[col] = pts[i];
		}
	}
	int k = 0;
	for (int col = 0; col < UV_HULL_COLS; col++) {
		if (col_used[col]) {
			pts[k++] = col_min[col];
			pts[k++] = col_max[col];
		}
	}
	return k;
}

// Monotone chain convex hull; sorts pts in place, out must hold 2 * n + 1 points
static int uv_convex_hull(uv_pt_t *pts, int n, uv_pt_t *out) {
	n = uv_hull_prefilter(pts, n);
	qsort(pts, n, sizeof(uv_pt_t), uv_pt_cmp);
	int k = 0;
	for (int i = 0; i < n; i++) {
		while (k >= 2 && uv_cross(out[k - 2], out[k - 1], pts[i]) <= 0.0f) {
			k--;
		}
		out[k++] = pts[i];
	}
	int lower = k + 1;
	for (int i = n - 2; i >= 0; i--) {
		while (k >= lower && uv_cross(out[k - 2], out[k - 1], pts[i]) <= 0.0f) {
			k--;
		}
		out[k++] = pts[i];
	}
	return k - 1; // Last point repeats the first
}

// Find rotation (cos, sin) that aligns the hull's minimal bounding rectangle with the axes.
// The optimal rectangle has an edge collinear with a hull edge, so sweep hull edges.
// Minimizes rectangle area, or the larger side when by_max_dim is set (best for a lone chart,
// where the fit scale is limited by the larger dimension).
static void uv_min_rect_dir(const uv_pt_t *hull, int hn, bool by_max_dim, float *out_cx, float *out_cy) {
	float best_area = FLT_MAX;
	int   step      = hn > 360 ? hn / 360 : 1;
	for (int i = -1; i < hn; i += step) {
		float dx, dy;
		if (i == -1) {
			// Identity orientation as baseline candidate
			dx = 1.0f;
			dy = 0.0f;
		}
		else {
			uv_pt_t a = hull[i];
			uv_pt_t b = hull[(i + 1) % hn];
			dx        = b.u - a.u;
			dy        = b.v - a.v;
			float len = sqrtf(dx * dx + dy * dy);
			if (len < 1e-12f) {
				continue;
			}
			dx /= len;
			dy /= len;
		}
		float min_d = FLT_MAX;
		float max_d = -FLT_MAX;
		float min_p = FLT_MAX;
		float max_p = -FLT_MAX;
		for (int j = 0; j < hn; j++) {
			float d = hull[j].u * dx + hull[j].v * dy;
			float p = hull[j].v * dx - hull[j].u * dy;
			if (d < min_d)
				min_d = d;
			if (d > max_d)
				max_d = d;
			if (p < min_p)
				min_p = p;
			if (p > max_p)
				max_p = p;
		}
		float side_d = max_d - min_d;
		float side_p = max_p - min_p;
		float area   = by_max_dim ? (side_d > side_p ? side_d : side_p) : side_d * side_p;
		if (area < best_area) {
			best_area = area;
			*out_cx   = dx;
			*out_cy   = dy;
		}
	}
}

// Find the lowest (then leftmost) skyline position where a cw x ch rectangle fits in [0,1]
static bool uv_sky_find(const float *sky_x, const float *sky_y, int sky_len, float cw, float ch, float *out_x, float *out_y) {
	bool found = false;
	for (int j = 0; j < sky_len; j++) {
		float x0    = sky_x[j];
		float x_end = x0 + cw;
		if (x_end > 1.0f + UV_PACK_EPS) {
			break; // Segments are sorted by x, later ones only extend further right
		}
		// Find max y across skyline segments this rectangle spans
		float max_y = 0.0f;
		for (int k = j; k < sky_len; k++) {
			if (sky_x[k] >= x_end - UV_PACK_EPS) {
				break;
			}
			if (sky_y[k] > max_y) {
				max_y = sky_y[k];
			}
		}
		if (max_y + ch <= 1.0f + UV_PACK_EPS && (!found || max_y < *out_y)) {
			*out_x = x0;
			*out_y = max_y;
			found  = true;
		}
	}
	return found;
}

// Replace the skyline over [x0, x1) with height y; returns new segment count
static int uv_sky_insert(float *sky_x, float *sky_y, int sky_len, float *tmp_x, float *tmp_y, float x0, float x1, float y) {
	int  tmp_len  = 0;
	bool inserted = false;
	for (int k = 0; k < sky_len; k++) {
		float seg_x0 = sky_x[k];
		float seg_x1 = (k + 1 < sky_len) ? sky_x[k + 1] : 1.0f;
		float seg_y  = sky_y[k];
		if (seg_x1 <= x0 + UV_PACK_EPS || seg_x0 >= x1 - UV_PACK_EPS) {
			// Segment fully outside the new rectangle
			tmp_x[tmp_len] = seg_x0;
			tmp_y[tmp_len] = seg_y;
			tmp_len++;
		}
		else {
			if (seg_x0 < x0 - UV_PACK_EPS) {
				tmp_x[tmp_len] = seg_x0;
				tmp_y[tmp_len] = seg_y;
				tmp_len++;
			}
			if (!inserted) {
				tmp_x[tmp_len] = x0;
				tmp_y[tmp_len] = y;
				tmp_len++;
				inserted = true;
			}
			if (seg_x1 > x1 + UV_PACK_EPS) {
				tmp_x[tmp_len] = x1;
				tmp_y[tmp_len] = seg_y;
				tmp_len++;
			}
		}
	}
	// Merge adjacent segments at the same height
	sky_len = 0;
	for (int k = 0; k < tmp_len; k++) {
		if (sky_len > 0 && fabsf(tmp_y[k] - sky_y[sky_len - 1]) < UV_PACK_EPS) {
			continue;
		}
		sky_x[sky_len] = tmp_x[k];
		sky_y[sky_len] = tmp_y[k];
		sky_len++;
	}
	return sky_len;
}

// Pack all charts at the given scale; tries both orientations per chart and keeps
// the lower placement. Returns false if any chart does not fit in [0,1].
static bool uv_pack_run(int chart_count, const int *order, const float *chart_w, const float *chart_h, float scale, float margin, float *sky_x, float *sky_y,
                        float *tmp_x, float *tmp_y, float *off_u, float *off_v, bool *rotated) {
	int sky_len = 1;
	sky_x[0]    = 0.0f;
	sky_y[0]    = 0.0f;

	for (int i = 0; i < chart_count; i++) {
		int   c  = order[i];
		float cw = chart_w[c] * scale + margin;
		float ch = chart_h[c] * scale + margin;

		float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
		bool  fit0 = uv_sky_find(sky_x, sky_y, sky_len, cw, ch, &x0, &y0);
		bool  fit1 = cw != ch && uv_sky_find(sky_x, sky_y, sky_len, ch, cw, &x1, &y1);
		if (!fit0 && !fit1) {
			return false;
		}

		bool  rot = fit1 && (!fit0 || y1 < y0 || (y1 == y0 && x1 < x0));
		float px  = rot ? x1 : x0;
		float py  = rot ? y1 : y0;
		float pw  = rot ? ch : cw;
		float ph  = rot ? cw : ch;

		off_u[c]   = px + margin * 0.5f;
		off_v[c]   = py + margin * 0.5f;
		rotated[c] = rot;

		sky_len = uv_sky_insert(sky_x, sky_y, sky_len, tmp_x, tmp_y, px, px + pw, py + ph);
	}
	return true;
}

void proc_uv_unwrap(raw_mesh_t *mesh) {
	double t = iron_time();

	// Decode input mesh data
	int    vertex_count = mesh->posa->length / 4;
	float *pa           = (float *)malloc(sizeof(float) * vertex_count * 3);
	float  inv          = 1.0f / 32767.0f;

	for (int i = 0; i < vertex_count; i++) {
		pa[i * 3]     = mesh->posa->buffer[i * 4] * inv;
		pa[i * 3 + 1] = mesh->posa->buffer[i * 4 + 1] * inv;
		pa[i * 3 + 2] = mesh->posa->buffer[i * 4 + 2] * inv;
	}

	int       index_count = mesh->inda->length;
	uint32_t *indices     = mesh->inda->buffer;
	int       face_count  = index_count / 3;

	// Compute face normals
	float *fnormals = (float *)malloc(sizeof(float) * face_count * 3);
	for (int f = 0; f < face_count; f++) {
		int   i0  = indices[f * 3];
		int   i1  = indices[f * 3 + 1];
		int   i2  = indices[f * 3 + 2];
		float e1x = pa[i1 * 3] - pa[i0 * 3];
		float e1y = pa[i1 * 3 + 1] - pa[i0 * 3 + 1];
		float e1z = pa[i1 * 3 + 2] - pa[i0 * 3 + 2];
		float e2x = pa[i2 * 3] - pa[i0 * 3];
		float e2y = pa[i2 * 3 + 1] - pa[i0 * 3 + 1];
		float e2z = pa[i2 * 3 + 2] - pa[i0 * 3 + 2];
		float nx  = e1y * e2z - e1z * e2y;
		float ny  = e1z * e2x - e1x * e2z;
		float nz  = e1x * e2y - e1y * e2x;
		float len = sqrtf(nx * nx + ny * ny + nz * nz);
		if (len > 1e-10f) {
			nx /= len;
			ny /= len;
			nz /= len;
		}
		fnormals[f * 3]     = nx;
		fnormals[f * 3 + 1] = ny;
		fnormals[f * 3 + 2] = nz;
	}

	// Build canonical vertex ID map (deduplicate positions)
	int             pos_cap     = vertex_count * 2 + 1;
	uv_pos_entry_t *pos_map     = (uv_pos_entry_t *)calloc(pos_cap, sizeof(uv_pos_entry_t));
	int            *vert_canon  = (int *)malloc(sizeof(int) * vertex_count);
	int             canon_count = 0;

	for (int i = 0; i < vertex_count; i++) {
		int16_t  x = mesh->posa->buffer[i * 4];
		int16_t  y = mesh->posa->buffer[i * 4 + 1];
		int16_t  z = mesh->posa->buffer[i * 4 + 2];
		uint32_t h = uv_hash_pos(x, y, z) % pos_cap;
		while (true) {
			if (!pos_map[h].occupied) {
				pos_map[h].x        = x;
				pos_map[h].y        = y;
				pos_map[h].z        = z;
				pos_map[h].id       = canon_count;
				pos_map[h].occupied = true;
				vert_canon[i]       = canon_count++;
				break;
			}
			if (pos_map[h].x == x && pos_map[h].y == y && pos_map[h].z == z) {
				vert_canon[i] = pos_map[h].id;
				break;
			}
			h = (h + 1) % pos_cap;
		}
	}
	free(pos_map);

	// Build face adjacency via shared edges
	int              edge_cap = face_count * 6 + 1;
	uv_edge_entry_t *edge_map = (uv_edge_entry_t *)calloc(edge_cap, sizeof(uv_edge_entry_t));
	int             *face_adj = (int *)malloc(sizeof(int) * face_count * 3);
	for (int i = 0; i < face_count * 3; i++) {
		face_adj[i] = -1;
	}

	for (int f = 0; f < face_count; f++) {
		for (int e = 0; e < 3; e++) {
			int c0 = vert_canon[indices[f * 3 + e]];
			int c1 = vert_canon[indices[f * 3 + (e + 1) % 3]];
			int ea = c0 < c1 ? c0 : c1;
			int eb = c0 < c1 ? c1 : c0;

			uint32_t h = uv_hash_edge(ea, eb) % edge_cap;
			while (true) {
				if (!edge_map[h].occupied) {
					edge_map[h].v0       = ea;
					edge_map[h].v1       = eb;
					edge_map[h].face0    = f;
					edge_map[h].face1    = -1;
					edge_map[h].occupied = true;
					break;
				}
				if (edge_map[h].v0 == ea && edge_map[h].v1 == eb) {
					if (edge_map[h].face1 == -1) {
						edge_map[h].face1 = f;
						int f0            = edge_map[h].face0;
						for (int s = 0; s < 3; s++) {
							if (face_adj[f0 * 3 + s] == -1) {
								face_adj[f0 * 3 + s] = f;
								break;
							}
						}
						for (int s = 0; s < 3; s++) {
							if (face_adj[f * 3 + s] == -1) {
								face_adj[f * 3 + s] = f0;
								break;
							}
						}
					}
					break;
				}
				h = (h + 1) % edge_cap;
			}
		}
	}
	free(edge_map);
	free(vert_canon);

	// Flood-fill faces into charts based on normal angle threshold
	int *chart_id    = (int *)malloc(sizeof(int) * face_count);
	int  chart_count = 0;
	int *stack       = (int *)malloc(sizeof(int) * face_count);
	memset(chart_id, -1, sizeof(int) * face_count);

	for (int f = 0; f < face_count; f++) {
		if (chart_id[f] != -1) {
			continue;
		}
		int   cid     = chart_count++;
		float seed_nx = fnormals[f * 3];
		float seed_ny = fnormals[f * 3 + 1];
		float seed_nz = fnormals[f * 3 + 2];
		chart_id[f]   = cid;
		int sp        = 0;
		stack[sp++]   = f;

		while (sp > 0) {
			int cf = stack[--sp];
			for (int s = 0; s < 3; s++) {
				int nf = face_adj[cf * 3 + s];
				if (nf == -1 || chart_id[nf] != -1) {
					continue;
				}
				// Compare against seed normal to prevent chart drift
				float dot = seed_nx * fnormals[nf * 3] + seed_ny * fnormals[nf * 3 + 1] + seed_nz * fnormals[nf * 3 + 2];
				if (dot >= UV_ANGLE_THRESHOLD) {
					chart_id[nf] = cid;
					stack[sp++]  = nf;
				}
			}
		}
	}
	free(stack);
	free(face_adj);

	// Compute average normal per chart
	float *chart_nx = (float *)calloc(chart_count, sizeof(float));
	float *chart_ny = (float *)calloc(chart_count, sizeof(float));
	float *chart_nz = (float *)calloc(chart_count, sizeof(float));

	for (int f = 0; f < face_count; f++) {
		int c = chart_id[f];
		chart_nx[c] += fnormals[f * 3];
		chart_ny[c] += fnormals[f * 3 + 1];
		chart_nz[c] += fnormals[f * 3 + 2];
	}
	free(fnormals);

	for (int c = 0; c < chart_count; c++) {
		float len = sqrtf(chart_nx[c] * chart_nx[c] + chart_ny[c] * chart_ny[c] + chart_nz[c] * chart_nz[c]);
		if (len > 1e-10f) {
			chart_nx[c] /= len;
			chart_ny[c] /= len;
			chart_nz[c] /= len;
		}
	}

	// Compute projection axes (U, V) per chart from average normal
	float *chart_ux = (float *)malloc(sizeof(float) * chart_count);
	float *chart_uy = (float *)malloc(sizeof(float) * chart_count);
	float *chart_uz = (float *)malloc(sizeof(float) * chart_count);
	float *chart_vx = (float *)malloc(sizeof(float) * chart_count);
	float *chart_vy = (float *)malloc(sizeof(float) * chart_count);
	float *chart_vz = (float *)malloc(sizeof(float) * chart_count);

	for (int c = 0; c < chart_count; c++) {
		float nx = chart_nx[c];
		float ny = chart_ny[c];
		float nz = chart_nz[c];

		// Reference vector not parallel to normal
		float rx, ry, rz;
		if (fabsf(ny) < 0.9f) {
			rx = 0.0f;
			ry = 1.0f;
			rz = 0.0f;
		}
		else {
			rx = 1.0f;
			ry = 0.0f;
			rz = 0.0f;
		}

		// U = normalize(cross(N, ref))
		float ux = ny * rz - nz * ry;
		float uy = nz * rx - nx * rz;
		float uz = nx * ry - ny * rx;
		float ul = sqrtf(ux * ux + uy * uy + uz * uz);
		if (ul > 1e-10f) {
			ux /= ul;
			uy /= ul;
			uz /= ul;
		}

		// V = cross(N, U)
		chart_ux[c] = ux;
		chart_uy[c] = uy;
		chart_uz[c] = uz;
		chart_vx[c] = ny * uz - nz * uy;
		chart_vy[c] = nz * ux - nx * uz;
		chart_vz[c] = nx * uy - ny * ux;
	}
	free(chart_nx);
	free(chart_ny);
	free(chart_nz);

	// Project vertices onto chart planes
	float *uv_out = (float *)malloc(sizeof(float) * index_count * 2);
	for (int f = 0; f < face_count; f++) {
		int c = chart_id[f];
		for (int k = 0; k < 3; k++) {
			int   vi        = indices[f * 3 + k];
			float px        = pa[vi * 3];
			float py        = pa[vi * 3 + 1];
			float pz        = pa[vi * 3 + 2];
			int   idx       = (f * 3 + k) * 2;
			uv_out[idx]     = px * chart_ux[c] + py * chart_uy[c] + pz * chart_uz[c];
			uv_out[idx + 1] = px * chart_vx[c] + py * chart_vy[c] + pz * chart_vz[c];
		}
	}
	free(chart_ux);
	free(chart_uy);
	free(chart_uz);
	free(chart_vx);
	free(chart_vy);
	free(chart_vz);

	// Equalize texel density: scale each chart so its UV area matches its 3D surface
	// area, compensating the shrink from planar projection of curved charts
	float *chart_area3d = (float *)calloc(chart_count, sizeof(float));
	float *chart_areauv = (float *)calloc(chart_count, sizeof(float));
	for (int f = 0; f < face_count; f++) {
		int   i0  = indices[f * 3];
		int   i1  = indices[f * 3 + 1];
		int   i2  = indices[f * 3 + 2];
		float e1x = pa[i1 * 3] - pa[i0 * 3];
		float e1y = pa[i1 * 3 + 1] - pa[i0 * 3 + 1];
		float e1z = pa[i1 * 3 + 2] - pa[i0 * 3 + 2];
		float e2x = pa[i2 * 3] - pa[i0 * 3];
		float e2y = pa[i2 * 3 + 1] - pa[i0 * 3 + 1];
		float e2z = pa[i2 * 3 + 2] - pa[i0 * 3 + 2];
		float cx  = e1y * e2z - e1z * e2y;
		float cy  = e1z * e2x - e1x * e2z;
		float cz  = e1x * e2y - e1y * e2x;
		chart_area3d[chart_id[f]] += 0.5f * sqrtf(cx * cx + cy * cy + cz * cz);

		int   b   = f * 3 * 2;
		float u0  = uv_out[b];
		float v0  = uv_out[b + 1];
		float du1 = uv_out[b + 2] - u0;
		float dv1 = uv_out[b + 3] - v0;
		float du2 = uv_out[b + 4] - u0;
		float dv2 = uv_out[b + 5] - v0;
		chart_areauv[chart_id[f]] += 0.5f * fabsf(du1 * dv2 - du2 * dv1);
	}
	for (int c = 0; c < chart_count; c++) {
		float s = chart_areauv[c] > 1e-12f ? sqrtf(chart_area3d[c] / chart_areauv[c]) : 1.0f;
		// Projection only shrinks, so s >= 1 up to noise; cap pathological slivers
		if (s < 0.5f)
			s = 0.5f;
		if (s > 4.0f)
			s = 4.0f;
		chart_area3d[c] = s; // Reuse as per-chart scale
	}
	for (int f = 0; f < face_count; f++) {
		float s = chart_area3d[chart_id[f]];
		for (int k = 0; k < 3; k++) {
			int idx = (f * 3 + k) * 2;
			uv_out[idx] *= s;
			uv_out[idx + 1] *= s;
		}
	}
	free(chart_area3d);
	free(chart_areauv);

	// Group face corners by chart
	int *c_start = (int *)calloc(chart_count + 1, sizeof(int));
	for (int f = 0; f < face_count; f++) {
		c_start[chart_id[f] + 1] += 3;
	}
	int max_corners = 0;
	for (int c = 0; c < chart_count; c++) {
		if (c_start[c + 1] > max_corners) {
			max_corners = c_start[c + 1];
		}
		c_start[c + 1] += c_start[c];
	}
	int *c_corner = (int *)malloc(sizeof(int) * index_count);
	int *c_fill   = (int *)malloc(sizeof(int) * chart_count);
	memcpy(c_fill, c_start, sizeof(int) * chart_count);
	for (int f = 0; f < face_count; f++) {
		int c = chart_id[f];
		for (int k = 0; k < 3; k++) {
			c_corner[c_fill[c]++] = f * 3 + k;
		}
	}
	free(c_fill);

	// Rotate each chart to its minimal-area bounding rectangle, normalize to origin
	// and compute chart sizes and total area for scaling
	uv_pt_t *pts        = (uv_pt_t *)malloc(sizeof(uv_pt_t) * max_corners);
	uv_pt_t *hull       = (uv_pt_t *)malloc(sizeof(uv_pt_t) * (max_corners * 2 + 1));
	float   *chart_w    = (float *)malloc(sizeof(float) * chart_count);
	float   *chart_h    = (float *)malloc(sizeof(float) * chart_count);
	float    total_area = 0.0f;

	for (int c = 0; c < chart_count; c++) {
		int   m  = c_start[c + 1] - c_start[c];
		float cx = 1.0f;
		float cy = 0.0f;
		if (m >= 3) {
			for (int i = 0; i < m; i++) {
				int idx  = c_corner[c_start[c] + i] * 2;
				pts[i].u = uv_out[idx];
				pts[i].v = uv_out[idx + 1];
			}
			int hn = uv_convex_hull(pts, m, hull);
			if (hn >= 3) {
				uv_min_rect_dir(hull, hn, chart_count == 1, &cx, &cy);
			}
		}

		float min_u = FLT_MAX;
		float min_v = FLT_MAX;
		float max_u = -FLT_MAX;
		float max_v = -FLT_MAX;
		for (int i = 0; i < m; i++) {
			int   idx       = c_corner[c_start[c] + i] * 2;
			float u         = uv_out[idx];
			float v         = uv_out[idx + 1];
			float ru        = u * cx + v * cy;
			float rv        = v * cx - u * cy;
			uv_out[idx]     = ru;
			uv_out[idx + 1] = rv;
			if (ru < min_u)
				min_u = ru;
			if (rv < min_v)
				min_v = rv;
			if (ru > max_u)
				max_u = ru;
			if (rv > max_v)
				max_v = rv;
		}
		for (int i = 0; i < m; i++) {
			int idx = c_corner[c_start[c] + i] * 2;
			uv_out[idx] -= min_u;
			uv_out[idx + 1] -= min_v;
		}

		chart_w[c] = max_u - min_u;
		chart_h[c] = max_v - min_v;
		if (chart_w[c] < 1e-10f)
			chart_w[c] = 1e-6f;
		if (chart_h[c] < 1e-10f)
			chart_h[c] = 1e-6f;
		total_area += chart_w[c] * chart_h[c];
	}
	free(pts);
	free(hull);
	free(c_corner);
	free(c_start);

	// Sort charts by largest dimension (descending) for packing
	uv_sort_t *chart_sort = (uv_sort_t *)malloc(sizeof(uv_sort_t) * chart_count);
	for (int c = 0; c < chart_count; c++) {
		chart_sort[c].key = chart_w[c] > chart_h[c] ? chart_w[c] : chart_h[c];
		chart_sort[c].id  = c;
	}
	qsort(chart_sort, chart_count, sizeof(uv_sort_t), uv_sort_cmp);
	int *chart_order = (int *)malloc(sizeof(int) * chart_count);
	for (int i = 0; i < chart_count; i++) {
		chart_order[i] = chart_sort[i].id;
	}
	free(chart_sort);

	// Skyline packing with binary search for optimal scale
	float *chart_off_u   = (float *)calloc(chart_count, sizeof(float));
	float *chart_off_v   = (float *)calloc(chart_count, sizeof(float));
	bool  *chart_rotated = (bool *)calloc(chart_count, sizeof(bool));

	// Skyline: array of (x, y) pairs representing the top edge of placed islands
	int    sky_cap = chart_count * 2 + 4;
	float *sky_x   = (float *)malloc(sizeof(float) * sky_cap);
	float *sky_y   = (float *)malloc(sizeof(float) * sky_cap);
	float *tmp_x   = (float *)malloc(sizeof(float) * sky_cap);
	float *tmp_y   = (float *)malloc(sizeof(float) * sky_cap);

	float margin   = UV_PACK_MARGIN;
	float scale_lo = 0.0f;
	float scale_hi = total_area > 1e-10f ? (2.0f / sqrtf(total_area)) : 2.0f;
	float scale    = 0.0f;

	// Binary search: find largest scale where all islands fit in [0,1]
	for (int iter = 0; iter < 24; iter++) {
		float try_scale = (scale_lo + scale_hi) * 0.5f;
		if (uv_pack_run(chart_count, chart_order, chart_w, chart_h, try_scale, margin, sky_x, sky_y, tmp_x, tmp_y, chart_off_u, chart_off_v, chart_rotated)) {
			scale    = try_scale;
			scale_lo = try_scale;
		}
		else {
			scale_hi = try_scale;
		}
	}

	// Final pass with best scale to get definitive offsets
	if (scale > 0.0f) {
		uv_pack_run(chart_count, chart_order, chart_w, chart_h, scale, margin, sky_x, sky_y, tmp_x, tmp_y, chart_off_u, chart_off_v, chart_rotated);
	}
	free(sky_x);
	free(sky_y);
	free(tmp_x);
	free(tmp_y);
	free(chart_order);

	// Apply packing offsets, scale, and rotation to all UVs
	for (int f = 0; f < face_count; f++) {
		int c = chart_id[f];
		for (int k = 0; k < 3; k++) {
			int   idx = (f * 3 + k) * 2;
			float u   = uv_out[idx];
			float v   = uv_out[idx + 1];
			if (chart_rotated[c]) {
				// Rotate 90 degrees: (u, v) -> (v, w - u) where w = chart_w[c]
				float ru = v;
				float rv = chart_w[c] - u;
				u        = ru;
				v        = rv;
			}
			uv_out[idx]     = u * scale + chart_off_u[c];
			uv_out[idx + 1] = v * scale + chart_off_v[c];
		}
	}
	free(chart_off_u);
	free(chart_off_v);
	free(chart_rotated);
	free(chart_w);
	free(chart_h);

	// Build output arrays (per-face-corner vertices)
	int       out_v_count = index_count;
	int16_t  *pa_out      = (int16_t *)malloc(sizeof(int16_t) * out_v_count * 4);
	int16_t  *na_out      = (int16_t *)malloc(sizeof(int16_t) * out_v_count * 2);
	int16_t  *ta_out      = (int16_t *)malloc(sizeof(int16_t) * out_v_count * 2);
	uint32_t *ia_out      = (uint32_t *)malloc(sizeof(uint32_t) * out_v_count);

	for (int i = 0; i < out_v_count; i++) {
		int vi            = indices[i];
		pa_out[i * 4]     = mesh->posa->buffer[vi * 4];
		pa_out[i * 4 + 1] = mesh->posa->buffer[vi * 4 + 1];
		pa_out[i * 4 + 2] = mesh->posa->buffer[vi * 4 + 2];
		pa_out[i * 4 + 3] = mesh->posa->buffer[vi * 4 + 3];
		na_out[i * 2]     = mesh->nora->buffer[vi * 2];
		na_out[i * 2 + 1] = mesh->nora->buffer[vi * 2 + 1];

		float u = uv_out[i * 2];
		float v = uv_out[i * 2 + 1];
		if (u < 0.0f)
			u = 0.0f;
		if (u > 1.0f)
			u = 1.0f;
		if (v < 0.0f)
			v = 0.0f;
		if (v > 1.0f)
			v = 1.0f;
		ta_out[i * 2]     = (int16_t)((1.0f - u) * 32767.0f);
		ta_out[i * 2 + 1] = (int16_t)(v * 32767.0f);

		ia_out[i] = i;
	}

	free(uv_out);
	free(chart_id);
	free(pa);

	// Replace mesh data
	free(mesh->posa->buffer);
	free(mesh->nora->buffer);
	free(mesh->inda->buffer);

	mesh->posa->buffer   = pa_out;
	mesh->posa->length   = out_v_count * 4;
	mesh->posa->capacity = out_v_count * 4;
	mesh->nora->buffer   = na_out;
	mesh->nora->length   = out_v_count * 2;
	mesh->nora->capacity = out_v_count * 2;

	if (mesh->texa == NULL) {
		mesh->texa = (i16_array_t *)calloc(1, sizeof(i16_array_t));
	}
	else {
		free(mesh->texa->buffer);
	}
	mesh->texa->buffer   = ta_out;
	mesh->texa->length   = out_v_count * 2;
	mesh->texa->capacity = out_v_count * 2;

	mesh->inda->buffer   = ia_out;
	mesh->inda->length   = out_v_count;
	mesh->inda->capacity = out_v_count;
	mesh->vertex_count   = out_v_count;
	mesh->index_count    = out_v_count;

	iron_log("Unwrapped in %fs\n", iron_time() - t);
}
