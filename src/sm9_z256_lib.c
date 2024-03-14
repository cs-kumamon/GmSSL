/*
 *  Copyright 2014-2022 The GmSSL Project. All Rights Reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the License); you may
 *  not use this file except in compliance with the License.
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <gmssl/mem.h>
#include <gmssl/sm3.h>
#include <gmssl/sm9_z256.h>
#include <gmssl/asn1.h>
#include <gmssl/error.h>


extern const sm9_z256_t SM9_Z256_ZERO;
extern const sm9_z256_t SM9_Z256_N;
extern const SM9_Z256_POINT *SM9_Z256_MONT_P1;
extern const SM9_Z256_TWIST_POINT *SM9_Z256_MONT_P2;


int sm9_signature_to_der(const SM9_SIGNATURE *sig, uint8_t **out, size_t *outlen)
{
	uint8_t hbuf[32];
	uint8_t Sbuf[65];
	size_t len = 0;

	sm9_z256_fn_to_bytes(sig->h, hbuf);
	sm9_z256_point_to_uncompressed_octets(&sig->S, Sbuf);

	if (asn1_octet_string_to_der(hbuf, sizeof(hbuf), NULL, &len) != 1
		|| asn1_bit_octets_to_der(Sbuf, sizeof(Sbuf), NULL, &len) != 1
		|| asn1_sequence_header_to_der(len, out, outlen) != 1
		|| asn1_octet_string_to_der(hbuf, sizeof(hbuf), out, outlen) != 1
		|| asn1_bit_octets_to_der(Sbuf, sizeof(Sbuf), out, outlen) != 1) {
		error_print();
		return -1;
	}
	return 1;
}

int sm9_signature_from_der(SM9_SIGNATURE *sig, const uint8_t **in, size_t *inlen)
{
	int ret;
	const uint8_t *d;
	size_t dlen;
	const uint8_t *h;
	size_t hlen;
	const uint8_t *S;
	size_t Slen;

	if ((ret = asn1_sequence_from_der(&d, &dlen, in, inlen)) != 1) {
		if (ret < 0) error_print();
		return ret;
	}
	if (asn1_octet_string_from_der(&h, &hlen, &d, &dlen) != 1
		|| asn1_bit_octets_from_der(&S, &Slen, &d, &dlen) != 1
		|| asn1_check(hlen == 32) != 1
		|| asn1_check(Slen == 65) != 1
		|| asn1_length_is_zero(dlen) != 1) {
		error_print();
		return -1;
	}
	if (sm9_z256_fn_from_bytes(sig->h, h) != 1
		|| sm9_z256_point_from_uncompressed_octets(&sig->S, S) != 1) {
		error_print();
		return -1;
	}
	return 1;
}

int sm9_sign_init(SM9_SIGN_CTX *ctx)
{
	const uint8_t prefix[1] = { SM9_HASH2_PREFIX };
	sm3_init(&ctx->sm3_ctx);
	sm3_update(&ctx->sm3_ctx, prefix, sizeof(prefix));
	return 1;
}

int sm9_sign_update(SM9_SIGN_CTX *ctx, const uint8_t *data, size_t datalen)
{
	sm3_update(&ctx->sm3_ctx, data, datalen);
	return 1;
}

int sm9_sign_finish(SM9_SIGN_CTX *ctx, const SM9_SIGN_KEY *key, uint8_t *sig, size_t *siglen)
{
	SM9_SIGNATURE signature;

	if (sm9_do_sign(key, &ctx->sm3_ctx, &signature) != 1) {
		error_print();
		return -1;
	}
	*siglen = 0;
	if (sm9_signature_to_der(&signature, &sig, siglen) != 1) {
		error_print();
		return -1;
	}
	return 1;
}

int sm9_do_sign(const SM9_SIGN_KEY *key, const SM3_CTX *sm3_ctx, SM9_SIGNATURE *sig)
{
	sm9_z256_t r;
	sm9_z256_fp12 g;
	uint8_t wbuf[32 * 12];
	SM3_CTX ctx = *sm3_ctx;
	SM3_CTX tmp_ctx;
	uint8_t ct1[4] = {0,0,0,1};
	uint8_t ct2[4] = {0,0,0,2};
	uint8_t Ha[64];

	// A1: g = e(P1, Ppubs)
	sm9_z256_pairing(g, &key->Ppubs, SM9_Z256_MONT_P1);

	do {
		// A2: rand r in [1, N-1]
		if (sm9_z256_fn_rand(r) != 1) {
			error_print();
			return -1;
		}
		
		// Only for testing
		//sm9_z256_from_hex(r, "00033C8616B06704813203DFD00965022ED15975C662337AED648835DC4B1CBE");

		// A3: w = g^r
		sm9_z256_fp12_pow(g, g, r);
		sm9_z256_fp12_to_bytes(g, wbuf);

		// A4: h = H2(M || w, N)
		sm3_update(&ctx, wbuf, sizeof(wbuf));
		tmp_ctx = ctx;
		sm3_update(&ctx, ct1, sizeof(ct1));
		sm3_finish(&ctx, Ha);
		sm3_update(&tmp_ctx, ct2, sizeof(ct2));
		sm3_finish(&tmp_ctx, Ha + 32);
		sm9_z256_fn_from_hash(sig->h, Ha);

		// A5: l = (r - h) mod N, if l = 0, goto A2
		sm9_z256_fn_sub(r, r, sig->h);

	} while (sm9_z256_fn_is_zero(r));

	// A6: S = l * dsA
	sm9_z256_point_mul(&sig->S, r, &key->ds);

	gmssl_secure_clear(&r, sizeof(r));
	gmssl_secure_clear(&g, sizeof(g));
	gmssl_secure_clear(wbuf, sizeof(wbuf));
	gmssl_secure_clear(&tmp_ctx, sizeof(tmp_ctx));
	gmssl_secure_clear(Ha, sizeof(Ha));

	return 1;
}

int sm9_verify_init(SM9_SIGN_CTX *ctx)
{
	const uint8_t prefix[1] = { SM9_HASH2_PREFIX };
	sm3_init(&ctx->sm3_ctx);
	sm3_update(&ctx->sm3_ctx, prefix, sizeof(prefix));
	return 1;
}

int sm9_verify_update(SM9_SIGN_CTX *ctx, const uint8_t *data, size_t datalen)
{
	sm3_update(&ctx->sm3_ctx, data, datalen);
	return 1;
}

int sm9_verify_finish(SM9_SIGN_CTX *ctx, const uint8_t *sig, size_t siglen,
	const SM9_SIGN_MASTER_KEY *mpk, const char *id, size_t idlen)
{
	int ret;
	SM9_SIGNATURE signature;

	if (sm9_signature_from_der(&signature, &sig, &siglen) != 1
		|| asn1_length_is_zero(siglen) != 1) {
		error_print();
		return -1;
	}

	if ((ret = sm9_do_verify(mpk, id, idlen, &ctx->sm3_ctx, &signature)) < 0) {
		error_print();
		return -1;
	}
	return ret;
}

int sm9_do_verify(const SM9_SIGN_MASTER_KEY *mpk, const char *id, size_t idlen,
	const SM3_CTX *sm3_ctx, const SM9_SIGNATURE *sig)
{
	sm9_z256_t h1;
	sm9_z256_t h2;
	sm9_z256_fp12 g;
	sm9_z256_fp12 t;
	sm9_z256_fp12 u;
	sm9_z256_fp12 w;
	SM9_Z256_TWIST_POINT P;
	uint8_t wbuf[32 * 12];
	SM3_CTX ctx = *sm3_ctx;
	SM3_CTX tmp_ctx;
	uint8_t ct1[4] = {0,0,0,1};
	uint8_t ct2[4] = {0,0,0,2};
	uint8_t Ha[64];

	// B1: check h in [1, N-1]

	// B2: check S in G1

	// B3: g = e(P1, Ppubs)
	sm9_z256_pairing(g, &mpk->Ppubs, SM9_Z256_MONT_P1);

	// B4: t = g^h
	sm9_z256_fp12_pow(t, g, sig->h);

	// B5: h1 = H1(ID || hid, N)
	sm9_z256_hash1(h1, id, idlen, SM9_HID_SIGN);

	// B6: P = h1 * P2 + Ppubs
	sm9_z256_twist_point_mul_generator(&P, h1);
	sm9_z256_twist_point_add_full(&P, &P, &mpk->Ppubs);

	// B7: u = e(S, P)
	sm9_z256_pairing(u, &P, &sig->S);

	// B8: w = u * t
	sm9_z256_fp12_mul(w, u, t);
	sm9_z256_fp12_to_bytes(w, wbuf);

	// B9: h2 = H2(M || w, N), check h2 == h
	sm3_update(&ctx, wbuf, sizeof(wbuf));
	tmp_ctx = ctx;
	sm3_update(&ctx, ct1, sizeof(ct1));
	sm3_finish(&ctx, Ha);
	sm3_update(&tmp_ctx, ct2, sizeof(ct2));
	sm3_finish(&tmp_ctx, Ha + 32);
	sm9_z256_fn_from_hash(h2, Ha);
	if (sm9_z256_fn_equ(h2, sig->h) != 1) {
		return 0;
	}

	return 1;
}

int sm9_kem_encrypt(const SM9_ENC_MASTER_KEY *mpk, const char *id, size_t idlen,
	size_t klen, uint8_t *kbuf, SM9_Z256_POINT *C)
{
	sm9_z256_t r;
	sm9_z256_fp12 w;
	uint8_t wbuf[32 * 12];
	uint8_t cbuf[65];
	SM3_KDF_CTX kdf_ctx;

	// A1: Q = H1(ID||hid,N) * P1 + Ppube
	sm9_z256_hash1(r, id, idlen, SM9_HID_ENC);
	sm9_z256_point_mul(C, r, SM9_Z256_MONT_P1);
	sm9_z256_point_add(C, C, &mpk->Ppube);

	do {
		// A2: rand r in [1, N-1]
		if (sm9_z256_fn_rand(r) != 1) {
			error_print();
			return -1;
		}

		// A3: C1 = r * Q
		sm9_z256_point_mul(C, r, C);
		sm9_z256_point_to_uncompressed_octets(C, cbuf);

		// A4: g = e(Ppube, P2)
		sm9_z256_pairing(w, SM9_Z256_MONT_P2, &mpk->Ppube);

		// A5: w = g^r
		sm9_z256_fp12_pow(w, w, r);
		sm9_z256_fp12_to_bytes(w, wbuf);

		// A6: K = KDF(C || w || ID_B, klen), if K == 0, goto A2
		sm3_kdf_init(&kdf_ctx, klen);
		sm3_kdf_update(&kdf_ctx, cbuf + 1, 64);
		sm3_kdf_update(&kdf_ctx, wbuf, sizeof(wbuf));
		sm3_kdf_update(&kdf_ctx, (uint8_t *)id, idlen);
		sm3_kdf_finish(&kdf_ctx, kbuf);

	} while (mem_is_zero(kbuf, klen) == 1);

	gmssl_secure_clear(&r, sizeof(r));
	gmssl_secure_clear(&w, sizeof(w));
	gmssl_secure_clear(wbuf, sizeof(wbuf));
	gmssl_secure_clear(&kdf_ctx, sizeof(kdf_ctx));

	// A7: output (K, C)
	return 1;
}

int sm9_kem_decrypt(const SM9_ENC_KEY *key, const char *id, size_t idlen, const SM9_Z256_POINT *C,
	size_t klen, uint8_t *kbuf)
{
	sm9_z256_fp12 w;
	uint8_t wbuf[32 * 12];
	uint8_t cbuf[65];
	SM3_KDF_CTX kdf_ctx;

	// B1: check C in G1
	sm9_z256_point_to_uncompressed_octets(C, cbuf);

	// B2: w = e(C, de);
	sm9_z256_pairing(w, &key->de, C);
	sm9_z256_fp12_to_bytes(w, wbuf);

	// B3: K = KDF(C || w || ID, klen)
	sm3_kdf_init(&kdf_ctx, klen);
	sm3_kdf_update(&kdf_ctx, cbuf + 1, 64);
	sm3_kdf_update(&kdf_ctx, wbuf, sizeof(wbuf));
	sm3_kdf_update(&kdf_ctx, (uint8_t *)id, idlen);
	sm3_kdf_finish(&kdf_ctx, kbuf);

	if (mem_is_zero(kbuf, klen)) {
		error_print();
		return -1;
	}

	gmssl_secure_clear(&w, sizeof(w));
	gmssl_secure_clear(wbuf, sizeof(wbuf));
	gmssl_secure_clear(&kdf_ctx, sizeof(kdf_ctx));

	// B4: output K
	return 1;
}

int sm9_do_encrypt(const SM9_ENC_MASTER_KEY *mpk, const char *id, size_t idlen,
	const uint8_t *in, size_t inlen,
	SM9_Z256_POINT *C1, uint8_t *c2, uint8_t c3[SM3_HMAC_SIZE])
{
	SM3_HMAC_CTX hmac_ctx;
	uint8_t K[SM9_MAX_PLAINTEXT_SIZE + 32];

	if (sm9_kem_encrypt(mpk, id, idlen, sizeof(K), K, C1) != 1) {
		error_print();
		return -1;
	}
	gmssl_memxor(c2, K, in, inlen);

	//sm3_hmac(K + inlen, 32, c2, inlen, c3);
	sm3_hmac_init(&hmac_ctx, K + inlen, SM3_HMAC_SIZE);
	sm3_hmac_update(&hmac_ctx, c2, inlen);
	sm3_hmac_finish(&hmac_ctx, c3);
	gmssl_secure_clear(&hmac_ctx, sizeof(hmac_ctx));
	return 1;
}

int sm9_do_decrypt(const SM9_ENC_KEY *key, const char *id, size_t idlen,
	const SM9_Z256_POINT *C1, const uint8_t *c2, size_t c2len, const uint8_t c3[SM3_HMAC_SIZE],
	uint8_t *out)
{
	SM3_HMAC_CTX hmac_ctx;
	uint8_t k[SM9_MAX_PLAINTEXT_SIZE + SM3_HMAC_SIZE];
	uint8_t mac[SM3_HMAC_SIZE];

	if (c2len > SM9_MAX_PLAINTEXT_SIZE) {
		error_print();
		return -1;
	}

	if (sm9_kem_decrypt(key, id, idlen, C1, sizeof(k), k) != 1) {
		error_print();
		return -1;
	}
	//sm3_hmac(k + c2len, SM3_HMAC_SIZE, c2, c2len, mac);
	sm3_hmac_init(&hmac_ctx, k + c2len, SM3_HMAC_SIZE);
	sm3_hmac_update(&hmac_ctx, c2, c2len);
	sm3_hmac_finish(&hmac_ctx, mac);
	gmssl_secure_clear(&hmac_ctx, sizeof(hmac_ctx));

	if (gmssl_secure_memcmp(c3, mac, sizeof(mac)) != 0) {
		error_print();
		return -1;
	}
	gmssl_memxor(out, k, c2, c2len);
	return 1;
}

#define SM9_ENC_TYPE_XOR	0
#define SM9_ENC_TYPE_ECB	1
#define SM9_ENC_TYPE_CBC	2
#define SM9_ENC_TYPE_OFB	4
#define SM9_ENC_TYPE_CFB	8

/*
SM9Cipher ::= SEQUENCE {
	EnType		INTEGER, -- 0 for XOR
	C1		BIT STRING, -- uncompressed octets of ECPoint
	C3		OCTET STRING, -- 32 bytes HMAC-SM3 tag
	CipherText	OCTET STRING,
}
*/
int sm9_ciphertext_to_der(const SM9_Z256_POINT *C1, const uint8_t *c2, size_t c2len,
	const uint8_t c3[SM3_HMAC_SIZE], uint8_t **out, size_t *outlen)
{
	int en_type = SM9_ENC_TYPE_XOR;
	uint8_t c1[65];
	size_t len = 0;

	if (sm9_z256_point_to_uncompressed_octets(C1, c1) != 1) {
		error_print();
		return -1;
	}
	if (asn1_int_to_der(en_type, NULL, &len) != 1
		|| asn1_bit_octets_to_der(c1, sizeof(c1), NULL, &len) != 1
		|| asn1_octet_string_to_der(c3, SM3_HMAC_SIZE, NULL, &len) != 1
		|| asn1_octet_string_to_der(c2, c2len, NULL, &len) != 1
		|| asn1_sequence_header_to_der(len, out, outlen) != 1
		|| asn1_int_to_der(en_type, out, outlen) != 1
		|| asn1_bit_octets_to_der(c1, sizeof(c1), out, outlen) != 1
		|| asn1_octet_string_to_der(c3, SM3_HMAC_SIZE, out, outlen) != 1
		|| asn1_octet_string_to_der(c2, c2len, out, outlen) != 1) {
		error_print();
		return -1;
	}
	return 1;
}

