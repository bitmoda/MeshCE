/*
PEER_ELEMENT.H

P-256 ECDSA, the signature a hello carries for the machine's secure element.
The signature is DER (X9.62) over SHA-256 of the signed bytes, which is the
form the Mac Secure Enclave produces and the form Windows CNG and a TPM
produce once r and s are encoded. Checking it shows the sender holds that
P-256 key. It does not show that the key was created inside a TPM.
*/

#ifndef PEER_ELEMENT_H
#define PEER_ELEMENT_H

#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

#include <string.h>

static int peer_element_integer(unsigned char *out, int cap, const unsigned char *be, int len)
{
	int prefix;

	if (!out || !be || len < 1)
		return -1;
	while (len > 1 && be[0] == 0)
	{
		be++;
		len--;
	}
	prefix = (be[0] & 0x80) ? 1 : 0;
	if (prefix + len > 127 || cap < 2 + prefix + len)
		return -1;
	out[0] = 0x02;
	out[1] = (unsigned char)(prefix + len);
	if (prefix)
		out[2] = 0;
	memcpy(out + 2 + prefix, be, (size_t)len);
	return 2 + prefix + len;
}

/* DER SEQUENCE of r and s, the encoding a Mac hello already uses. */
static int peer_element_der(unsigned char *out, int cap, const unsigned char *r, int r_len,
	const unsigned char *s, int s_len)
{
	unsigned char body[80];
	int ri;
	int si;
	int n;

	if (!out || cap < 2)
		return 0;
	ri = peer_element_integer(body, 40, r, r_len);
	if (ri < 0)
		return 0;
	si = peer_element_integer(body + ri, (int)sizeof(body) - ri, s, s_len);
	if (si < 0)
		return 0;
	n = ri + si;
	if (n > 127 || cap < 2 + n)
		return 0;
	out[0] = 0x30;
	out[1] = (unsigned char)n;
	memcpy(out + 2, body, (size_t)n);
	return 2 + n;
}

/*
Uncompressed P-256 point, 65 bytes starting with 0x04. Returns 1 when the
DER signature matches SHA-256 of the message.
*/
static int peer_element_verify(const unsigned char *public_key, int public_size,
	const unsigned char *message, int message_size, const unsigned char *signature, int signature_size)
{
	static const unsigned char prefix[26] = {
		0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
		0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00
	};
	unsigned char spki[91];
	unsigned char hash[32];
	mbedtls_pk_context key;
	int ok = 0;

	if (!public_key || public_size != 65 || public_key[0] != 0x04 || !message || message_size <= 0 ||
		!signature || signature_size <= 0 || signature_size > 80)
		return 0;
	memcpy(spki, prefix, sizeof(prefix));
	memcpy(spki + sizeof(prefix), public_key, 65);
	if (mbedtls_sha256(message, (size_t)message_size, hash, 0) != 0)
		return 0;
	mbedtls_pk_init(&key);
	if (mbedtls_pk_parse_public_key(&key, spki, sizeof(spki)) == 0 &&
		mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature,
			(size_t)signature_size) == 0)
		ok = 1;
	mbedtls_pk_free(&key);
	return ok;
}

#endif
