/*
The install's peer seed, sealed to a key that stays in the secure element.
A copied file cannot be opened on another Mac. The key is not an Apple
attestation. Another peer can check that this element signed the peer id.
A program can still create its own P-256 key in ordinary memory.
*/

#import <Foundation/Foundation.h>
#import <Security/Security.h>

static NSData *peerTag(void)
{
	return [@"meshce.peer.v1" dataUsingEncoding:NSUTF8StringEncoding];
}

static SecKeyRef peerPrivateKey(void)
{
	static SecKeyRef cached;
	static int unavailable;
	NSDictionary *query;
	CFTypeRef found = NULL;
	NSDictionary *attributes;
	CFErrorRef error = NULL;
	SecKeyRef created;

	if (cached)
		return cached;
	if (unavailable)
		return NULL;
	query = @{
		(__bridge id)kSecClass: (__bridge id)kSecClassKey,
		(__bridge id)kSecAttrApplicationTag: peerTag(),
		(__bridge id)kSecAttrKeyType: (__bridge id)kSecAttrKeyTypeECSECPrimeRandom,
		(__bridge id)kSecAttrTokenID: (__bridge id)kSecAttrTokenIDSecureEnclave,
		(__bridge id)kSecReturnRef: @YES
	};
	if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &found) == errSecSuccess && found)
	{
		cached = (SecKeyRef)found;
		return cached;
	}
	attributes = @{
		(__bridge id)kSecAttrKeyType: (__bridge id)kSecAttrKeyTypeECSECPrimeRandom,
		(__bridge id)kSecAttrKeySizeInBits: @256,
		(__bridge id)kSecAttrTokenID: (__bridge id)kSecAttrTokenIDSecureEnclave,
		(__bridge id)kSecPrivateKeyAttrs: @{
			(__bridge id)kSecAttrIsPermanent: @YES,
			(__bridge id)kSecAttrApplicationTag: peerTag()
		}
	};
	created = SecKeyCreateRandomKey((__bridge CFDictionaryRef)attributes, &error);
	if (error)
		CFRelease(error);
	if (!created)
	{
		unavailable = 1;
		return NULL;
	}
	cached = created;
	return cached;
}

static SecKeyRef peerPublicKey(void)
{
	SecKeyRef privateKey = peerPrivateKey();

	if (!privateKey)
		return NULL;
	return SecKeyCopyPublicKey(privateKey);
}

int host_peer_public(unsigned char *out, int out_cap)
{
	SecKeyRef publicKey;
	CFDataRef data;
	CFErrorRef error = NULL;
	const UInt8 *bytes;
	CFIndex length;

	if (!out || out_cap < 65)
		return 0;
	publicKey = peerPublicKey();
	if (!publicKey)
		return 0;
	data = SecKeyCopyExternalRepresentation(publicKey, &error);
	CFRelease(publicKey);
	if (error)
		CFRelease(error);
	if (!data)
		return 0;
	bytes = CFDataGetBytePtr(data);
	length = CFDataGetLength(data);
	if (!bytes || length <= 0 || length > out_cap)
	{
		CFRelease(data);
		return 0;
	}
	memcpy(out, bytes, (size_t)length);
	CFRelease(data);
	return (int)length;
}

int host_peer_wrap(const unsigned char *seed, int seed_size, unsigned char *out, int out_cap)
{
	SecKeyRef publicKey;
	CFDataRef clear;
	CFDataRef sealed;
	CFErrorRef error = NULL;
	const UInt8 *bytes;
	CFIndex length;

	if (!seed || seed_size != 32 || !out || out_cap < 32)
		return 0;
	publicKey = peerPublicKey();
	if (!publicKey)
		return 0;
	clear = CFDataCreate(NULL, seed, seed_size);
	sealed = SecKeyCreateEncryptedData(publicKey, kSecKeyAlgorithmECIESEncryptionCofactorVariableIVX963SHA256AESGCM,
		clear, &error);
	CFRelease(publicKey);
	CFRelease(clear);
	if (error)
		CFRelease(error);
	if (!sealed)
		return 0;
	bytes = CFDataGetBytePtr(sealed);
	length = CFDataGetLength(sealed);
	if (!bytes || length <= 0 || length > out_cap)
	{
		CFRelease(sealed);
		return 0;
	}
	memcpy(out, bytes, (size_t)length);
	CFRelease(sealed);
	return (int)length;
}

