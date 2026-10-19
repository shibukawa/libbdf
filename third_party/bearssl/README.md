# BearSSL AES circuit

`src/aes_ct_internal.h` adapts only `br_aes_ct_bitslice_Sbox` and
`br_aes_ct_ortho` from Thomas Pornin's BearSSL `src/symcipher/aes_ct.c`,
under the MIT license reproduced in that header and in `LICENSE`, for words
of 32 bits and, as `aes_ct64.c` has them, of 64 bits.
There is no BearSSL runtime or link dependency.

Source: [upstream aes_ct.c](https://www.bearssl.org/gitweb/?p=BearSSL;a=blob_plain;f=src/symcipher/aes_ct.c;hb=HEAD)
and [aes_ct64.c](https://www.bearssl.org/gitweb/?p=BearSSL;a=blob_plain;f=src/symcipher/aes_ct64.c;hb=HEAD)

Retrieved 2026-10-18. SHA-256 of the complete upstream `aes_ct.c`:
`673c1c82e9a62ba349b806674249b62ed3116482f06f433f6ce71decd9413f0c`.

Changes: renamed functions/macros to the private `bdfi_`/`BDFI_` namespace,
made the functions static, added include guards, and define the circuit
and the transpose once as macros that instantiate them for `uint32_t` and
`uint64_t`. The Boolean circuit and transpose operations are unchanged.
The callers in `src/crypto.c` pack sixteen bytes, use the 32-bit circuit,
and unpack them for the key schedule and for decryption (inverse
substitution applies the inverse affine transform before and after the
forward circuit); encryption slices four blocks over the 64-bit words the
way `aes_ct64.c` does (`br_aes_ct64_interleave_in`/`out`, the key schedule
of `br_aes_ct64_keysched` and `br_aes_ct64_skey_expand`, the ShiftRows and
MixColumns of `aes_ct64_enc.c`), ported into `src/crypto.c`.

See [BearSSL's constant-time implementation notes](https://www.bearssl.org/constanttime.html)
and [FIPS 197](https://csrc.nist.gov/pubs/fips/197/final).
