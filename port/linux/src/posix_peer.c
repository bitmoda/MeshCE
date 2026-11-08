/*
POSIX_PEER.C

Linux half of the peer identity seal. The seed is sealed under a TPM
primary key, and the hello is signed by a second primary key that stays in
the TPM. Neither private key is written to the identity file. A copied file
does not open on another machine. If this machine has no TPM, or the TPM
rejects the command, the functions return 0 and the caller keeps the raw seed.

The owner hierarchy is used with an empty password. A TPM that has an owner
password set will refuse that, and the seed stays raw. One refused command
stops further TPM calls for this run. The refusal is recorded beside the
identity file, and the next launch waits a day before asking again.
*/

#if defined(__linux__) && !defined(HALO_WINDOWS) && !defined(HALO_MACOS_GUEST)

#include "peer_element.h"
#include "peer_tpm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void platform_log(const char *format, ...);
const char *platform_save_root(void);

static int tpm_fd = -1;
static int tpm_unavailable;
static int tpm_stop;
static unsigned tpm_sign_handle;
static unsigned char tpm_sign_point[65];
static int tpm_sign_ready;

static int tpm_wait_path(char *path, int cap)
{
	const char *root;
	int wrote;

	root = platform_save_root();
	if (!path || cap < 32 || !root || !root[0])
		return 0;
	wrote = snprintf(path, (size_t)cap, "%s/peer-tpm-wait", root);
	return wrote > 0 && wrote < cap;
}

static unsigned long long tpm_now(void)
{
	time_t now = time(NULL);

	if (now <= 0)
		return 0;
	return (unsigned long long)now;
}

static void tpm_put_u64(unsigned char out[8], unsigned long long value)
{
	int index;

	for (index = 0; index < 8; index++)
		out[index] = (unsigned char)((value >> (8 * index)) & 0xffu);
}

static unsigned long long tpm_get_u64(const unsigned char in[8])
{
	unsigned long long value = 0;
	int index;

	for (index = 7; index >= 0; index--)
		value = (value << 8) | in[index];
	return value;
}

/* A bad owner password steps the TPM lockout counter. Ask once a day. */
static int tpm_waiting(void)
{
	char path[512];
	FILE *file;
	unsigned char stamp[8];
	unsigned long long then;
	unsigned long long now;

	if (!tpm_wait_path(path, (int)sizeof(path)))
		return 0;
	file = fopen(path, "rb");
	if (!file)
		return 0;
	if (fread(stamp, 1, sizeof(stamp), file) != sizeof(stamp))
	{
		fclose(file);
		return 0;
	}
	fclose(file);
	then = tpm_get_u64(stamp);
	now = tpm_now();
	if (now == 0 || then > now)
		return 1;
	return now - then < 24ull * 60ull * 60ull;
}

static void tpm_remember_wait(void)
{
	char path[512];
	FILE *file;
	unsigned char stamp[8];

	if (!tpm_wait_path(path, (int)sizeof(path)))
		return;
	tpm_put_u64(stamp, tpm_now());
	file = fopen(path, "wb");
	if (!file)
		return;
	if (fwrite(stamp, 1, sizeof(stamp), file) != sizeof(stamp))
	{
		fclose(file);
		remove(path);
		return;
	}
	fclose(file);
}

static int tpm_ready(void)
{
	if (tpm_stop || tpm_unavailable)
		return 0;
	if (tpm_fd >= 0)
		return 1;
	if (tpm_waiting())
	{
		tpm_unavailable = 1;
		platform_log("peer tpm is waiting a day before trying again");
		return 0;
	}
	/* The raw device is only for a machine with no resource manager node.
	   Opening it while tpmrm0 exists can steal the TPM from the rest of the system. */
	do
		tpm_fd = open("/dev/tpmrm0", O_RDWR | O_CLOEXEC);
	while (tpm_fd < 0 && errno == EINTR);
	if (tpm_fd < 0 && errno == ENOENT)
	{
		do
			tpm_fd = open("/dev/tpm0", O_RDWR | O_CLOEXEC);
		while (tpm_fd < 0 && errno == EINTR);
	}
	if (tpm_fd < 0)
	{
		tpm_unavailable = 1;
		platform_log("peer tpm is not available");
		return 0;
	}
	return 1;
}

static int tpm_write_all(const unsigned char *buf, int len)
{
	int at = 0;

	while (at < len)
	{
		int n = (int)write(tpm_fd, buf + at, (size_t)(len - at));

		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			return 0;
		}
		if (n == 0)
			return 0;
		at += n;
	}
	return 1;
}