int host_peer_unwrap(const unsigned char *wrapped, int wrapped_size, unsigned char *seed, int seed_size)
{
	SecKeyRef privateKey;
	CFDataRef sealed;
	CFDataRef clear;
	CFErrorRef error = NULL;
	const UInt8 *bytes;
	CFIndex length;

	if (!wrapped || wrapped_size <= 0 || !seed || seed_size != 32)
		return 0;
	privateKey = peerPrivateKey();
	if (!privateKey)
		return 0;
	sealed = CFDataCreate(NULL, wrapped, wrapped_size);
	clear = SecKeyCreateDecryptedData(privateKey, kSecKeyAlgorithmECIESEncryptionCofactorVariableIVX963SHA256AESGCM,
		sealed, &error);
	CFRelease(sealed);
	if (error)
		CFRelease(error);
	if (!clear)
		return 0;
	bytes = CFDataGetBytePtr(clear);
	length = CFDataGetLength(clear);
	if (!bytes || length != seed_size)
	{
		CFRelease(clear);
		return 0;
	}
	memcpy(seed, bytes, (size_t)seed_size);
	CFRelease(clear);
	return 1;
}

int host_peer_sign(const unsigned char *message, int message_size, unsigned char *out, int out_cap)
{
	SecKeyRef privateKey;
	CFDataRef data;
	CFDataRef signature;
	CFErrorRef error = NULL;
	const UInt8 *bytes;
	CFIndex length;

	if (!message || message_size <= 0 || !out || out_cap < 8)
		return 0;
	privateKey = peerPrivateKey();
	if (!privateKey)
		return 0;
	data = CFDataCreate(NULL, message, message_size);
	signature = SecKeyCreateSignature(privateKey, kSecKeyAlgorithmECDSASignatureMessageX962SHA256, data, &error);
	CFRelease(data);
	if (error)
		CFRelease(error);
	if (!signature)
		return 0;
	bytes = CFDataGetBytePtr(signature);
	length = CFDataGetLength(signature);
	if (!bytes || length <= 0 || length > out_cap)
	{
		CFRelease(signature);
		return 0;
	}
	memcpy(out, bytes, (size_t)length);
	CFRelease(signature);
	return (int)length;
}

int host_peer_verify(const unsigned char *public_key, int public_size, const unsigned char *message, int message_size,
	const unsigned char *signature, int signature_size)
{
	NSDictionary *attributes;
	CFDataRef keyData;
	CFDataRef messageData;
	CFDataRef signatureData;
	CFErrorRef error = NULL;
	SecKeyRef key;
	Boolean ok;

	if (!public_key || public_size <= 0 || !message || message_size <= 0 || !signature || signature_size <= 0)
		return 0;
	attributes = @{
		(__bridge id)kSecAttrKeyType: (__bridge id)kSecAttrKeyTypeECSECPrimeRandom,
		(__bridge id)kSecAttrKeyClass: (__bridge id)kSecAttrKeyClassPublic,
		(__bridge id)kSecAttrKeySizeInBits: @256
	};
	keyData = CFDataCreate(NULL, public_key, public_size);
	key = SecKeyCreateWithData(keyData, (__bridge CFDictionaryRef)attributes, &error);
	CFRelease(keyData);
	if (error)
		CFRelease(error);
	if (!key)
		return 0;
	messageData = CFDataCreate(NULL, message, message_size);
	signatureData = CFDataCreate(NULL, signature, signature_size);
	ok = SecKeyVerifySignature(key, kSecKeyAlgorithmECDSASignatureMessageX962SHA256, messageData, signatureData, NULL);
	CFRelease(key);
	CFRelease(messageData);
	CFRelease(signatureData);
	return ok ? 1 : 0;
}