int sm9_ciphertext_from_der(SM9_Z256_POINT *C1, const uint8_t **c2, size_t *c2len,
	const uint8_t **c3, const uint8_t **in, size_t *inlen)
{
	int ret;
	const uint8_t *d;
	size_t dlen;
	int en_type;
	const uint8_t *c1;
	size_t c1len;
	size_t c3len;

	if ((ret = asn1_sequence_from_der(&d, &dlen, in, inlen)) != 1) {
		if (ret < 0) error_print();
		return ret;
	}
	if (asn1_int_from_der(&en_type, &d, &dlen) != 1
		|| asn1_bit_octets_from_der(&c1, &c1len, &d, &dlen) != 1
		|| asn1_octet_string_from_der(c3, &c3len, &d, &dlen) != 1
		|| asn1_octet_string_from_der(c2, c2len, &d, &dlen) != 1
		|| asn1_length_is_zero(dlen) != 1) {
		error_print();
		return -1;
	}
	if (en_type != SM9_ENC_TYPE_XOR) {
		error_print();
		return -1;
	}
	if (c1len != 65) {
		error_print();
		return -1;
	}
	if (c3len != SM3_HMAC_SIZE) {
		error_print();
		return -1;
	}
	if (sm9_z256_point_from_uncompressed_octets(C1, c1) != 1) {
		error_print();
		return -1;
	}
	return 1;
}

