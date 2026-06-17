/*
 * iris_upscale.c - RealESRGAN_x4plus image upscaler (RRDBNet)
 *
 * Pure-C, CPU-only implementation of the RRDBNet super-resolution network
 * used by RealESRGAN_x4plus. The architecture (from BasicSR) is:
 *
 *   feat      = conv_first(x)                     # 3 -> 64
 *   body_feat = conv_body(body(feat))             # 23x RRDB, then 64 -> 64
 *   feat      = feat + body_feat
 *   feat      = lrelu(conv_up1(nearest_2x(feat))) # upsample x2
 *   feat      = lrelu(conv_up2(nearest_2x(feat))) # upsample x2 (total x4)
 *   out       = conv_last(lrelu(conv_hr(feat)))   # 64 -> 64 -> 3
 *
 * Each RRDB is three ResidualDenseBlocks with a 0.2 residual scale; each
 * ResidualDenseBlock is five 3x3 convs over densely concatenated features
 * (LeakyReLU 0.2 between them), again with a 0.2 residual scale.
 *
 * All convolutions are 3x3, stride 1, zero-padded. Weights are read directly
 * from the memory-mapped safetensors file (all tensors are F32), so there is
 * no separate weight copy.
 */

#include "iris_upscale.h"
#include "iris_safetensors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_VULKAN
#include "iris_vulkan.h"
#endif

/* RRDBNet hyperparameters for RealESRGAN_x4plus */
#define RG_NUM_FEAT    64
#define RG_NUM_GROW_CH 32
#define RG_NUM_BLOCK   23
#define RG_SCALE       4
#define RG_LRELU_SLOPE 0.2f
#define RG_RES_SCALE   0.2f

/* Densely-concatenated width inside a ResidualDenseBlock:
 * num_feat + 4*num_grow_ch = 64 + 128 = 192 channels. */
#define RG_CAT_CH (RG_NUM_FEAT + 4 * RG_NUM_GROW_CH)

struct iris_upscale {
	safetensors_file_t *sf;
};

/* ========================================================================
 * Weight access (zero-copy; all tensors are F32 in the mmap'd file)
 * ======================================================================== */

static const float *rg_tensor(iris_upscale_t *m, const char *name) {
	const safetensor_t *t = safetensors_find(m->sf, name);
	if (!t) {
		fprintf(stderr, "RealESRGAN: missing tensor '%s'\n", name);
		return NULL;
	}
	if (t->dtype != DTYPE_F32) {
		fprintf(stderr, "RealESRGAN: tensor '%s' is not F32\n", name);
		return NULL;
	}
	return (const float *)safetensors_data(m->sf, t);
}

/* ========================================================================
 * Primitive ops (CPU)
 * ======================================================================== */

/* 3x3 zero-padded, stride-1 convolution.
 *   in:     [Cin, H, W]
 *   weight: [Cout, Cin, 3, 3]
 *   bias:   [Cout]
 *   out:    [Cout, H, W]  (must not alias in)
 * The inner loop runs contiguously over x for good auto-vectorization; the
 * valid x-range per kernel tap is precomputed so there is no per-pixel branch.
 */
static void conv2d_3x3(const float *in, int Cin, int H, int W, const float *weight, const float *bias, int Cout, float *out) {
	const size_t plane = (size_t)H * W;

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
	for (int oc = 0; oc < Cout; oc++) {
		float *op = out + (size_t)oc * plane;
		float  b  = bias[oc];
		for (size_t i = 0; i < plane; i++)
			op[i] = b;

		for (int ic = 0; ic < Cin; ic++) {
			const float *ip = in + (size_t)ic * plane;
			const float *wk = weight + ((size_t)oc * Cin + ic) * 9;
			for (int ky = 0; ky < 3; ky++) {
				int dy = ky - 1;
				for (int kx = 0; kx < 3; kx++) {
					float wv = wk[ky * 3 + kx];
					if (wv == 0.0f)
						continue;
					int dx = kx - 1;
					int x0 = (dx < 0) ? 1 : 0;
					int x1 = (dx > 0) ? W - 1 : W;
					for (int y = 0; y < H; y++) {
						int sy = y + dy;
						if (sy < 0 || sy >= H)
							continue;
						float       *orow = op + (size_t)y * W;
						const float *irow = ip + (size_t)sy * W + dx;
						for (int x = x0; x < x1; x++)
							orow[x] += wv * irow[x];
					}
				}
			}
		}
	}
}

