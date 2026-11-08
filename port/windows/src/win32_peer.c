/*
WIN32_PEER.C

Windows half of the peer identity seal. The platform crypto provider keeps
the keys in the TPM for this user. The signing key is P-256 and is named
opence.peer.v1. The seed is sealed to a second key, opence.peer.v1.seal,
with RSA-OAEP and SHA-256. Neither private key is exportable. A copied
identity file does not open on another machine, or for another Windows user.

If the machine has no TPM, or the provider refuses the key, the functions
return 0 and the caller keeps the raw seed. The P-256 signature is the same
DER encoding a Mac hello uses. Checking it shows the sender holds that key.
It does not, by itself, prove the key was created in a TPM: a modified
program can sign with a key it generated in ordinary memory.
*/

#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>

#include <string.h>

#include "peer_element.h"

void platform_log(const char *format, ...);

#define PEER_SIGN_KEY L"opence.peer.v1"
#define PEER_SEAL_KEY L"opence.peer.v1.seal"

static NCRYPT_PROV_HANDLE provider;
static NCRYPT_KEY_HANDLE sign_key;
static NCRYPT_KEY_HANDLE seal_key;
static int provider_failed;
static int key_logs;

static int open_provider(void)
{
	SECURITY_STATUS status;

	if (provider)
		return 1;
	if (provider_failed)
		return 0;
	status = NCryptOpenStorageProvider(&provider, MS_PLATFORM_CRYPTO_PROVIDER, 0);
	if (status != ERROR_SUCCESS)
	{
		provider_failed = 1;
		platform_log("peer tpm is not available");
		return 0;
	}
	return 1;
}

static void log_key(SECURITY_STATUS status)
{
	if (key_logs >= 4)
		return;
	key_logs++;
	platform_log("peer tpm key was not created, status %08lx", (unsigned long)status);
}

static int ensure_key(const wchar_t *name, const wchar_t *algorithm, int rsa, NCRYPT_KEY_HANDLE *slot)
{
	SECURITY_STATUS status;
	NCRYPT_KEY_HANDLE created;
	NCRYPT_KEY_HANDLE opened;
	DWORD bits;
	DWORD usage;
	DWORD policy;

	if (*slot)
		return 1;
	if (!open_provider())
		return 0;
	opened = 0;
	status = NCryptOpenKey(provider, &opened, name, 0, NCRYPT_SILENT_FLAG);
	if (status == ERROR_SUCCESS && opened)
	{
		*slot = opened;
		return 1;
	}
	if ((unsigned long)status != (unsigned long)NTE_BAD_KEYSET &&
		(unsigned long)status != (unsigned long)NTE_NO_KEY)
	{
		log_key(status);
		return 0;
	}
	created = 0;
	status = NCryptCreatePersistedKey(provider, &created, algorithm, name, 0, NCRYPT_SILENT_FLAG);
	if (status != ERROR_SUCCESS)
	{
		log_key(status);
		return 0;
	}
	bits = 2048;
	usage = rsa ? NCRYPT_ALLOW_DECRYPT_FLAG : NCRYPT_ALLOW_SIGNING_FLAG;
	policy = 0;
	if (rsa)
	{
		status = NCryptSetProperty(created, NCRYPT_LENGTH_PROPERTY, (PBYTE)&bits, sizeof(bits), 0);
		if (status != ERROR_SUCCESS)
		{
			log_key(status);
			NCryptFreeObject(created);
			return 0;
		}
	}
	/* Finalize only after the usage sticks. A key that cannot decrypt would keep this name. */
	status = NCryptSetProperty(created, NCRYPT_KEY_USAGE_PROPERTY, (PBYTE)&usage, sizeof(usage), 0);
	if (status != ERROR_SUCCESS)
	{
		log_key(status);
		NCryptFreeObject(created);
		return 0;
	}
	NCryptSetProperty(created, NCRYPT_EXPORT_POLICY_PROPERTY, (PBYTE)&policy, sizeof(policy), 0);
	status = NCryptFinalizeKey(created, NCRYPT_SILENT_FLAG);
	if (status != ERROR_SUCCESS)
	{
		log_key(status);
		NCryptFreeObject(created);
		return 0;
	}
	*slot = created;
	return 1;
}

static int sha256(const unsigned char *message, int message_size, unsigned char digest[32])
{
	BCRYPT_ALG_HANDLE algorithm = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	NTSTATUS status;
	int ok = 0;

	status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0);
	if (status != 0)
		return 0;
	status = BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0);
	if (status == 0)
		status = BCryptHashData(hash, (PUCHAR)message, (ULONG)message_size, 0);
	if (status == 0)
		status = BCryptFinishHash(hash, digest, 32, 0);
	if (status == 0)
		ok = 1;
	if (hash)
		BCryptDestroyHash(hash);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return ok;
}