int sm9_encrypt(const SM9_ENC_MASTER_KEY *mpk, const char *id, size_t idlen,
	const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen)
{
	SM9_Z256_POINT C1;
	uint8_t c2[SM9_MAX_PLAINTEXT_SIZE];
	uint8_t c3[SM3_HMAC_SIZE];

	if (inlen > SM9_MAX_PLAINTEXT_SIZE) {
		error_print();
		return -1;
	}

	if (sm9_do_encrypt(mpk, id, idlen, in, inlen, &C1, c2, c3) != 1) {
		error_print();
		return -1;
	}
	*outlen = 0;
	if (sm9_ciphertext_to_der(&C1, c2, inlen, c3, &out, outlen) != 1) { // FIXME: when out == NULL	
		error_print();
		return -1;
	}
	return 1;
}

int sm9_decrypt(const SM9_ENC_KEY *key, const char *id, size_t idlen,
	const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen)
{
	SM9_Z256_POINT C1;
	const uint8_t *c2;
	size_t c2len;
	const uint8_t *c3;

	if (sm9_ciphertext_from_der(&C1, &c2, &c2len, &c3, &in, &inlen) != 1
		|| asn1_length_is_zero(inlen) != 1) {
		error_print();
		return -1;
	}
	*outlen = c2len;
	if (!out) {
		return 1;
	}
	if (sm9_do_decrypt(key, id, idlen, &C1, c2, c2len, c3, out) != 1) {
		error_print();
		return -1;
	}
	return 1;
}



