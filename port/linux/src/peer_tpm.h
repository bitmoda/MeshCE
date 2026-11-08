/*
PEER_TPM.H

TPM 2.0 command and response bytes for the Linux peer seal. Integers are
big-endian, as the TPM spec requires. The storage parent and the signing
key are primary keys: the same template on the same TPM yields the same
key, and the private half is not in the blob we save. A sealed seed loads
only under that parent, so a copied file does not open on another machine.

Empty owner authorization is the password session. A TPM whose owner
password is set rejects that, and the caller leaves the seed unsealed.
tpm_rc_retry is the warning to send again. tpm_rc_auth_stop is a bad
password or lockout: those are final for this run.
*/

#ifndef PEER_TPM_H
#define PEER_TPM_H

#include <string.h>

struct tpm_writer
{
	unsigned char *data;
	int cap;
	int used;
	int overflow;
};

struct tpm_reader
{
	const unsigned char *data;
	int size;
	int at;
	int overflow;
};

static void tpm_w_bytes(struct tpm_writer *w, const void *bytes, int size)
{
	if (w->overflow)
		return;
	if (size < 0 || w->used > w->cap || size > w->cap - w->used)
	{
		w->overflow = 1;
		return;
	}
	if (size > 0 && bytes)
		memcpy(w->data + w->used, bytes, (size_t)size);
	w->used += size;
}

static void tpm_w_u8(struct tpm_writer *w, unsigned value)
{
	unsigned char byte = (unsigned char)value;

	tpm_w_bytes(w, &byte, 1);
}

static void tpm_w_u16(struct tpm_writer *w, unsigned value)
{
	unsigned char bytes[2];

	bytes[0] = (unsigned char)((value >> 8) & 0xffu);
	bytes[1] = (unsigned char)(value & 0xffu);
	tpm_w_bytes(w, bytes, 2);
}

static void tpm_w_u32(struct tpm_writer *w, unsigned value)
{
	unsigned char bytes[4];

	bytes[0] = (unsigned char)((value >> 24) & 0xffu);
	bytes[1] = (unsigned char)((value >> 16) & 0xffu);
	bytes[2] = (unsigned char)((value >> 8) & 0xffu);
	bytes[3] = (unsigned char)(value & 0xffu);
	tpm_w_bytes(w, bytes, 4);
}

static void tpm_begin(struct tpm_writer *w, unsigned char *out, int cap, int sessions, unsigned code)
{
	w->data = out;
	w->cap = cap;
	w->used = 0;
	w->overflow = 0;
	tpm_w_u16(w, sessions ? 0x8002u : 0x8001u);
	tpm_w_u32(w, 0);
	tpm_w_u32(w, code);
}

static int tpm_finish(struct tpm_writer *w)
{
	if (!w->data || w->overflow || w->used < 10)
		return 0;
	w->data[2] = (unsigned char)(((unsigned)w->used >> 24) & 0xffu);
	w->data[3] = (unsigned char)(((unsigned)w->used >> 16) & 0xffu);
	w->data[4] = (unsigned char)(((unsigned)w->used >> 8) & 0xffu);
	w->data[5] = (unsigned char)((unsigned)w->used & 0xffu);
	return w->used;
}

static void tpm_fill_label(unsigned char block[32], const char *text, unsigned char fill)
{
	int n = 0;

	memset(block, fill, 32);
	if (!text)
		return;
	while (text[n] && n < 32)
	{
		block[n] = (unsigned char)text[n];
		n++;
	}
}

static void tpm_put_password(struct tpm_writer *w)
{
	tpm_w_u32(w, 9);
	tpm_w_u32(w, 0x40000009u);
	tpm_w_u16(w, 0);
	tpm_w_u8(w, 0);
	tpm_w_u16(w, 0);
}