static int tpm_read_all(unsigned char *buf, int cap)
{
	int got = 0;
	unsigned total;

	while (got < 10)
	{
		int n = (int)read(tpm_fd, buf + got, (size_t)(cap - got));

		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		got += n;
	}
	total = tpm_be32(buf + 2);
	if (total < 10 || (int)total > cap)
		return -1;
	while (got < (int)total)
	{
		int n = (int)read(tpm_fd, buf + got, (size_t)(total - (unsigned)got));

		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		got += n;
	}
	return (int)total;
}

static int tpm_call(const unsigned char *cmd, int cmd_len, unsigned char *resp, int resp_cap, int *resp_len)
{
	int attempt;
	unsigned rc = 0xffffffffu;

	if (tpm_stop || !cmd || cmd_len < 10 || !resp || resp_cap < 10 || !resp_len)
		return 0;
	for (attempt = 0; attempt < 3; attempt++)
	{
		int n;

		if (!tpm_write_all(cmd, cmd_len))
		{
			platform_log("peer tpm command failed");
			tpm_stop = 1;
			return 0;
		}
		n = tpm_read_all(resp, resp_cap);
		if (n < 10)
		{
			platform_log("peer tpm command failed");
			tpm_stop = 1;
			return 0;
		}
		*resp_len = n;
		rc = tpm_response_rc(resp, n);
		if (rc == 0)
			return 1;
		if (!tpm_rc_retry(rc))
			break;
	}
	platform_log("peer tpm command %02x%02x%02x%02x returned %08x", cmd[6], cmd[7], cmd[8], cmd[9], rc);
	if (tpm_rc_auth_stop(rc))
	{
		tpm_stop = 1;
		tpm_remember_wait();
		if (rc == 0x00000921u)
			platform_log("peer tpm is in lockout. The next try is in a day");
		else
			platform_log("peer tpm owner password was refused. The next try is in a day");
	}
	return 0;
}

static void tpm_flush(unsigned handle)
{
	unsigned char cmd[32];
	unsigned char resp[64];
	int cmd_len;
	int n;

	if (!handle || tpm_fd < 0)
		return;
	cmd_len = tpm_build_flush(cmd, (int)sizeof(cmd), handle);
	if (cmd_len <= 0)
		return;
	if (!tpm_write_all(cmd, cmd_len))
		return;
	n = tpm_read_all(resp, (int)sizeof(resp));
	(void)n;
}

/* Kept loaded for this process so the advertised key and the signature are the same primary. */
static int tpm_signing_key(unsigned char point[65], unsigned *handle)
{
	unsigned char cmd[512];
	unsigned char resp[2048];
	int cmd_len;
	int resp_len = 0;
	unsigned found = 0;

	if (tpm_sign_ready)
	{
		if (tpm_stop || tpm_fd < 0)
			return 0;
		memcpy(point, tpm_sign_point, 65);
		if (handle)
			*handle = tpm_sign_handle;
		return 1;
	}
	cmd_len = tpm_build_primary(cmd, (int)sizeof(cmd), 1);
	if (cmd_len <= 0 || !tpm_call(cmd, cmd_len, resp, (int)sizeof(resp), &resp_len))
		return 0;
	if (!tpm_response_public_point(resp, resp_len, &found, point))
	{
		tpm_flush(found);
		return 0;
	}
	memcpy(tpm_sign_point, point, 65);
	tpm_sign_handle = found;
	tpm_sign_ready = 1;
	if (handle)
		*handle = found;
	return 1;
}

int host_peer_wrap(const unsigned char *seed, int seed_size, unsigned char *out, int out_cap)
{
	unsigned char cmd[512];
	unsigned char resp[2048];
	unsigned char point[65];
	unsigned char priv[768];
	unsigned char pub[256];
	int cmd_len;
	int resp_len = 0;
	int priv_len = 0;
	int pub_len = 0;
	unsigned parent = 0;
	int packed = 0;

	if (!seed || seed_size != 32 || !out || out_cap <= 0 || !tpm_ready())
		return 0;
	cmd_len = tpm_build_primary(cmd, (int)sizeof(cmd), 0);
	if (cmd_len <= 0 || !tpm_call(cmd, cmd_len, resp, (int)sizeof(resp), &resp_len) ||
		!tpm_response_public_point(resp, resp_len, &parent, point))
	{
		tpm_flush(parent);
		return 0;
	}
	cmd_len = tpm_build_create(cmd, (int)sizeof(cmd), parent, seed);
	if (cmd_len <= 0 || !tpm_call(cmd, cmd_len, resp, (int)sizeof(resp), &resp_len) ||
		!tpm_response_seal(resp, resp_len, priv, (int)sizeof(priv), &priv_len, pub, (int)sizeof(pub), &pub_len))
	{
		tpm_flush(parent);
		memset(cmd, 0, sizeof(cmd));
		memset(resp, 0, sizeof(resp));
		return 0;
	}
	tpm_flush(parent);
	packed = tpm_pack_seal(out, out_cap, priv, priv_len, pub, pub_len);
	memset(cmd, 0, sizeof(cmd));
	memset(resp, 0, sizeof(resp));
	memset(priv, 0, sizeof(priv));
	return packed;
}