int host_peer_public(unsigned char *out, int out_cap)
{
	unsigned char blob[128];
	DWORD size = 0;
	DWORD magic = 0;
	DWORD key_bytes = 0;
	SECURITY_STATUS status;

	if (!out || out_cap < 65)
		return 0;
	if (!ensure_key(PEER_SIGN_KEY, NCRYPT_ECDSA_P256_ALGORITHM, 0, &sign_key))
		return 0;
	status = NCryptExportKey(sign_key, 0, BCRYPT_ECCPUBLIC_BLOB, NULL, blob, sizeof(blob), &size, 0);
	if (status != ERROR_SUCCESS || size < sizeof(magic) + sizeof(key_bytes) + 64)
		return 0;
	memcpy(&magic, blob, sizeof(magic));
	memcpy(&key_bytes, blob + sizeof(magic), sizeof(key_bytes));
	if (magic != BCRYPT_ECDSA_PUBLIC_P256_MAGIC || key_bytes != 32)
		return 0;
	out[0] = 0x04;
	memcpy(out + 1, blob + sizeof(magic) + sizeof(key_bytes), 64);
	return 65;
}

int host_peer_sign(const unsigned char *message, int message_size, unsigned char *out, int out_cap)
{
	unsigned char digest[32];
	unsigned char raw[64];
	DWORD size = 0;
	SECURITY_STATUS status;
	int der = 0;

	if (!message || message_size <= 0 || message_size > 1024 || !out || out_cap <= 0)
		return 0;
	if (!ensure_key(PEER_SIGN_KEY, NCRYPT_ECDSA_P256_ALGORITHM, 0, &sign_key))
		return 0;
	if (!sha256(message, message_size, digest))
		return 0;
	status = NCryptSignHash(sign_key, NULL, digest, sizeof(digest), raw, sizeof(raw), &size, NCRYPT_SILENT_FLAG);
	if (status == ERROR_SUCCESS && size == sizeof(raw))
		der = peer_element_der(out, out_cap, raw, 32, raw + 32, 32);
	memset(digest, 0, sizeof(digest));
	memset(raw, 0, sizeof(raw));
	return der;
}

int host_peer_wrap(const unsigned char *seed, int seed_size, unsigned char *out, int out_cap)
{
	BCRYPT_OAEP_PADDING_INFO padding;
	unsigned char cipher[512];
	DWORD size = 0;
	SECURITY_STATUS status;

	if (!seed || seed_size != 32 || !out || out_cap < 2)
		return 0;
	if (!ensure_key(PEER_SEAL_KEY, NCRYPT_RSA_ALGORITHM, 1, &seal_key))
		return 0;
	padding.pszAlgId = BCRYPT_SHA256_ALGORITHM;
	padding.pbLabel = NULL;
	padding.cbLabel = 0;
	status = NCryptEncrypt(seal_key, (PBYTE)seed, 32, &padding, cipher, sizeof(cipher), &size,
		NCRYPT_PAD_OAEP_FLAG | NCRYPT_SILENT_FLAG);
	if (status != ERROR_SUCCESS || size == 0 || size >= sizeof(cipher) || 1 + (int)size > out_cap)
	{
		memset(cipher, 0, sizeof(cipher));
		return 0;
	}
	out[0] = 1;
	memcpy(out + 1, cipher, size);
	memset(cipher, 0, sizeof(cipher));
	return 1 + (int)size;
}

int host_peer_unwrap(const unsigned char *wrapped, int wrapped_size, unsigned char *seed, int seed_size)
{
	BCRYPT_OAEP_PADDING_INFO padding;
	unsigned char plain[512];
	DWORD size = 0;
	SECURITY_STATUS status;

	if (!wrapped || wrapped_size < 2 || wrapped[0] != 1 || !seed || seed_size != 32)
		return 0;
	if (!ensure_key(PEER_SEAL_KEY, NCRYPT_RSA_ALGORITHM, 1, &seal_key))
		return 0;
	padding.pszAlgId = BCRYPT_SHA256_ALGORITHM;
	padding.pbLabel = NULL;
	padding.cbLabel = 0;
	status = NCryptDecrypt(seal_key, (PBYTE)wrapped + 1, (DWORD)(wrapped_size - 1), &padding, plain, sizeof(plain),
		&size, NCRYPT_PAD_OAEP_FLAG | NCRYPT_SILENT_FLAG);
	if (status != ERROR_SUCCESS || size != 32)
	{
		memset(plain, 0, sizeof(plain));
		return 0;
	}
	memcpy(seed, plain, 32);
	memset(plain, 0, sizeof(plain));
	return 1;
}

int host_peer_verify(const unsigned char *public_key, int public_size, const unsigned char *message,
	int message_size, const unsigned char *signature, int signature_size)
{
	return peer_element_verify(public_key, public_size, message, message_size, signature, signature_size);
}

int host_peer_replace(const char *from, const char *to)
{
	if (!from || !to)
		return 0;
	return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
}