/* In-place LeakyReLU over a [C, H, W] buffer. */
static void leaky_relu(float *buf, size_t n) {
	for (size_t i = 0; i < n; i++)
		if (buf[i] < 0.0f)
			buf[i] *= RG_LRELU_SLOPE;
}

/* Nearest-neighbour 2x upsample: in [C,H,W] -> out [C,2H,2W]. */
static void upsample_nearest_2x(const float *in, int C, int H, int W, float *out) {
	int          OH = H * 2, OW = W * 2;
	const size_t in_plane  = (size_t)H * W;
	const size_t out_plane = (size_t)OH * OW;
	for (int c = 0; c < C; c++) {
		const float *ip = in + (size_t)c * in_plane;
		float       *op = out + (size_t)c * out_plane;
		for (int y = 0; y < OH; y++) {
			const float *irow = ip + (size_t)(y / 2) * W;
			float       *orow = op + (size_t)y * OW;
			for (int x = 0; x < OW; x++)
				orow[x] = irow[x / 2];
		}
	}
}

/* ========================================================================
 * RRDBNet blocks
 * ======================================================================== */

/* ResidualDenseBlock: in/out are [num_feat, H, W]; out may equal in only if
 * caller is fine with that (here we always use distinct buffers).
 * `prefix` is e.g. "body.0.rdb1". `scratch_cat` is [RG_CAT_CH, H, W] and
 * `scratch_x5` is [num_feat, H, W]; both are caller-owned work buffers. */
static int residual_dense_block(iris_upscale_t *m, const char *prefix, const float *in, int H, int W, float *scratch_cat, float *scratch_x5, float *out) {
	const size_t plane = (size_t)H * W;
	char         name[256];

	/* Concatenation buffer starts with the input features (channels 0..63). */
	memcpy(scratch_cat, in, RG_NUM_FEAT * plane * sizeof(float));

	/* conv1..conv4 each produce num_grow_ch channels appended to the cat
	 * buffer; conv reads cat[0:cur] and writes cat[cur:cur+grow] (disjoint). */
	int cur = RG_NUM_FEAT;
	for (int i = 1; i <= 4; i++) {
		snprintf(name, sizeof(name), "%s.conv%d.weight", prefix, i);
		const float *w = rg_tensor(m, name);
		snprintf(name, sizeof(name), "%s.conv%d.bias", prefix, i);
		const float *b = rg_tensor(m, name);
		if (!w || !b)
			return -1;

		float *dst = scratch_cat + (size_t)cur * plane;
		conv2d_3x3(scratch_cat, cur, H, W, w, b, RG_NUM_GROW_CH, dst);
		leaky_relu(dst, (size_t)RG_NUM_GROW_CH * plane);
		cur += RG_NUM_GROW_CH;
	}

	/* conv5: cat[0:192] -> num_feat channels (no activation). */
	snprintf(name, sizeof(name), "%s.conv5.weight", prefix);
	const float *w5 = rg_tensor(m, name);
	snprintf(name, sizeof(name), "%s.conv5.bias", prefix);
	const float *b5 = rg_tensor(m, name);
	if (!w5 || !b5)
		return -1;
	conv2d_3x3(scratch_cat, cur, H, W, w5, b5, RG_NUM_FEAT, scratch_x5);

	/* out = x5 * 0.2 + in */
	size_t n = (size_t)RG_NUM_FEAT * plane;
	for (size_t i = 0; i < n; i++)
		out[i] = scratch_x5[i] * RG_RES_SCALE + in[i];
	return 0;
}

/* RRDB: three ResidualDenseBlocks then a 0.2 residual.
 * `tmp_a`/`tmp_b` are [num_feat,H,W] ping-pong buffers. */
