/*
Checks the P-256 signature encoding and the TPM command bytes used to seal a
peer identity. A real TPM is not required.
*/

#include "peer_element.h"
#include "peer_tpm.h"

#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *what)
{
	if (condition)
		return;
	fprintf(stderr, "fail: %s\n", what);
	failures++;
}

static int rng(void *ctx, unsigned char *out, size_t len)
{
	unsigned *state = ctx;

	while (len--)
	{
		*state = *state * 1664525u + 1013904223u;
		*out++ = (unsigned char)(*state >> 16);
	}
	return 0;
}

static int contains(const unsigned char *buf, int len, const void *needle, int needle_len)
{
	int index;

	if (needle_len <= 0 || needle_len > len)
		return 0;
	for (index = 0; index <= len - needle_len; index++)
	{
		if (memcmp(buf + index, needle, (size_t)needle_len) == 0)
			return 1;
	}
	return 0;
}

static void test_der_plain(void)
{
	unsigned char r[32];
	unsigned char s[32];
	unsigned char der[80];
	int n;
	int index;

	memset(r, 0, sizeof(r));
	memset(s, 0, sizeof(s));
	r[0] = 0x01;
	s[0] = 0x02;
	n = peer_element_der(der, (int)sizeof(der), r, 32, s, 32);
	expect(n == 70, "plain der length");
	expect(der[0] == 0x30 && der[1] == 0x44, "plain der header");
	expect(der[2] == 0x02 && der[3] == 0x20 && der[4] == 0x01, "plain r");
	expect(der[36] == 0x02 && der[37] == 0x20 && der[38] == 0x02, "plain s");
	for (index = 5; index < 36; index++)
		expect(der[index] == 0, "plain r pad");
}

static void test_der_high_bit(void)
{
	unsigned char r[32];
	unsigned char s[33];
	unsigned char der[80];
	int n;

	memset(r, 0xff, sizeof(r));
	memset(s, 0x01, sizeof(s));
	s[0] = 0x00;
	s[1] = 0x80;
	n = peer_element_der(der, (int)sizeof(der), r, 32, s, 33);
	expect(n == 2 + 2 + 33 + 2 + 33, "high der length");
	expect(der[0] == 0x30, "high sequence");
	expect(der[2] == 0x02 && der[3] == 33 && der[4] == 0x00 && der[5] == 0xff, "high r prefix");
	expect(der[2 + 2 + 33] == 0x02 && der[2 + 2 + 33 + 1] == 33, "high s length");
	expect(der[2 + 2 + 33 + 2] == 0x00 && der[2 + 2 + 33 + 3] == 0x80, "high s prefix");
}