/*
fixedTPM | fixedParent | sensitiveDataOrigin | userWithAuth | noDA, and
either restricted|decrypt (a storage parent) or sign (an unrestricted
signing key). noDA keeps a bad password from counting toward lockout on
this object. The owner hierarchy still counts a bad password on the handle.
*/
static void tpm_put_public_ecc(struct tpm_writer *w, int signing)
{
	int size_at;
	int start;
	unsigned char label_x[32];
	unsigned char label_y[32];

	size_at = w->used;
	tpm_w_u16(w, 0);
	start = w->used;
	tpm_w_u16(w, 0x0023u);
	tpm_w_u16(w, 0x000bu);
	tpm_w_u32(w, signing ? 0x00040472u : 0x00030472u);
	tpm_w_u16(w, 0);
	if (signing)
		tpm_w_u16(w, 0x0010u);
	else
	{
		tpm_w_u16(w, 0x0006u);
		tpm_w_u16(w, 128);
		tpm_w_u16(w, 0x0043u);
	}
	if (signing)
	{
		tpm_w_u16(w, 0x0018u);
		tpm_w_u16(w, 0x000bu);
	}
	else
		tpm_w_u16(w, 0x0010u);
	tpm_w_u16(w, 0x0003u);
	tpm_w_u16(w, 0x0010u);
	tpm_fill_label(label_x, signing ? "OpenCE peer sign v1" : "OpenCE peer parent v1", 0);
	tpm_fill_label(label_y, signing ? "OpenCE peer sign y1" : "OpenCE peer parent y1",
		(unsigned char)(signing ? 0x22 : 0x11));
	tpm_w_u16(w, 32);
	tpm_w_bytes(w, label_x, 32);
	tpm_w_u16(w, 32);
	tpm_w_bytes(w, label_y, 32);
	if (!w->overflow && start >= 2 && w->used >= start)
	{
		int size = w->used - start;

		w->data[size_at] = (unsigned char)((size >> 8) & 0xff);
		w->data[size_at + 1] = (unsigned char)(size & 0xff);
	}
}

static void tpm_put_public_seal(struct tpm_writer *w)
{
	int size_at;
	int start;

	size_at = w->used;
	tpm_w_u16(w, 0);
	start = w->used;
	tpm_w_u16(w, 0x0008u);
	tpm_w_u16(w, 0x000bu);
	tpm_w_u32(w, 0x00000452u);
	tpm_w_u16(w, 0);
	tpm_w_u16(w, 0x0010u);
	tpm_w_u16(w, 0);
	if (!w->overflow && start >= 2 && w->used >= start)
	{
		int size = w->used - start;

		w->data[size_at] = (unsigned char)((size >> 8) & 0xff);
		w->data[size_at + 1] = (unsigned char)(size & 0xff);
	}
}

static void tpm_put_sensitive_empty(struct tpm_writer *w)
{
	tpm_w_u16(w, 4);
	tpm_w_u16(w, 0);
	tpm_w_u16(w, 0);
}

static void tpm_put_sensitive_seed(struct tpm_writer *w, const unsigned char seed[32])
{
	tpm_w_u16(w, 36);
	tpm_w_u16(w, 0);
	tpm_w_u16(w, 32);
	tpm_w_bytes(w, seed, 32);
}

static void tpm_put_outside(struct tpm_writer *w)
{
	tpm_w_u16(w, 0);
	tpm_w_u32(w, 0);
}

static int tpm_build_primary(unsigned char *out, int cap, int signing)
{
	struct tpm_writer w;

	tpm_begin(&w, out, cap, 1, 0x00000131u);
	tpm_w_u32(&w, 0x40000001u);
	tpm_put_password(&w);
	tpm_put_sensitive_empty(&w);
	tpm_put_public_ecc(&w, signing);
	tpm_put_outside(&w);
	return tpm_finish(&w);
}

static int tpm_build_create(unsigned char *out, int cap, unsigned parent, const unsigned char seed[32])
{
	struct tpm_writer w;

	tpm_begin(&w, out, cap, 1, 0x00000153u);
	tpm_w_u32(&w, parent);
	tpm_put_password(&w);
	tpm_put_sensitive_seed(&w, seed);
	tpm_put_public_seal(&w);
	tpm_put_outside(&w);
	return tpm_finish(&w);
}