static int rrdb(iris_upscale_t *m, int block_idx, const float *in, int H, int W, float *scratch_cat, float *scratch_x5, float *tmp_a, float *tmp_b,
                float *out) {
	char         prefix[64];
	const size_t plane = (size_t)H * W;

	snprintf(prefix, sizeof(prefix), "body.%d.rdb1", block_idx);
	if (residual_dense_block(m, prefix, in, H, W, scratch_cat, scratch_x5, tmp_a) != 0)
		return -1;
	snprintf(prefix, sizeof(prefix), "body.%d.rdb2", block_idx);
	if (residual_dense_block(m, prefix, tmp_a, H, W, scratch_cat, scratch_x5, tmp_b) != 0)
		return -1;
	snprintf(prefix, sizeof(prefix), "body.%d.rdb3", block_idx);
	if (residual_dense_block(m, prefix, tmp_b, H, W, scratch_cat, scratch_x5, tmp_a) != 0)
		return -1;

	/* out = rdb_out * 0.2 + in */
	size_t n = (size_t)RG_NUM_FEAT * plane;
	for (size_t i = 0; i < n; i++)
		out[i] = tmp_a[i] * RG_RES_SCALE + in[i];
	return 0;
}

/* ========================================================================
 * Input / output conversion (shared by CPU and GPU paths)
 * ======================================================================== */

/* RGB(A) uint8 image -> planar CHW float in [0,1], 3 channels.
 * Returns a malloc'd [3*H*W] buffer (caller frees), or NULL on OOM. */
static float *rg_build_input(const iris_image *input) {
	int          H = input->height, W = input->width, ic = input->channels;
	const size_t plane = (size_t)H * W;
	float       *x_in  = malloc((size_t)3 * plane * sizeof(float));
	if (!x_in)
		return NULL;
	for (int y = 0; y < H; y++) {
		for (int xx = 0; xx < W; xx++) {
			const uint8_t *px    = input->data + ((size_t)y * W + xx) * ic;
			size_t         pi    = (size_t)y * W + xx;
			x_in[0 * plane + pi] = px[0] / 255.0f;
			x_in[1 * plane + pi] = (ic > 1 ? px[1] : px[0]) / 255.0f;
			x_in[2 * plane + pi] = (ic > 2 ? px[2] : px[0]) / 255.0f;
		}
	}
	return x_in;
}

/* Planar CHW float [3, H4, W4] in [0,1] -> RGB uint8 image. */
static iris_image *rg_chw_to_image(const float *out, int W4, int H4) {
	const size_t plane4 = (size_t)H4 * W4;
	iris_image  *result = iris_image_create(W4, H4, 3);
	if (!result)
		return NULL;
	for (size_t pi = 0; pi < plane4; pi++) {
		for (int c = 0; c < 3; c++) {
			float v = out[(size_t)c * plane4 + pi];
			if (v < 0.0f)
				v = 0.0f;
			if (v > 1.0f)
				v = 1.0f;
			result->data[pi * 3 + c] = (uint8_t)(v * 255.0f + 0.5f);
		}
	}
	return result;
}

/* ========================================================================
 * Vulkan GPU-resident forward path
 *
 * Mirrors the CPU forward, but every convolution / activation / residual runs
 * on the GPU and activations stay resident in VRAM between ops. Convolution is
 * the entire cost of RRDBNet (~350 3x3 convs), so offloading it is the win.
 * ======================================================================== */
#ifdef USE_VULKAN

/* Run a named 3x3/pad-1/stride-1 conv on the GPU. Weights are F32 in the
 * mmap'd file and cached in VRAM by pointer across the run. */
static iris_gpu_tensor_t rg_conv_gpu(iris_upscale_t *m, const char *prefix, iris_gpu_tensor_t x, int Cin, int H, int W, int Cout) {
	char name[256];
	snprintf(name, sizeof(name), "%s.weight", prefix);
	const float *w = rg_tensor(m, name);
	snprintf(name, sizeof(name), "%s.bias", prefix);
	const float *b = rg_tensor(m, name);
	if (!w || !b)
		return NULL;
	return iris_gpu_conv2d_f32(x, w, b, 1, Cin, Cout, H, W, 3, 3, 1, 1);
}

