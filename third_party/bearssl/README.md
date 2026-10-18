# BearSSL AES circuit

`src/aes_ct_internal.h` adapts only `br_aes_ct_bitslice_Sbox` and
`br_aes_ct_ortho` from Thomas Pornin's BearSSL `src/symcipher/aes_ct.c`,
under the MIT license reproduced in that header and in `LICENSE`.
There is no BearSSL runtime or link dependency.

Source: [upstream aes_ct.c](https://www.bearssl.org/gitweb/?p=BearSSL;a=blob_plain;f=src/symcipher/aes_ct.c;hb=HEAD)

Retrieved 2026-10-18. SHA-256 of the complete upstream source file:
`673c1c82e9a62ba349b806674249b62ed3116482f06f433f6ce71decd9413f0c`.

Changes: renamed functions/macros to the private `bdfi_`/`BDFI_` namespace,
made the functions static, added include guards, and undefine the helper
macros. The Boolean circuit and transpose operations are unchanged.
The callers in `src/crypto.c` pack sixteen bytes, use the circuit, and
unpack them; inverse substitution applies the inverse affine transform
before and after the forward circuit.

See [BearSSL's constant-time implementation notes](https://www.bearssl.org/constanttime.html)
and [FIPS 197](https://csrc.nist.gov/pubs/fips/197/final).
