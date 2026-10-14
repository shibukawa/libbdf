package cbdf

/*
#include <stdlib.h>
#include "bdf_internal.h"
*/
import "C"

import "unsafe"

// The bindings of this file reach the library's internals (the functions of
// src/crypto.c and src/unicode.c), which the tests check against Go's
// standard library and golang.org/x/text one by one.

func SHA256(data []byte) (out [32]byte) {
	C.bdfi_sha256_of(unsafe.Pointer(ptr(data)), C.size_t(len(data)), (*C.uint8_t)(&out[0]))
	return out
}

func HMACSHA256(key, data []byte) (out [32]byte) {
	C.bdfi_hmac_sha256(ptr(key), C.size_t(len(key)), unsafe.Pointer(ptr(data)), C.size_t(len(data)), (*C.uint8_t)(&out[0]))
	return out
}

func PBKDF2(password, salt []byte, iterations uint32) (out [32]byte) {
	C.bdfi_pbkdf2_sha256(unsafe.Pointer(ptr(password)), C.size_t(len(password)), ptr(salt), C.size_t(len(salt)), C.uint32_t(iterations), (*C.uint8_t)(&out[0]))
	return out
}

func HKDF(ikm, salt, info []byte) (out [32]byte) {
	C.bdfi_hkdf_sha256(ptr(ikm), C.size_t(len(ikm)), ptr(salt), C.size_t(len(salt)), unsafe.Pointer(ptr(info)), C.size_t(len(info)), (*C.uint8_t)(&out[0]))
	return out
}

func AESEncrypt(key [32]byte, block [16]byte) (out [16]byte) {
	var a C.bdfi_aes
	C.bdfi_aes256_init(&a, (*C.uint8_t)(&key[0]))
	C.bdfi_aes256_encrypt(&a, (*C.uint8_t)(&block[0]), (*C.uint8_t)(&out[0]))
	return out
}

func AESDecrypt(key [32]byte, block [16]byte) (out [16]byte) {
	var a C.bdfi_aes
	C.bdfi_aes256_init(&a, (*C.uint8_t)(&key[0]))
	C.bdfi_aes256_decrypt(&a, (*C.uint8_t)(&block[0]), (*C.uint8_t)(&out[0]))
	return out
}

// KWUnwrap unwraps a key (RFC 3394); ok is false when it does not unwrap.
func KWUnwrap(kek [32]byte, wrapped []byte) (key []byte, ok bool) {
	out := make([]byte, 64)
	if C.bdfi_aes_kw_unwrap((*C.uint8_t)(&kek[0]), ptr(wrapped), C.size_t(len(wrapped)), (*C.uint8_t)(&out[0])) != 0 {
		return nil, false
	}
	return out[:len(wrapped)-8], true
}

// GCMOpen opens ciphertext and tag sealed with AES-256-GCM.
func GCMOpen(key [32]byte, nonce [12]byte, aad, ct []byte, tag [16]byte) (plain []byte, ok bool) {
	var g C.bdfi_gcm
	C.bdfi_gcm_init(&g, (*C.uint8_t)(&key[0]))
	out := make([]byte, len(ct)+1)
	if C.bdfi_gcm_open(&g, (*C.uint8_t)(&nonce[0]), ptr(aad), C.size_t(len(aad)), ptr(ct), C.size_t(len(ct)), (*C.uint8_t)(&tag[0]), (*C.uint8_t)(&out[0])) != 0 {
		return nil, false
	}
	return out[:len(ct)], true
}

func P256Public(private [32]byte) (out [65]byte, ok bool) {
	return out, C.bdfi_p256_public((*C.uint8_t)(&private[0]), (*C.uint8_t)(&out[0])) == 0
}

// P256ECDH returns the shared secret; rc is 0, -1 for a private key out of
// range, -2 for a peer that is no point of the curve.
func P256ECDH(private [32]byte, peer [65]byte) (out [32]byte, rc int) {
	rc = int(C.bdfi_p256_ecdh((*C.uint8_t)(&private[0]), (*C.uint8_t)(&peer[0]), (*C.uint8_t)(&out[0])))
	return out, rc
}

// NFKC normalises a string with bdfi_nfkc.
func NFKC(s string) string {
	var out *C.char
	var n C.size_t
	b := []byte(s)
	if C.bdfi_nfkc((*C.char)(unsafe.Pointer(ptr(b))), C.size_t(len(b)), &out, &n) != 0 {
		panic("bdfi_nfkc: out of memory")
	}
	defer C.free(unsafe.Pointer(out))
	return goString(out, n)
}