/* ResidualDenseBlock on GPU. `in` is [num_feat,H,W]; returns a new tensor. */
static iris_gpu_tensor_t rdb_gpu(iris_upscale_t *m, const char *prefix, iris_gpu_tensor_t in, int H, int W) {
	const size_t plane = (size_t)H * W;
	char         name[64];

	iris_gpu_tensor_t cat = iris_gpu_tensor_alloc((size_t)RG_CAT_CH * plane);
	if (!cat)
		return NULL;
	iris_gpu_copy_region_f32(cat, 0, in, 0, (size_t)RG_NUM_FEAT * plane);

	int cur = RG_NUM_FEAT;
	for (int i = 1; i <= 4; i++) {
		snprintf(name, sizeof(name), "%s.conv%d", prefix, i);
		iris_gpu_tensor_t gch = rg_conv_gpu(m, name, cat, cur, H, W, RG_NUM_GROW_CH);
		if (!gch) {
			iris_gpu_tensor_free(cat);
			return NULL;
		}
		iris_gpu_leaky_relu_f32(gch, gch, (int)(RG_NUM_GROW_CH * plane), RG_LRELU_SLOPE);
		iris_gpu_copy_region_f32(cat, (size_t)cur * plane, gch, 0, (size_t)RG_NUM_GROW_CH * plane);
		iris_gpu_tensor_free(gch);
		cur += RG_NUM_GROW_CH;
	}

	snprintf(name, sizeof(name), "%s.conv5", prefix);
	iris_gpu_tensor_t x5 = rg_conv_gpu(m, name, cat, cur, H, W, RG_NUM_FEAT);
	iris_gpu_tensor_free(cat);
	if (!x5)
		return NULL;

	iris_gpu_tensor_t out = iris_gpu_tensor_alloc((size_t)RG_NUM_FEAT * plane);
	if (!out) {
		iris_gpu_tensor_free(x5);
		return NULL;
	}
	/* out = x5 * 0.2 + in */
	iris_gpu_scale_add_f32(out, x5, in, RG_RES_SCALE, (int)(RG_NUM_FEAT * plane));
	iris_gpu_tensor_free(x5);
	return out;
}

/* RRDB on GPU: three RDBs then a 0.2 residual. Returns a new tensor. */
static iris_gpu_tensor_t rrdb_gpu(iris_upscale_t *m, int idx, iris_gpu_tensor_t in, int H, int W) {
	const size_t plane = (size_t)H * W;
	char         p[64];

	snprintf(p, sizeof(p), "body.%d.rdb1", idx);
	iris_gpu_tensor_t a = rdb_gpu(m, p, in, H, W);
	if (!a)
		return NULL;
	snprintf(p, sizeof(p), "body.%d.rdb2", idx);
	iris_gpu_tensor_t b = rdb_gpu(m, p, a, H, W);
	iris_gpu_tensor_free(a);
	if (!b)
		return NULL;
	snprintf(p, sizeof(p), "body.%d.rdb3", idx);
	iris_gpu_tensor_t c = rdb_gpu(m, p, b, H, W);
	iris_gpu_tensor_free(b);
	if (!c)
		return NULL;

	iris_gpu_tensor_t out = iris_gpu_tensor_alloc((size_t)RG_NUM_FEAT * plane);
	if (!out) {
		iris_gpu_tensor_free(c);
		return NULL;
	}
	iris_gpu_scale_add_f32(out, c, in, RG_RES_SCALE, (int)(RG_NUM_FEAT * plane));
	iris_gpu_tensor_free(c);
	return out;
}