static int tpm_build_load(unsigned char *out, int cap, unsigned parent, const unsigned char *priv, int priv_len,
	const unsigned char *pub, int pub_len)
{
	struct tpm_writer w;

	tpm_begin(&w, out, cap, 1, 0x00000157u);
	tpm_w_u32(&w, parent);
	tpm_put_password(&w);
	tpm_w_bytes(&w, priv, priv_len);
	tpm_w_bytes(&w, pub, pub_len);
	return tpm_finish(&w);
}

static int tpm_build_unseal(unsigned char *out, int cap, unsigned handle)
{
	struct tpm_writer w;

	tpm_begin(&w, out, cap, 1, 0x0000015eu);
	tpm_w_u32(&w, handle);
	tpm_put_password(&w);
	return tpm_finish(&w);
}

static int tpm_build_sign(unsigned char *out, int cap, unsigned handle, const unsigned char digest[32])
{
	struct tpm_writer w;

	tpm_begin(&w, out, cap, 1, 0x0000015du);
	tpm_w_u32(&w, handle);
	tpm_put_password(&w);
	tpm_w_u16(&w, 32);
	tpm_w_bytes(&w, digest, 32);
	tpm_w_u16(&w, 0x0010u);
	tpm_w_u16(&w, 0x8024u);
	tpm_w_u32(&w, 0x40000007u);
	tpm_w_u16(&w, 0);
	return tpm_finish(&w);
}

static int tpm_build_flush(unsigned char *out, int cap, unsigned handle)
{
	struct tpm_writer w;

	tpm_begin(&w, out, cap, 0, 0x00000165u);
	tpm_w_u32(&w, handle);
	return tpm_finish(&w);
}

static unsigned tpm_r_u16(struct tpm_reader *r)
{
	unsigned value;

	if (r->overflow || r->at < 0 || r->size < r->at + 2)
	{
		r->overflow = 1;
		return 0;
	}
	value = ((unsigned)r->data[r->at] << 8) | r->data[r->at + 1];
	r->at += 2;
	return value;
}

static unsigned tpm_r_u32(struct tpm_reader *r)
{
	unsigned value;

	if (r->overflow || r->at < 0 || r->size < r->at + 4)
	{
		r->overflow = 1;
		return 0;
	}
	value = ((unsigned)r->data[r->at] << 24) | ((unsigned)r->data[r->at + 1] << 16) |
		((unsigned)r->data[r->at + 2] << 8) | r->data[r->at + 3];
	r->at += 4;
	return value;
}

static const unsigned char *tpm_r_ptr(struct tpm_reader *r, int n)
{
	const unsigned char *p;

	if (r->overflow || n < 0 || r->at < 0 || r->size < r->at || n > r->size - r->at)
	{
		r->overflow = 1;
		return 0;
	}
	p = r->data + r->at;
	r->at += n;
	return p;
}