int host_peer_unwrap(const unsigned char *wrapped, int wrapped_size, unsigned char *seed, int seed_size)
{
	const unsigned char *priv;
	const unsigned char *pub;
	unsigned char cmd[2048];
	unsigned char resp[2048];
	unsigned char point[65];
	int priv_len = 0;
	int pub_len = 0;
	int cmd_len;
	int resp_len = 0;
	unsigned parent = 0;
	unsigned item = 0;
	int ok = 0;

	if (!wrapped || wrapped_size <= 0 || !seed || seed_size != 32 || !tpm_ready())
		return 0;
	if (!tpm_unpack_seal(wrapped, wrapped_size, &priv, &priv_len, &pub, &pub_len))
		return 0;
	cmd_len = tpm_build_primary(cmd, (int)sizeof(cmd), 0);
	if (cmd_len <= 0 || !tpm_call(cmd, cmd_len, resp, (int)sizeof(resp), &resp_len) ||
		!tpm_response_public_point(resp, resp_len, &parent, point))
	{
		tpm_flush(parent);
		return 0;
	}
	cmd_len = tpm_build_load(cmd, (int)sizeof(cmd), parent, priv, priv_len, pub, pub_len);
	if (cmd_len <= 0 || !tpm_call(cmd, cmd_len, resp, (int)sizeof(resp), &resp_len))
	{
		tpm_flush(parent);
		return 0;
	}
	{
		struct tpm_reader ignored;

		memset(&ignored, 0, sizeof(ignored));
		if (!tpm_response_params(resp, resp_len, 1, &item, &ignored))
		{
			tpm_flush(item);
			tpm_flush(parent);
			return 0;
		}
	}
	cmd_len = tpm_build_unseal(cmd, (int)sizeof(cmd), item);
	if (cmd_len > 0 && tpm_call(cmd, cmd_len, resp, (int)sizeof(resp), &resp_len) &&
		tpm_response_secret(resp, resp_len, seed))
		ok = 1;
	tpm_flush(item);
	tpm_flush(parent);
	memset(resp, 0, sizeof(resp));
	return ok;
}

int host_peer_public(unsigned char *out, int out_cap)
{
	unsigned char point[65];

	if (!out || out_cap < 65 || !tpm_ready())
		return 0;
	if (!tpm_signing_key(point, 0))
		return 0;
	memcpy(out, point, 65);
	return 65;
}

int host_peer_sign(const unsigned char *message, int message_size, unsigned char *out, int out_cap)
{
	unsigned char digest[32];
	unsigned char point[65];
	unsigned char cmd[512];
	unsigned char resp[2048];
	unsigned char r[48];
	unsigned char s[48];
	int r_len = 0;
	int s_len = 0;
	int cmd_len;
	int resp_len = 0;
	unsigned handle = 0;
	int der = 0;

	if (!message || message_size <= 0 || message_size > 1024 || !out || out_cap <= 0 || !tpm_ready())
		return 0;
	if (mbedtls_sha256(message, (size_t)message_size, digest, 0) != 0)
		return 0;
	if (!tpm_signing_key(point, &handle))
		return 0;
	cmd_len = tpm_build_sign(cmd, (int)sizeof(cmd), handle, digest);
	if (cmd_len > 0 && tpm_call(cmd, cmd_len, resp, (int)sizeof(resp), &resp_len) &&
		tpm_response_ecdsa(resp, resp_len, r, &r_len, s, &s_len, 48))
		der = peer_element_der(out, out_cap, r, r_len, s, s_len);
	memset(digest, 0, sizeof(digest));
	return der;
}

int host_peer_verify(const unsigned char *public_key, int public_size, const unsigned char *message,
	int message_size, const unsigned char *signature, int signature_size)
{
	return peer_element_verify(public_key, public_size, message, message_size, signature, signature_size);
}

#endif