static iris_image *upscale_vulkan(iris_upscale_t *m, const iris_image *input) {
	int          H = input->height, W = input->width;
	const size_t plane = (size_t)H * W;

	float *x_in = rg_build_input(input);
	if (!x_in)
		return NULL;
	iris_gpu_tensor_t x = iris_gpu_tensor_create(x_in, 3 * plane);
	free(x_in);
	if (!x)
		return NULL;

	/* feat = conv_first(x)  (3 -> 64); feat is kept for the body residual. */
	iris_gpu_tensor_t feat = rg_conv_gpu(m, "conv_first", x, 3, H, W, RG_NUM_FEAT);
	iris_gpu_tensor_free(x);
	if (!feat)
		return NULL;

	/* body: 23 RRDB blocks. `cur` is the running activation (!= feat). */
	iris_gpu_tensor_t cur = feat;
	for (int i = 0; i < RG_NUM_BLOCK; i++) {
		iris_gpu_tensor_t nxt = rrdb_gpu(m, i, cur, H, W);
		if (cur != feat)
			iris_gpu_tensor_free(cur);
		if (!nxt) {
			iris_gpu_tensor_free(feat);
			return NULL;
		}
		cur = nxt;
	}

	/* body_feat = conv_body(body(feat)); feat = feat + body_feat */
	iris_gpu_tensor_t body_feat = rg_conv_gpu(m, "conv_body", cur, RG_NUM_FEAT, H, W, RG_NUM_FEAT);
	if (cur != feat)
		iris_gpu_tensor_free(cur);
	if (!body_feat) {
		iris_gpu_tensor_free(feat);
		return NULL;
	}
	iris_gpu_add_f32(feat, feat, body_feat, (int)(RG_NUM_FEAT * plane));
	iris_gpu_tensor_free(body_feat);

	/* Upsample stage 1: nearest 2x -> lrelu(conv_up1) */
	int               H2 = H * 2, W2 = W * 2;
	iris_gpu_tensor_t up1 = iris_gpu_upsample_nearest_2x_f32(feat, RG_NUM_FEAT, H, W);
	iris_gpu_tensor_free(feat);
	if (!up1)
		return NULL;
	iris_gpu_tensor_t f2 = rg_conv_gpu(m, "conv_up1", up1, RG_NUM_FEAT, H2, W2, RG_NUM_FEAT);
	iris_gpu_tensor_free(up1);
	if (!f2)
		return NULL;
	iris_gpu_leaky_relu_f32(f2, f2, (int)(RG_NUM_FEAT * (size_t)H2 * W2), RG_LRELU_SLOPE);

	/* Upsample stage 2: nearest 2x -> lrelu(conv_up2) */
	int               H4 = H * 4, W4 = W * 4;
	iris_gpu_tensor_t up2 = iris_gpu_upsample_nearest_2x_f32(f2, RG_NUM_FEAT, H2, W2);
	iris_gpu_tensor_free(f2);
	if (!up2)
		return NULL;
	iris_gpu_tensor_t f3 = rg_conv_gpu(m, "conv_up2", up2, RG_NUM_FEAT, H4, W4, RG_NUM_FEAT);
	iris_gpu_tensor_free(up2);
	if (!f3)
		return NULL;
	const size_t plane4 = (size_t)H4 * W4;
	iris_gpu_leaky_relu_f32(f3, f3, (int)(RG_NUM_FEAT * plane4), RG_LRELU_SLOPE);

	/* conv_hr -> lrelu -> conv_last (64 -> 64 -> 3) */
	iris_gpu_tensor_t hr = rg_conv_gpu(m, "conv_hr", f3, RG_NUM_FEAT, H4, W4, RG_NUM_FEAT);
	iris_gpu_tensor_free(f3);
	if (!hr)
		return NULL;
	iris_gpu_leaky_relu_f32(hr, hr, (int)(RG_NUM_FEAT * plane4), RG_LRELU_SLOPE);

	iris_gpu_tensor_t out = rg_conv_gpu(m, "conv_last", hr, RG_NUM_FEAT, H4, W4, 3);
	iris_gpu_tensor_free(hr);
	if (!out)
		return NULL;

	float *outbuf = malloc((size_t)3 * plane4 * sizeof(float));
	if (!outbuf) {
		iris_gpu_tensor_free(out);
		return NULL;
	}
	iris_gpu_tensor_read(out, outbuf);
	iris_gpu_tensor_free(out);

	iris_image *result = rg_chw_to_image(outbuf, W4, H4);
	free(outbuf);
	return result;
}
#endif /* USE_VULKAN */

/* ========================================================================
 * Public API
 * ======================================================================== */