static void test_commands(void)
{
	unsigned char cmd[512];
	unsigned char seed[32];
	unsigned char flush[14] = {
		0x80, 0x01, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x01, 0x65, 0x80, 0x00, 0x00, 0x01
	};
	unsigned char parent_attr[4] = {0x00, 0x03, 0x04, 0x72};
	unsigned char sign_attr[4] = {0x00, 0x04, 0x04, 0x72};
	unsigned char seal_attr[4] = {0x00, 0x00, 0x04, 0x52};
	int n;
	int index;

	n = tpm_build_primary(cmd, (int)sizeof(cmd), 0);
	expect(n > 16, "parent command size");
	expect(cmd[0] == 0x80 && cmd[1] == 0x02, "parent tag");
	expect(tpm_be32(cmd + 2) == (unsigned)n, "parent length field");
	expect(tpm_be32(cmd + 6) == 0x00000131u, "parent command code");
	expect(tpm_be32(cmd + 10) == 0x40000001u, "parent owner");
	expect(contains(cmd, n, parent_attr, 4), "parent attributes");
	expect(contains(cmd, n, "MeshCE peer parent v1", 20), "parent label");
	expect(cmd[n - 6] == 0 && cmd[n - 5] == 0 && cmd[n - 4] == 0 && cmd[n - 3] == 0 && cmd[n - 2] == 0 &&
		cmd[n - 1] == 0, "parent outside and pcr");

	n = tpm_build_primary(cmd, (int)sizeof(cmd), 1);
	expect(n > 16 && tpm_be32(cmd + 6) == 0x00000131u, "sign primary");
	expect(contains(cmd, n, sign_attr, 4), "sign attributes");
	expect(contains(cmd, n, "MeshCE peer sign v1", 18), "sign label");
	expect(!contains(cmd, n, parent_attr, 4), "sign is not a storage key");

	memset(seed, 0x5a, sizeof(seed));
	n = tpm_build_create(cmd, (int)sizeof(cmd), 0x80000001u, seed);
	expect(n > 16 && tpm_be32(cmd + 6) == 0x00000153u, "create command");
	expect(tpm_be32(cmd + 10) == 0x80000001u, "create parent handle");
	expect(contains(cmd, n, seed, 32), "create carries the seed");
	expect(contains(cmd, n, seal_attr, 4), "seal attributes");
	expect(cmd[n - 6] == 0 && cmd[n - 1] == 0, "create outside and pcr");

	n = tpm_build_sign(cmd, (int)sizeof(cmd), 0x80000002u, seed);
	expect(n > 8 && tpm_be32(cmd + 6) == 0x0000015du, "sign command");
	expect(cmd[n - 8] == 0x80 && cmd[n - 7] == 0x24, "null ticket tag");
	expect(cmd[n - 6] == 0x40 && cmd[n - 5] == 0x00 && cmd[n - 4] == 0x00 && cmd[n - 3] == 0x07, "null hierarchy");
	expect(cmd[n - 2] == 0 && cmd[n - 1] == 0, "null ticket digest");

	n = tpm_build_flush(cmd, (int)sizeof(cmd), 0x80000001u);
	expect(n == 14, "flush length");
	for (index = 0; index < 14; index++)
		expect(cmd[index] == flush[index], "flush bytes");

	n = tpm_build_unseal(cmd, (int)sizeof(cmd), 0x80000003u);
	expect(n > 10 && tpm_be32(cmd + 6) == 0x0000015eu, "unseal command");
	expect(tpm_response_rc(cmd, 4) == 0xffffffffu, "short response has no code");
}