int sm9_exch_step_1A(const SM9_EXCH_MASTER_KEY *mpk, const char *idB, size_t idBlen, SM9_Z256_POINT *RA, sm9_z256_t rA)
{
	// A1: Q = H1(ID_B||hid,N) * P1 + Ppube
	sm9_z256_hash1(rA, idB, idBlen, SM9_HID_EXCH);
	sm9_z256_point_mul(RA, rA, SM9_Z256_MONT_P1);
	sm9_z256_point_add(RA, RA, &mpk->Ppube);

	// A2: rand rA in [1, N-1]
	if (sm9_z256_fn_rand(rA) != 1) {
		error_print();
		return -1;
	}
	// Only for testing
	sm9_z256_from_hex(rA, "00005879DD1D51E175946F23B1B41E93BA31C584AE59A426EC1046A4D03B06C8");

	// A3: RA = rA * Q
	sm9_z256_point_mul(RA, rA, RA);

	// A4: Output RA, save rA
	return 1;
}

int sm9_exch_step_1B(const SM9_EXCH_MASTER_KEY *mpk, const char *idA, size_t idAlen, const char *idB, size_t idBlen,
	const SM9_EXCH_KEY *key, const SM9_Z256_POINT *RA, SM9_Z256_POINT *RB, uint8_t *sk, size_t klen)
{
	sm9_z256_t rB;
	sm9_z256_fp12 G1, G2, G3;
	uint8_t g1[32 * 12], g2[32 * 12], g3[32 * 12];
	uint8_t ta[65], tb[65];
	SM3_KDF_CTX kdf_ctx;

	// B1: Q = H1(ID_A||hid,N) * P1 + Ppube
	sm9_z256_hash1(rB, idA, idAlen, SM9_HID_EXCH);
	sm9_z256_point_mul(RB, rB, SM9_Z256_MONT_P1);
	sm9_z256_point_add(RB, RB, &mpk->Ppube);

	do {
		// B2: rand rB in [1, N-1]
		if (sm9_z256_fn_rand(rB) != 1) {
			error_print();
			return -1;
		}
		// Only for testing
		sm9_z256_from_hex(rB, "00018B98C44BEF9F8537FB7D071B2C928B3BC65BD3D69E1EEE213564905634FE");

		// B3: RB = rB * Q
		sm9_z256_point_mul(RB, rB, RB);

		// B4: check RA on curve; G1 = e(RA, deB), G2 = e(Ppube, P2) ^ rB, G3 = G1 ^ rB
		if (!sm9_z256_point_is_on_curve(RA)) {
			error_print();
			return -1;
		}
		sm9_z256_pairing(G1, &key->de, RA);
		sm9_z256_pairing(G2, SM9_Z256_MONT_P2, &mpk->Ppube);
		sm9_z256_fp12_pow(G2, G2, rB);
		sm9_z256_fp12_pow(G3, G1, rB);

		sm9_z256_point_to_uncompressed_octets(RA, ta);
		sm9_z256_point_to_uncompressed_octets(RB, tb);
		sm9_z256_fp12_to_bytes(G1, g1);
		sm9_z256_fp12_to_bytes(G2, g2);
		sm9_z256_fp12_to_bytes(G3, g3);

		// B5: sk = KDF(ID_A || ID_B || RA || RB || g1 || g2 || g3, klen)
		sm3_kdf_init(&kdf_ctx, klen);
		sm3_kdf_update(&kdf_ctx, idA, idAlen);
		sm3_kdf_update(&kdf_ctx, idB, idBlen);
		sm3_kdf_update(&kdf_ctx, ta + 1, 64);
		sm3_kdf_update(&kdf_ctx, tb + 1, 64);
		sm3_kdf_update(&kdf_ctx, g1, sizeof(g1));
		sm3_kdf_update(&kdf_ctx, g2, sizeof(g2));
		sm3_kdf_update(&kdf_ctx, g3, sizeof(g3));
		sm3_kdf_finish(&kdf_ctx, sk);

	} while (mem_is_zero(sk, klen) == 1);

	// B6: SB = Hash(0x82 || g1 || Hash(g2 || g3 || ID_A || ID_B || RA || RB)) [optional]

	gmssl_secure_clear(&rB, sizeof(rB));
	gmssl_secure_clear(&G1, sizeof(G1));
	gmssl_secure_clear(&G2, sizeof(G2));
	gmssl_secure_clear(&G3, sizeof(G3));
	gmssl_secure_clear(g1, sizeof(g1));
	gmssl_secure_clear(g2, sizeof(g2));
	gmssl_secure_clear(g3, sizeof(g3));
	gmssl_secure_clear(ta, sizeof(ta));
	gmssl_secure_clear(tb, sizeof(tb));
	gmssl_secure_clear(&kdf_ctx, sizeof(kdf_ctx));

	// B7: Output RB
	return 1;
}