iris_upscale_t *iris_upscale_load(const char *path) {
	safetensors_file_t *sf = safetensors_open(path);
	if (!sf) {
		fprintf(stderr, "RealESRGAN: failed to open '%s'\n", path);
		return NULL;
	}
	/* Sanity check a couple of expected tensors. */
	if (!safetensors_find(sf, "conv_first.weight") || !safetensors_find(sf, "conv_last.weight")) {
		fprintf(stderr, "RealESRGAN: '%s' does not look like an RRDBNet model\n", path);
		safetensors_close(sf);
		return NULL;
	}
	iris_upscale_t *m = calloc(1, sizeof(iris_upscale_t));
	if (!m) {
		safetensors_close(sf);
		return NULL;
	}
	m->sf = sf;
	return m;
}

void iris_upscale_free(iris_upscale_t *m) {
	if (!m)
		return;
	safetensors_close(m->sf);
	free(m);
}

/* Helper: run a 3x3 conv by tensor prefix into `out`. Returns 0 on success. */
static int conv_by_name(iris_upscale_t *m, const char *prefix, const float *in, int Cin, int H, int W, int Cout, float *out) {
	char name[256];
	snprintf(name, sizeof(name), "%s.weight", prefix);
	const float *w = rg_tensor(m, name);
	snprintf(name, sizeof(name), "%s.bias", prefix);
	const float *b = rg_tensor(m, name);
	if (!w || !b)
		return -1;
	conv2d_3x3(in, Cin, H, W, w, b, Cout, out);
	return 0;
}