static void test_responses(void)
{
	unsigned char public_area[88];
	unsigned char response[128];
	unsigned char point[65];
	unsigned char secret[32];
	unsigned char r[32];
	unsigned char s[32];
	unsigned char packed[64];
	unsigned char priv[4] = {0x00, 0x02, 0xaa, 0xbb};
	unsigned char pub[4] = {0x00, 0x02, 0xcc, 0xdd};
	const unsigned char *got_priv;
	const unsigned char *got_pub;
	int priv_len = 0;
	int pub_len = 0;
	int r_len = 0;
	int s_len = 0;
	unsigned handle = 0;
	int at = 0;
	int index;

	memset(public_area, 0, sizeof(public_area));
	public_area[at++] = 0x00;
	public_area[at++] = 0x23;
	public_area[at++] = 0x00;
	public_area[at++] = 0x0b;
	public_area[at++] = 0x00;
	public_area[at++] = 0x04;
	public_area[at++] = 0x04;
	public_area[at++] = 0x72;
	public_area[at++] = 0x00;
	public_area[at++] = 0x00;
	public_area[at++] = 0x00;
	public_area[at++] = 0x10;
	public_area[at++] = 0x00;
	public_area[at++] = 0x18;
	public_area[at++] = 0x00;
	public_area[at++] = 0x0b;
	public_area[at++] = 0x00;
	public_area[at++] = 0x03;
	public_area[at++] = 0x00;
	public_area[at++] = 0x10;
	public_area[at++] = 0x00;
	public_area[at++] = 0x20;
	for (index = 0; index < 32; index++)
		public_area[at++] = 0x11;
	public_area[at++] = 0x00;
	public_area[at++] = 0x20;
	for (index = 0; index < 32; index++)
		public_area[at++] = 0x22;
	expect(at == 88, "hand built public area");

	memset(response, 0, sizeof(response));
	response[0] = 0x80;
	response[1] = 0x02;
	response[2] = 0x00;
	response[3] = 0x00;
	response[4] = 0x00;
	response[5] = 108;
	response[10] = 0x80;
	response[11] = 0x00;
	response[12] = 0x00;
	response[13] = 0x01;
	response[14] = 0x00;
	response[15] = 0x00;
	response[16] = 0x00;
	response[17] = 90;
	response[18] = 0x00;
	response[19] = 88;
	memcpy(response + 20, public_area, 88);
	expect(tpm_response_public_point(response, 108, &handle, point), "parse signing public");
	expect(handle == 0x80000001u, "primary handle");
	expect(point[0] == 0x04 && point[1] == 0x11 && point[33] == 0x22, "uncompressed point");
	response[6] = 0x00;
	response[9] = 0x01;
	expect(!tpm_response_public_point(response, 108, &handle, point), "failed command is ignored");

	memset(response, 0, sizeof(response));
	response[0] = 0x80;
	response[1] = 0x02;
	response[5] = 48;
	response[13] = 34;
	response[15] = 32;
	memset(response + 16, 0x44, 32);
	expect(tpm_response_secret(response, response[5], secret), "unseal parse");
	expect(secret[0] == 0x44 && secret[31] == 0x44, "unseal bytes");

	memset(response, 0, sizeof(response));
	response[0] = 0x80;
	response[1] = 0x02;
	response[5] = 30;
	response[13] = 16;
	response[14] = 0x00;
	response[15] = 0x18;
	response[16] = 0x00;
	response[17] = 0x0b;
	response[18] = 0x00;
	response[19] = 4;
	response[20] = 0x00;
	response[21] = 0x01;
	response[22] = 0x02;
	response[23] = 0x03;
	response[24] = 0x00;
	response[25] = 4;
	response[26] = 0x0a;
	response[27] = 0x0b;
	response[28] = 0x0c;
	response[29] = 0x0d;
	expect(tpm_response_ecdsa(response, response[5], r, &r_len, s, &s_len, 32), "signature parse");
	expect(r_len == 4 && s_len == 4 && r[0] == 0x00 && r[3] == 0x03 && s[0] == 0x0a && s[3] == 0x0d, "signature halves");

	index = tpm_pack_seal(packed, (int)sizeof(packed), priv, 4, pub, 4);
	expect(index == 16, "packed seal size");
	expect(tpm_unpack_seal(packed, index, &got_priv, &priv_len, &got_pub, &pub_len), "unpack seal");
	expect(priv_len == 4 && pub_len == 4 && memcmp(got_priv, priv, 4) == 0 && memcmp(got_pub, pub, 4) == 0, "seal bytes");
	expect(!tpm_unpack_seal(packed, index - 1, &got_priv, &priv_len, &got_pub, &pub_len), "truncated seal rejected");
}

static int der_body(const unsigned char *p, int n, int *used, const unsigned char **body, int *body_len)
{
	int len;

	if (n < 2 || p[0] != 0x02)
		return 0;
	len = p[1];
	if (len < 1 || 2 + len > n)
		return 0;
	*body = p + 2;
	*body_len = len;
	*used = 2 + len;
	return 1;
}