static unsigned tpm_be32(const unsigned char *p)
{
	return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

static unsigned tpm_response_rc(const unsigned char *resp, int len)
{
	if (!resp || len < 10)
		return 0xffffffffu;
	return tpm_be32(resp + 6);
}

static int tpm_response_params(const unsigned char *resp, int len, int want_handle, unsigned *handle,
	struct tpm_reader *params)
{
	struct tpm_reader r;
	unsigned tag;
	unsigned size;
	unsigned rc;
	unsigned param_size;

	if (!resp || len < 10 || !params)
		return 0;
	tag = ((unsigned)resp[0] << 8) | resp[1];
	size = tpm_be32(resp + 2);
	rc = tpm_be32(resp + 6);
	if ((tag != 0x8001u && tag != 0x8002u) || size < 10 || (int)size > len || rc != 0)
		return 0;
	r.data = resp;
	r.size = (int)size;
	r.at = 10;
	r.overflow = 0;
	if (want_handle)
	{
		unsigned found = tpm_r_u32(&r);

		if (r.overflow)
			return 0;
		if (handle)
			*handle = found;
	}
	if (tag == 0x8002u)
	{
		param_size = tpm_r_u32(&r);
		if (r.overflow || r.size < r.at || (int)param_size > r.size - r.at)
			return 0;
		params->data = r.data + r.at;
		params->size = (int)param_size;
		params->at = 0;
		params->overflow = 0;
	}
	else
	{
		params->data = r.data + r.at;
		params->size = r.size - r.at;
		params->at = 0;
		params->overflow = 0;
	}
	return 1;
}

static int tpm_read_ecc_point(struct tpm_reader *r, unsigned char out[65])
{
	unsigned type;
	unsigned policy;
	unsigned sym;
	unsigned scheme;
	unsigned curve;
	unsigned kdf;
	unsigned x_len;
	unsigned y_len;
	const unsigned char *x;
	const unsigned char *y;

	type = tpm_r_u16(r);
	(void)tpm_r_u16(r);
	(void)tpm_r_u32(r);
	policy = tpm_r_u16(r);
	if (r->overflow || (int)policy > r->size - r->at)
	{
		r->overflow = 1;
		return 0;
	}
	r->at += (int)policy;
	if (type != 0x0023u)
		return 0;
	sym = tpm_r_u16(r);
	if (sym == 0x0006u)
	{
		(void)tpm_r_u16(r);
		(void)tpm_r_u16(r);
	}
	else if (sym != 0x0010u)
		return 0;
	scheme = tpm_r_u16(r);
	if (scheme == 0x0018u || scheme == 0x0019u)
		(void)tpm_r_u16(r);
	else if (scheme != 0x0010u)
		return 0;
	curve = tpm_r_u16(r);
	kdf = tpm_r_u16(r);
	if (curve != 0x0003u || kdf != 0x0010u)
		return 0;
	x_len = tpm_r_u16(r);
	x = tpm_r_ptr(r, (int)x_len);
	y_len = tpm_r_u16(r);
	y = tpm_r_ptr(r, (int)y_len);
	if (r->overflow || x_len != 32 || y_len != 32 || !x || !y)
		return 0;
	out[0] = 0x04;
	memcpy(out + 1, x, 32);
	memcpy(out + 33, y, 32);
	return 1;
}

static int tpm_response_public_point(const unsigned char *resp, int len, unsigned *handle, unsigned char point[65])
{
	struct tpm_reader params;
	struct tpm_reader pub;
	unsigned pub_size;

	if (!tpm_response_params(resp, len, 1, handle, &params))
		return 0;
	pub_size = tpm_r_u16(&params);
	if (params.overflow || params.size < params.at || (int)pub_size > params.size - params.at)
		return 0;
	pub.data = params.data + params.at;
	pub.size = (int)pub_size;
	pub.at = 0;
	pub.overflow = 0;
	return tpm_read_ecc_point(&pub, point);
}

static int tpm_copy_tpm2b(struct tpm_reader *r, unsigned char *out, int cap, int *out_len)
{
	int start;
	unsigned n;

	start = r->at;
	n = tpm_r_u16(r);
	if (r->overflow || r->size < r->at || (int)n > r->size - r->at || 2 + (int)n > cap)
	{
		r->overflow = 1;
		return 0;
	}
	memcpy(out, r->data + start, 2u + n);
	r->at += (int)n;
	*out_len = 2 + (int)n;
	return 1;
}

static int tpm_response_seal(const unsigned char *resp, int len, unsigned char *priv, int priv_cap, int *priv_len,
	unsigned char *pub, int pub_cap, int *pub_len)
{
	struct tpm_reader params;

	if (!tpm_response_params(resp, len, 0, 0, &params))
		return 0;
	if (!tpm_copy_tpm2b(&params, priv, priv_cap, priv_len))
		return 0;
	if (!tpm_copy_tpm2b(&params, pub, pub_cap, pub_len))
		return 0;
	return 1;
}

static int tpm_response_secret(const unsigned char *resp, int len, unsigned char seed[32])
{
	struct tpm_reader params;
	unsigned n;
	const unsigned char *p;

	if (!tpm_response_params(resp, len, 0, 0, &params))
		return 0;
	n = tpm_r_u16(&params);
	p = tpm_r_ptr(&params, (int)n);
	if (params.overflow || n != 32 || !p)
		return 0;
	memcpy(seed, p, 32);
	return 1;
}

static int tpm_response_ecdsa(const unsigned char *resp, int len, unsigned char *r, int *r_len, unsigned char *s,
	int *s_len, int cap)
{
	struct tpm_reader params;
	unsigned alg;
	unsigned n;
	const unsigned char *p;

	if (!tpm_response_params(resp, len, 0, 0, &params))
		return 0;
	alg = tpm_r_u16(&params);
	if (alg != 0x0018u)
		return 0;
	(void)tpm_r_u16(&params);
	n = tpm_r_u16(&params);
	p = tpm_r_ptr(&params, (int)n);
	if (params.overflow || n == 0 || (int)n > cap || !p)
		return 0;
	memcpy(r, p, n);
	*r_len = (int)n;
	n = tpm_r_u16(&params);
	p = tpm_r_ptr(&params, (int)n);
	if (params.overflow || n == 0 || (int)n > cap || !p)
		return 0;
	memcpy(s, p, n);
	*s_len = (int)n;
	return 1;
}

static int tpm_pack_seal(unsigned char *out, int cap, const unsigned char *priv, int priv_len,
	const unsigned char *pub, int pub_len)
{
	int total;

	if (!out || !priv || !pub || priv_len < 2 || pub_len < 2)
		return 0;
	total = 8 + priv_len + pub_len;
	if (total > cap || priv_len > 65535 || pub_len > 65535)
		return 0;
	memcpy(out, "TPM2", 4);
	out[4] = (unsigned char)((priv_len >> 8) & 0xff);
	out[5] = (unsigned char)(priv_len & 0xff);
	memcpy(out + 6, priv, (size_t)priv_len);
	out[6 + priv_len] = (unsigned char)((pub_len >> 8) & 0xff);
	out[7 + priv_len] = (unsigned char)(pub_len & 0xff);
	memcpy(out + 8 + priv_len, pub, (size_t)pub_len);
	return total;
}

static int tpm_unpack_seal(const unsigned char *in, int in_len, const unsigned char **priv, int *priv_len,
	const unsigned char **pub, int *pub_len)
{
	int private_len;
	int public_len;

	if (!in || in_len < 8 || !priv || !priv_len || !pub || !pub_len)
		return 0;
	if (memcmp(in, "TPM2", 4) != 0)
		return 0;
	private_len = ((int)in[4] << 8) | in[5];
	if (private_len < 2 || 6 + private_len > in_len)
		return 0;
	public_len = ((int)in[6 + private_len] << 8) | in[7 + private_len];
	if (public_len < 2 || 8 + private_len + public_len != in_len)
		return 0;
	*priv = in + 6;
	*priv_len = private_len;
	*pub = in + 8 + private_len;
	*pub_len = public_len;
	return 1;
}

/* TPM_RC_YIELDED and TPM_RC_RETRY. Lockout is 0x921 and is not one of these. */
static int tpm_rc_retry(unsigned rc)
{
	return rc == 0x00000908u || rc == 0x00000922u;
}

/* Bad password (0xA2, or the same code with a session number) or DA lockout. */
static int tpm_rc_auth_stop(unsigned rc)
{
	if (rc == 0x00000921u)
		return 1;
	return (rc & 0x80u) != 0 && (rc & 0x7fu) == 0x22u;
}

#endif