iris_image *iris_upscale_run(iris_upscale_t *m, const iris_image *input) {
	if (!m || !input || !input->data)
		return NULL;

	int          H = input->height, W = input->width;
	const size_t plane = (size_t)H * W;

#ifdef USE_VULKAN
	/* GPU-resident path: convolution dominates RRDBNet, so offload it. Falls
	 * back to the CPU path below if the GPU forward fails. */
	if (iris_vulkan_available()) {
		iris_image *r = upscale_vulkan(m, input);
		if (r)
			return r;
		fprintf(stderr, "RealESRGAN: GPU path failed, falling back to CPU\n");
	}
#endif

	/* ---- Input: RGB uint8 -> CHW float in [0,1] ---- */
	float *x_in = rg_build_input(input);
	if (!x_in)
		return NULL;

	/* ---- Work buffers ---- */
	float *feat        = malloc((size_t)RG_NUM_FEAT * plane * sizeof(float));
	float *feat_body   = malloc((size_t)RG_NUM_FEAT * plane * sizeof(float));
	float *body_a      = malloc((size_t)RG_NUM_FEAT * plane * sizeof(float));
	float *body_b      = malloc((size_t)RG_NUM_FEAT * plane * sizeof(float));
	float *scratch_cat = malloc((size_t)RG_CAT_CH * plane * sizeof(float));
	float *scratch_x5  = malloc((size_t)RG_NUM_FEAT * plane * sizeof(float));
	float *tmp_a       = malloc((size_t)RG_NUM_FEAT * plane * sizeof(float));
	float *tmp_b       = malloc((size_t)RG_NUM_FEAT * plane * sizeof(float));

	if (!feat || !feat_body || !body_a || !body_b || !scratch_cat || !scratch_x5 || !tmp_a || !tmp_b) {
		free(x_in);
		free(feat);
		free(feat_body);
		free(body_a);
		free(body_b);
		free(scratch_cat);
		free(scratch_x5);
		free(tmp_a);
		free(tmp_b);
		fprintf(stderr, "RealESRGAN: out of memory for %dx%d input\n", W, H);
		return NULL;
	}

	int ok = 1;

	/* ---- conv_first: 3 -> 64 ---- */
	if (ok && conv_by_name(m, "conv_first", x_in, 3, H, W, RG_NUM_FEAT, feat) != 0)
		ok = 0;
	free(x_in);

	/* ---- body: 23 RRDB blocks, ping-pong between body_a / body_b ---- */
	if (ok) {
		const float *cur = feat;   /* current block input */
		float       *dst = body_a; /* current block output */
		for (int i = 0; i < RG_NUM_BLOCK; i++) {
			if (rrdb(m, i, cur, H, W, scratch_cat, scratch_x5, tmp_a, tmp_b, dst) != 0) {
				ok = 0;
				break;
			}
			cur = dst;
			dst = (dst == body_a) ? body_b : body_a;
		}
		/* conv_body: 64 -> 64, reading the last written body buffer (`cur`). */
		if (ok && conv_by_name(m, "conv_body", cur, RG_NUM_FEAT, H, W, RG_NUM_FEAT, feat_body) != 0)
			ok = 0;
	}

	/* ---- feat = feat + conv_body(body(feat)) ---- */
	if (ok) {
		size_t n = (size_t)RG_NUM_FEAT * plane;
		for (size_t i = 0; i < n; i++)
			feat[i] += feat_body[i];
	}

	free(body_a);
	free(body_b);
	free(feat_body);

	if (!ok) {
		free(feat);
		free(scratch_cat);
		free(scratch_x5);
		free(tmp_a);
		free(tmp_b);
		return NULL;
	}

	/* ---- Upsample stage 1: nearest 2x then lrelu(conv_up1) ---- */
	int    H2 = H * 2, W2 = W * 2;
	size_t plane2 = (size_t)H2 * W2;
	float *up     = malloc((size_t)RG_NUM_FEAT * plane2 * sizeof(float));
	float *feat2  = malloc((size_t)RG_NUM_FEAT * plane2 * sizeof(float));
	if (!up || !feat2) {
		free(feat);
		free(scratch_cat);
		free(scratch_x5);
		free(tmp_a);
		free(tmp_b);
		free(up);
		free(feat2);
		fprintf(stderr, "RealESRGAN: out of memory (upsample 1)\n");
		return NULL;
	}
	upsample_nearest_2x(feat, RG_NUM_FEAT, H, W, up);
	free(feat);
	free(scratch_cat);
	free(scratch_x5);
	free(tmp_a);
	free(tmp_b);

	if (conv_by_name(m, "conv_up1", up, RG_NUM_FEAT, H2, W2, RG_NUM_FEAT, feat2) != 0) {
		free(up);
		free(feat2);
		return NULL;
	}
	leaky_relu(feat2, (size_t)RG_NUM_FEAT * plane2);
	free(up);

	/* ---- Upsample stage 2: nearest 2x then lrelu(conv_up2) ---- */
	int    H4 = H * 4, W4 = W * 4;
	size_t plane4 = (size_t)H4 * W4;
	float *up2    = malloc((size_t)RG_NUM_FEAT * plane4 * sizeof(float));
	float *feat3  = malloc((size_t)RG_NUM_FEAT * plane4 * sizeof(float));
	if (!up2 || !feat3) {
		free(feat2);
		free(up2);
		free(feat3);
		fprintf(stderr, "RealESRGAN: out of memory (upsample 2)\n");
		return NULL;
	}
	upsample_nearest_2x(feat2, RG_NUM_FEAT, H2, W2, up2);
	free(feat2);

	if (conv_by_name(m, "conv_up2", up2, RG_NUM_FEAT, H4, W4, RG_NUM_FEAT, feat3) != 0) {
		free(up2);
		free(feat3);
		return NULL;
	}
	leaky_relu(feat3, (size_t)RG_NUM_FEAT * plane4);
	free(up2);

	/* ---- conv_hr -> lrelu -> conv_last: 64 -> 64 -> 3 ---- */
	float *hr  = malloc((size_t)RG_NUM_FEAT * plane4 * sizeof(float));
	float *out = malloc((size_t)3 * plane4 * sizeof(float));
	if (!hr || !out) {
		free(feat3);
		free(hr);
		free(out);
		fprintf(stderr, "RealESRGAN: out of memory (head)\n");
		return NULL;
	}
	if (conv_by_name(m, "conv_hr", feat3, RG_NUM_FEAT, H4, W4, RG_NUM_FEAT, hr) != 0) {
		free(feat3);
		free(hr);
		free(out);
		return NULL;
	}
	free(feat3);
	leaky_relu(hr, (size_t)RG_NUM_FEAT * plane4);

	if (conv_by_name(m, "conv_last", hr, RG_NUM_FEAT, H4, W4, 3, out) != 0) {
		free(hr);
		free(out);
		return NULL;
	}
	free(hr);

	/* ---- CHW float [0,1] -> RGB uint8 image ---- */
	iris_image *result = rg_chw_to_image(out, W4, H4);
	free(out);
	return result;
}