static void test_verify(void)
{
	mbedtls_pk_context key;
	unsigned state = 1;
	unsigned char der[160];
	unsigned char hash[32];
	unsigned char message[32];
	unsigned char signature[80];
	unsigned char again[80];
	unsigned char broken[80];
	const unsigned char *written;
	const unsigned char *r_body;
	const unsigned char *s_body;
	size_t signature_len = 0;
	int written_len;
	int used = 0;
	int r_len = 0;
	int s_len = 0;
	int again_len;
	int index;

	memset(message, 0x3c, sizeof(message));
	mbedtls_pk_init(&key);
	expect(mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0, "pk setup");
	expect(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), rng, &state) == 0, "gen key");
	written_len = mbedtls_pk_write_pubkey_der(&key, der, sizeof(der));
	expect(written_len == 91, "spki length");
	if (written_len < 65)
	{
		mbedtls_pk_free(&key);
		return;
	}
	written = der + sizeof(der) - written_len;
	expect(written_len > 65 && written[written_len - 65] == 0x04, "spki point");
	expect(mbedtls_sha256(message, sizeof(message), hash, 0) == 0, "hash");
	expect(mbedtls_pk_sign(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, sizeof(signature), &signature_len,
		rng, &state) == 0, "sign");
	expect(signature_len > 8 && signature[0] == 0x30, "signature is der");
	expect(peer_element_verify(written + written_len - 65, 65, message, 32, signature, (int)signature_len),
		"verify accepts the signature");
	memcpy(broken, signature, signature_len);
	broken[signature_len / 2] ^= 0x01;
	expect(!peer_element_verify(written + written_len - 65, 65, message, 32, broken, (int)signature_len),
		"verify rejects a changed signature");
	expect(der_body(signature + 2, (int)signature_len - 2, &used, &r_body, &r_len), "read r");
	expect(der_body(signature + 2 + used, (int)signature_len - 2 - used, &used, &s_body, &s_len), "read s");
	again_len = peer_element_der(again, (int)sizeof(again), r_body, r_len, s_body, s_len);
	expect(again_len == (int)signature_len && memcmp(again, signature, signature_len) == 0, "der matches mbedtls");
	for (index = 0; index < written_len && index < 26; index++)
	{
		static const unsigned char prefix[26] = {
			0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
			0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00
		};

		expect(written[index] == prefix[index], "spki prefix");
	}
	mbedtls_pk_free(&key);
}

static void test_rc(void)
{
	expect(tpm_rc_retry(0x908u), "yielded is retried");
	expect(tpm_rc_retry(0x922u), "retry is retried");
	expect(!tpm_rc_retry(0x921u), "lockout is final");
	expect(!tpm_rc_retry(0x9a2u), "bad auth is final");
	expect(!tpm_rc_retry(0x184u), "value is final");
	expect(tpm_rc_auth_stop(0x921u), "lockout stops");
	expect(tpm_rc_auth_stop(0xa2u), "bad auth stops");
	expect(tpm_rc_auth_stop(0x9a2u), "session bad auth stops");
	expect(!tpm_rc_auth_stop(0x922u), "retry is not a bad password");
	expect(!tpm_rc_auth_stop(0x184u), "value is not a bad password");
}

/* The largest private and public blobs we copy must fit in the load command buffer. */
static void test_load_size(void)
{
	unsigned char priv[768];
	unsigned char pub[256];
	unsigned char cmd[2048];
	int n;

	memset(priv, 0x11, sizeof(priv));
	memset(pub, 0x22, sizeof(pub));
	n = tpm_build_load(cmd, 1024, 0x80000001u, priv, (int)sizeof(priv), pub, (int)sizeof(pub));
	expect(n == 0, "short load buffer is rejected");
	n = tpm_build_load(cmd, (int)sizeof(cmd), 0x80000001u, priv, (int)sizeof(priv), pub, (int)sizeof(pub));
	expect(n > 1024 && n <= 2048, "load fits in 2048");
}

int main(void)
{
	test_der_plain();
	test_der_high_bit();
	test_commands();
	test_responses();
	test_verify();
	test_rc();
	test_load_size();
	if (failures)
	{
		fprintf(stderr, "%d checks failed\n", failures);
		return 1;
	}
	printf("peer element ok\n");
	return 0;
}