int sm9_exch_step_2A(const SM9_EXCH_MASTER_KEY *mpk, const char *idA, size_t idAlen, const char *idB, size_t idBlen,
	const SM9_EXCH_KEY *key, const sm9_z256_t rA, const SM9_Z256_POINT *RA, const SM9_Z256_POINT *RB, uint8_t *sk, size_t klen)
{
	sm9_z256_t r;
	sm9_z256_fp12 G1, G2, G3;
	uint8_t g1[32 * 12], g2[32 * 12], g3[32 * 12];
	uint8_t ta[65], tb[65];
	SM3_KDF_CTX kdf_ctx;

	do {
		// A5: check RB on curve; G1 = e(Ppube, P2) ^ rA, G2 = e(RB, deA),  G3 = G2 ^ rA
		if (!sm9_z256_point_is_on_curve(RB)) {
			error_print();
			return -1;
		}
		sm9_z256_pairing(G1, SM9_Z256_MONT_P2, &mpk->Ppube);
		sm9_z256_fp12_pow(G1, G1, rA);
		sm9_z256_pairing(G2, &key->de, RB);
		sm9_z256_fp12_pow(G3, G2, rA);

		sm9_z256_point_to_uncompressed_octets(RA, ta);
		sm9_z256_point_to_uncompressed_octets(RB, tb);
		sm9_z256_fp12_to_bytes(G1, g1);
		sm9_z256_fp12_to_bytes(G2, g2);
		sm9_z256_fp12_to_bytes(G3, g3);

		// A6: S1 = Hash(0x82 || g1 || Hash(g2 || g3 || ID_A || ID_B || RA || RB)), check S1 = SB [optional]

		// A7: sk = KDF(ID_A || ID_B || RA || RB || g1 || g2 || g3, klen) 
		sm3_kdf_init(&kdf_ctx, klen);
		sm3_kdf_update(&kdf_ctx, idA, idAlen);
		sm3_kdf_update(&kdf_ctx, idB, idBlen);
		sm3_kdf_update(&kdf_ctx, ta + 1, 64);
		sm3_kdf_update(&kdf_ctx, tb + 1, 64);
		sm3_kdf_update(&kdf_ctx, g1, sizeof(g1));
		sm3_kdf_update(&kdf_ctx, g2, sizeof(g2));
		sm3_kdf_update(&kdf_ctx, g3, sizeof(g3));
		sm3_kdf_finish(&kdf_ctx, sk);

	} while (mem_is_zero(sk, klen) == 1);

	// A8: SA = Hash(0x83 || g1 || Hash(g2 || g3 || ID_A || ID_B || RA || RB)) [optional]

	gmssl_secure_clear(&r, sizeof(r));
	gmssl_secure_clear(&G1, sizeof(G1));
	gmssl_secure_clear(&G2, sizeof(G2));
	gmssl_secure_clear(&G3, sizeof(G3));
	gmssl_secure_clear(g1, sizeof(g1));
	gmssl_secure_clear(g2, sizeof(g2));
	gmssl_secure_clear(g3, sizeof(g3));
	gmssl_secure_clear(ta, sizeof(ta));
	gmssl_secure_clear(tb, sizeof(tb));
	gmssl_secure_clear(&kdf_ctx, sizeof(kdf_ctx));

	return 1;
}

int sm9_exch_step_2B()
{
	// B8: S2 = Hash(0x83 || g1 || Hash(g2 || g3 || ID_A || ID_B || RA || RB)), check S2 = SA [optional]
	return 1;
}
