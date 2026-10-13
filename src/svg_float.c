/*
 * Decimal and hexadecimal numbers to float64: a port of ParseFloat of Go's
 * strconv (internal/strconv/atof.go, uscale.go and decimal.go of Go 1.27,
 * BSD license, see third_party/go-text/LICENSE), which the reference reads
 * the numbers of an SVG document with. It is the nearest float64, ties to
 * even, by integer arithmetic alone, so that it is the same number on every
 * platform and under every locale; and it is Go's answer where Go's is not
 * the nearest one (a number of more than 800 digits before its point, or
 * with an exponent of more than five digits).
 *
 * What is left out of Go's code is its short cut through floating point
 * arithmetic (atof64exact): the scaling below gives the same number.
 */
#include <string.h>

#include "svg_internal.h"

/* ---- The powers of ten (pow10tab.go) -------------------------------------- */

/* A power of ten as 128 bits, hi<<64 - lo, scaled so that the high bit is
 * set (pmHiLo). */
typedef struct bdfi_svg_float_pow {
  uint64_t hi, lo;
} bdfi_svg_float_pow;

/* The powers atof64 scales with: it answers those above and below itself. */
#define BDFI_SVG_FLOAT_POW_MIN (-345)
#define BDFI_SVG_FLOAT_POW_MAX 310

/* The table is written by gen/main.go from pow10tab.go of Go 1.27. */
static const bdfi_svg_float_pow
    bdfi_svg_float_pows[BDFI_SVG_FLOAT_POW_MAX - BDFI_SVG_FLOAT_POW_MIN + 1] = {
        /* clang-format off */
    /* BEGIN GENERATED: pow10 */
    {0xf4b0769e47eb5a79ULL, 0x19586848ad6f6106ULL}, /* 1e-345 */
    {0x98ee4a22ecf3188cULL, 0x6fd7412d6c659ca3ULL}, /* 1e-344 */
    {0xbf29dcaba82fdeafULL, 0x8bcd1178c77f03ccULL}, /* 1e-343 */
    {0xeef453d6923bd65bULL, 0xeec055d6f95ec4c0ULL}, /* 1e-342 */
    {0x9558b4661b6565f9ULL, 0xb53835a65bdb3af8ULL}, /* 1e-341 */
    {0xbaaee17fa23ebf77ULL, 0xa286430ff2d209b6ULL}, /* 1e-340 */
    {0xe95a99df8ace6f54ULL, 0x0b27d3d3ef868c23ULL}, /* 1e-339 */
    {0x91d8a02bb6c10595ULL, 0x86f8e46475b41796ULL}, /* 1e-338 */
    {0xb64ec836a47146faULL, 0x68b71d7d93211d7bULL}, /* 1e-337 */
    {0xe3e27a444d8d98b8ULL, 0x02e4e4dcf7e964daULL}, /* 1e-336 */
    {0x8e6d8c6ab0787f73ULL, 0x01cf0f0a1af1df08ULL}, /* 1e-335 */
    {0xb208ef855c969f50ULL, 0x4242d2cca1ae56caULL}, /* 1e-334 */
    {0xde8b2b66b3bc4724ULL, 0x52d3877fca19ec7dULL}, /* 1e-333 */
    {0x8b16fb203055ac77ULL, 0xb3c434afde5033ceULL}, /* 1e-332 */
    {0xaddcb9e83c6b1794ULL, 0x20b541dbd5e440c2ULL}, /* 1e-331 */
    {0xd953e8624b85dd79ULL, 0x28e29252cb5d50f2ULL}, /* 1e-330 */
    {0x87d4713d6f33aa6cULL, 0x798d9b73bf1a5297ULL}, /* 1e-329 */
    {0xa9c98d8ccb009507ULL, 0x97f10250aee0e73dULL}, /* 1e-328 */
    {0xd43bf0effdc0ba49ULL, 0xfded42e4da99210dULL}, /* 1e-327 */
    {0x84a57695fe98746eULL, 0xfeb449cf089fb4a8ULL}, /* 1e-326 */
    {0xa5ced43b7e3e9189ULL, 0xbe615c42cac7a1d2ULL}, /* 1e-325 */
    {0xcf42894a5dce35ebULL, 0xadf9b3537d798a46ULL}, /* 1e-324 */
    {0x818995ce7aa0e1b3ULL, 0x8cbc10142e6bf66cULL}, /* 1e-323 */
    {0xa1ebfb4219491a20ULL, 0xefeb14193a06f407ULL}, /* 1e-322 */
    {0xca66fa129f9b60a7ULL, 0x2be5d91f8888b109ULL}, /* 1e-321 */
    {0xfd00b897478238d1ULL, 0x76df4f676aaadd4bULL}, /* 1e-320 */
    {0x9e20735e8cb16383ULL, 0xaa4b91a0a2aaca4fULL}, /* 1e-319 */
    {0xc5a890362fddbc63ULL, 0x14de7608cb557ce2ULL}, /* 1e-318 */
    {0xf712b443bbd52b7cULL, 0x5a16138afe2adc1bULL}, /* 1e-317 */
    {0x9a6bb0aa55653b2eULL, 0xb84dcc36dedac991ULL}, /* 1e-316 */
    {0xc1069cd4eabe89f9ULL, 0x66613f4496917bf5ULL}, /* 1e-315 */
    {0xf148440a256e2c77ULL, 0x3ff98f15bc35daf2ULL}, /* 1e-314 */
    {0x96cd2a865764dbcbULL, 0xc7fbf96d95a1a8d7ULL}, /* 1e-313 */
    {0xbc807527ed3e12bdULL, 0x39faf7c8fb0a130dULL}, /* 1e-312 */
    {0xeba09271e88d976cULL, 0x0879b5bb39cc97d1ULL}, /* 1e-311 */
    {0x93445b8731587ea4ULL, 0x854c1195041fdee2ULL}, /* 1e-310 */
    {0xb8157268fdae9e4dULL, 0xa69f15fa4527d69bULL}, /* 1e-309 */
    {0xe61acf033d1a45e0ULL, 0x9046db78d671cc42ULL}, /* 1e-308 */
    {0x8fd0c16206306bacULL, 0x5a2c492b86071fa9ULL}, /* 1e-307 */
    {0xb3c4f1ba87bc8697ULL, 0x70b75b766788e793ULL}, /* 1e-306 */
    {0xe0b62e2929aba83dULL, 0xcce53254016b2178ULL}, /* 1e-305 */
    {0x8c71dcd9ba0b4926ULL, 0x600f3f7480e2f4ebULL}, /* 1e-304 */
    {0xaf8e5410288e1b70ULL, 0xf8130f51a11bb226ULL}, /* 1e-303 */
    {0xdb71e91432b1a24bULL, 0x3617d32609629eafULL}, /* 1e-302 */
    {0x892731ac9faf056fULL, 0x41cee3f7c5dda32dULL}, /* 1e-301 */
    {0xab70fe17c79ac6cbULL, 0x92429cf5b7550bf9ULL}, /* 1e-300 */
    {0xd64d3d9db981787eULL, 0xf6d34433252a4ef7ULL}, /* 1e-299 */
    {0x85f0468293f0eb4fULL, 0xda440a9ff73a715aULL}, /* 1e-298 */
    {0xa76c582338ed2622ULL, 0x50d50d47f5090db1ULL}, /* 1e-297 */
    {0xd1476e2c07286fabULL, 0xe50a5099f24b511eULL}, /* 1e-296 */
    {0x82cca4db847945cbULL, 0xaf267260376f12b2ULL}, /* 1e-295 */
    {0xa37fce126597973dULL, 0x1af00ef8454ad75fULL}, /* 1e-294 */
    {0xcc5fc196fefd7d0dULL, 0xe1ac12b6569d8d37ULL}, /* 1e-293 */
    {0xff77b1fcbebcdc50ULL, 0xda171763ec44f085ULL}, /* 1e-292 */
    {0x9faacf3df73609b2ULL, 0x884e6e9e73ab1653ULL}, /* 1e-291 */
    {0xc795830d75038c1eULL, 0x2a620a461095dbe8ULL}, /* 1e-290 */
    {0xf97ae3d0d2446f26ULL, 0xb4fa8cd794bb52e2ULL}, /* 1e-289 */
    {0x9becce62836ac578ULL, 0xb11c9806bcf513cdULL}, /* 1e-288 */
    {0xc2e801fb244576d6ULL, 0xdd63be086c3258c0ULL}, /* 1e-287 */
    {0xf3a20279ed56d48bULL, 0x94bcad8a873eeef0ULL}, /* 1e-286 */
    {0x9845418c345644d7ULL, 0x7cf5ec7694875556ULL}, /* 1e-285 */
    {0xbe5691ef416bd60dULL, 0xdc33679439a92aacULL}, /* 1e-284 */
    {0xedec366b11c6cb90ULL, 0xd340417948137557ULL}, /* 1e-283 */
    {0x94b3a202eb1c3f3aULL, 0x840828ebcd0c2956ULL}, /* 1e-282 */
    {0xb9e08a83a5e34f08ULL, 0x250a3326c04f33acULL}, /* 1e-281 */
    {0xe858ad248f5c22caULL, 0x2e4cbff070630097ULL}, /* 1e-280 */
    {0x91376c36d99995bfULL, 0xdceff7f6463de05eULL}, /* 1e-279 */
    {0xb58547448ffffb2eULL, 0x542bf5f3d7cd5875ULL}, /* 1e-278 */
    {0xe2e69915b3fff9faULL, 0xe936f370cdc0ae93ULL}, /* 1e-277 */
    {0x8dd01fad907ffc3cULL, 0x51c2582680986d1cULL}, /* 1e-276 */
    {0xb1442798f49ffb4bULL, 0x6632ee3020be8863ULL}, /* 1e-275 */
    {0xdd95317f31c7fa1eULL, 0xbfbfa9bc28ee2a7cULL}, /* 1e-274 */
    {0x8a7d3eef7f1cfc53ULL, 0xb7d7ca159994da8dULL}, /* 1e-273 */
    {0xad1c8eab5ee43b67ULL, 0x25cdbc9afffa1130ULL}, /* 1e-272 */
    {0xd863b256369d4a41ULL, 0x6f412bc1bff8957dULL}, /* 1e-271 */
    {0x873e4f75e2224e69ULL, 0xa588bb5917fb5d6eULL}, /* 1e-270 */
    {0xa90de3535aaae203ULL, 0x8eeaea2f5dfa34c9ULL}, /* 1e-269 */
    {0xd3515c2831559a84ULL, 0xf2a5a4bb3578c1fcULL}, /* 1e-268 */
    {0x8412d9991ed58092ULL, 0x17a786f5016b793dULL}, /* 1e-267 */
    {0xa5178fff668ae0b7ULL, 0x9d9168b241c6578dULL}, /* 1e-266 */
    {0xce5d73ff402d98e4ULL, 0x04f5c2ded237ed70ULL}, /* 1e-265 */
    {0x80fa687f881c7f8fULL, 0x831999cb4362f466ULL}, /* 1e-264 */
    {0xa139029f6a239f73ULL, 0xe3e0003e143bb17fULL}, /* 1e-263 */
    {0xc987434744ac874fULL, 0x5cd8004d994a9ddfULL}, /* 1e-262 */
    {0xfbe9141915d7a923ULL, 0xb40e0060ff9d4557ULL}, /* 1e-261 */
    {0x9d71ac8fada6c9b6ULL, 0x9088c03c9fc24b56ULL}, /* 1e-260 */
    {0xc4ce17b399107c23ULL, 0x34aaf04bc7b2de2cULL}, /* 1e-259 */
    {0xf6019da07f549b2cULL, 0x81d5ac5eb99f95b7ULL}, /* 1e-258 */
    {0x99c102844f94e0fcULL, 0xd1258bbb3403bd92ULL}, /* 1e-257 */
    {0xc0314325637a193aULL, 0x056eeeaa0104acf7ULL}, /* 1e-256 */
    {0xf03d93eebc589f89ULL, 0x86caaa548145d835ULL}, /* 1e-255 */
    {0x96267c7535b763b6ULL, 0xb43eaa74d0cba721ULL}, /* 1e-254 */
    {0xbbb01b9283253ca3ULL, 0x614e551204fe90e9ULL}, /* 1e-253 */
    {0xea9c227723ee8bccULL, 0xb9a1ea56863e3523ULL}, /* 1e-252 */
    {0x92a1958a76751760ULL, 0xf405327613e6e136ULL}, /* 1e-251 */
    {0xb749faed14125d37ULL, 0x31067f1398e09984ULL}, /* 1e-250 */
    {0xe51c79a85916f485ULL, 0x7d481ed87f18bfe5ULL}, /* 1e-249 */
    {0x8f31cc0937ae58d3ULL, 0x2e4d13474f6f77efULL}, /* 1e-248 */
    {0xb2fe3f0b8599ef08ULL, 0x79e05819234b55eaULL}, /* 1e-247 */
    {0xdfbdcece67006acaULL, 0x98586e1f6c1e2b65ULL}, /* 1e-246 */
    {0x8bd6a141006042beULL, 0x1f3744d3a392db1fULL}, /* 1e-245 */
    {0xaecc49914078536eULL, 0xa70516088c7791e7ULL}, /* 1e-244 */
    {0xda7f5bf590966849ULL, 0x50c65b8aaf957661ULL}, /* 1e-243 */
    {0x888f99797a5e012eULL, 0x927bf936adbd69fcULL}, /* 1e-242 */
    {0xaab37fd7d8f58179ULL, 0x371af784592cc47cULL}, /* 1e-241 */
    {0xd5605fcdcf32e1d7ULL, 0x04e1b5656f77f59bULL}, /* 1e-240 */
    {0x855c3be0a17fcd27ULL, 0xa30d115f65aaf980ULL}, /* 1e-239 */
    {0xa6b34ad8c9dfc070ULL, 0x0bd055b73f15b7e1ULL}, /* 1e-238 */
    {0xd0601d8efc57b08cULL, 0x0ec46b250edb25d9ULL}, /* 1e-237 */
    {0x823c12795db6ce58ULL, 0x893ac2f72948f7a7ULL}, /* 1e-236 */
    {0xa2cb1717b52481eeULL, 0xab8973b4f39b3591ULL}, /* 1e-235 */
    {0xcb7ddcdda26da269ULL, 0x566bd0a2308202f6ULL}, /* 1e-234 */
    {0xfe5d54150b090b03ULL, 0x2c06c4cabca283b3ULL}, /* 1e-233 */
    {0x9efa548d26e5a6e2ULL, 0x3b843afeb5e59250ULL}, /* 1e-232 */
    {0xc6b8e9b0709f109bULL, 0xca6549be635ef6e4ULL}, /* 1e-231 */
    {0xf867241c8cc6d4c1ULL, 0x3cfe9c2dfc36b49dULL}, /* 1e-230 */
    {0x9b407691d7fc44f9ULL, 0x861f219cbda230e2ULL}, /* 1e-229 */
    {0xc21094364dfb5637ULL, 0x67a6ea03ed0abd1bULL}, /* 1e-228 */
    {0xf294b943e17a2bc5ULL, 0xc190a484e84d6c62ULL}, /* 1e-227 */
    {0x979cf3ca6cec5b5bULL, 0x58fa66d3113063bdULL}, /* 1e-226 */
    {0xbd8430bd08277232ULL, 0xaf390087d57c7cacULL}, /* 1e-225 */
    {0xece53cec4a314ebeULL, 0x5b0740a9cadb9bd7ULL}, /* 1e-224 */
    {0x940f4613ae5ed137ULL, 0x78e4886a1ec94166ULL}, /* 1e-223 */
    {0xb913179899f68585ULL, 0xd71daa84a67b91c0ULL}, /* 1e-222 */
    {0xe757dd7ec07426e6ULL, 0xcce51525d01a7630ULL}, /* 1e-221 */
    {0x9096ea6f38489850ULL, 0xc00f2d37a21089deULL}, /* 1e-220 */
    {0xb4bca50b065abe64ULL, 0xf012f8858a94ac56ULL}, /* 1e-219 */
    {0xe1ebce4dc7f16dfcULL, 0x2c17b6a6ed39d76bULL}, /* 1e-218 */
    {0x8d3360f09cf6e4beULL, 0x9b8ed228544426a3ULL}, /* 1e-217 */
    {0xb080392cc4349dedULL, 0x427286b26955304cULL}, /* 1e-216 */
    {0xdca04777f541c568ULL, 0x130f285f03aa7c5fULL}, /* 1e-215 */
    {0x89e42caaf9491b61ULL, 0x0be9793b624a8dbbULL}, /* 1e-214 */
    {0xac5d37d5b79b623aULL, 0xcee3d78a3add312aULL}, /* 1e-213 */
    {0xd77485cb25823ac8ULL, 0x829ccd6cc9947d74ULL}, /* 1e-212 */
    {0x86a8d39ef77164bdULL, 0x51a20063fdfcce68ULL}, /* 1e-211 */
    {0xa8530886b54dbdecULL, 0x260a807cfd7c0203ULL}, /* 1e-210 */
    {0xd267caa862a12d67ULL, 0x2f8d209c3cdb0284ULL}, /* 1e-209 */
    {0x8380dea93da4bc61ULL, 0xbdb83461a608e192ULL}, /* 1e-208 */
    {0xa46116538d0deb79ULL, 0xad26417a0f8b19f7ULL}, /* 1e-207 */
    {0xcd795be870516657ULL, 0x986fd1d8936de074ULL}, /* 1e-206 */
    {0x806bd9714632dff7ULL, 0xff45e3275c24ac49ULL}, /* 1e-205 */
    {0xa086cfcd97bf97f4ULL, 0x7f175bf1332dd75bULL}, /* 1e-204 */
    {0xc8a883c0fdaf7df1ULL, 0x9edd32ed7ff94d32ULL}, /* 1e-203 */
    {0xfad2a4b13d1b5d6dULL, 0x86947fa8dff7a07eULL}, /* 1e-202 */
    {0x9cc3a6eec6311a64ULL, 0x341ccfc98bfac44fULL}, /* 1e-201 */
    {0xc3f490aa77bd60fdULL, 0x412403bbeef97563ULL}, /* 1e-200 */
    {0xf4f1b4d515acb93cULL, 0x116d04aaeab7d2bbULL}, /* 1e-199 */
    {0x991711052d8bf3c6ULL, 0x8ae422ead2b2e3b5ULL}, /* 1e-198 */
    {0xbf5cd54678eef0b7ULL, 0x2d9d2ba5875f9ca2ULL}, /* 1e-197 */
    {0xef340a98172aace5ULL, 0x7904768ee93783cbULL}, /* 1e-196 */
    {0x9580869f0e7aac0fULL, 0x2ba2ca1951c2b25fULL}, /* 1e-195 */
    {0xbae0a846d2195713ULL, 0x768b7c9fa6335ef6ULL}, /* 1e-194 */
    {0xe998d258869facd8ULL, 0xd42e5bc78fc036b4ULL}, /* 1e-193 */
    {0x91ff83775423cc07ULL, 0x849cf95cb9d82230ULL}, /* 1e-192 */
    {0xb67f6455292cbf09ULL, 0xe5c437b3e84e2abdULL}, /* 1e-191 */
    {0xe41f3d6a7377eecbULL, 0xdf3545a0e261b56cULL}, /* 1e-190 */
    {0x8e938662882af53fULL, 0xab814b848d7d1163ULL}, /* 1e-189 */
    {0xb23867fb2a35b28eULL, 0x16619e65b0dc55bcULL}, /* 1e-188 */
    {0xdec681f9f4c31f32ULL, 0x9bfa05ff1d136b2bULL}, /* 1e-187 */
    {0x8b3c113c38f9f37fULL, 0x217c43bf722c22fbULL}, /* 1e-186 */
    {0xae0b158b4738705fULL, 0x69db54af4eb72bbaULL}, /* 1e-185 */
    {0xd98ddaee19068c77ULL, 0xc45229db2264f6a8ULL}, /* 1e-184 */
    {0x87f8a8d4cfa417caULL, 0x1ab35a28f57f1a29ULL}, /* 1e-183 */
    {0xa9f6d30a038d1dbdULL, 0xa16030b332dee0b3ULL}, /* 1e-182 */
    {0xd47487cc8470652cULL, 0x89b83cdfff9698e0ULL}, /* 1e-181 */
    {0x84c8d4dfd2c63f3cULL, 0xd613260bffbe1f8cULL}, /* 1e-180 */
    {0xa5fb0a17c777cf0aULL, 0x0b97ef8effada76fULL}, /* 1e-179 */
    {0xcf79cc9db955c2cdULL, 0x8e7deb72bf99114bULL}, /* 1e-178 */
    {0x81ac1fe293d599c0ULL, 0x390eb327b7bfaacfULL}, /* 1e-177 */
    {0xa21727db38cb0030ULL, 0x47525ff1a5af9583ULL}, /* 1e-176 */
    {0xca9cf1d206fdc03cULL, 0x5926f7ee0f1b7ae3ULL}, /* 1e-175 */
    {0xfd442e4688bd304bULL, 0x6f70b5e992e2599cULL}, /* 1e-174 */
    {0x9e4a9cec15763e2fULL, 0x65a671b1fbcd7801ULL}, /* 1e-173 */
    {0xc5dd44271ad3cdbbULL, 0xbf100e1e7ac0d602ULL}, /* 1e-172 */
    {0xf7549530e188c129ULL, 0x2ed411a619710b83ULL}, /* 1e-171 */
    {0x9a94dd3e8cf578baULL, 0x7d448b07cfe6a731ULL}, /* 1e-170 */
    {0xc13a148e3032d6e8ULL, 0x1c95adc9c3e050feULL}, /* 1e-169 */
    {0xf18899b1bc3f8ca2ULL, 0x23bb193c34d8653eULL}, /* 1e-168 */
    {0x96f5600f15a7b7e6ULL, 0xd654efc5a1073f46ULL}, /* 1e-167 */
    {0xbcb2b812db11a5dfULL, 0x8bea2bb709490f18ULL}, /* 1e-166 */
    {0xebdf661791d60f57ULL, 0xeee4b6a4cb9b52deULL}, /* 1e-165 */
    {0x936b9fcebb25c996ULL, 0x354ef226ff4113cbULL}, /* 1e-164 */
    {0xb84687c269ef3bfcULL, 0xc2a2aeb0bf1158bdULL}, /* 1e-163 */
    {0xe65829b3046b0afbULL, 0xf34b5a5ceed5aeedULL}, /* 1e-162 */
    {0x8ff71a0fe2c2e6ddULL, 0xb80f187a15458d54ULL}, /* 1e-161 */
    {0xb3f4e093db73a094ULL, 0xa612de989a96f0a9ULL}, /* 1e-160 */
    {0xe0f218b8d25088b9ULL, 0xcf97963ec13cacd3ULL}, /* 1e-159 */
    {0x8c974f7383725574ULL, 0xe1bebde738c5ec04ULL}, /* 1e-158 */
    {0xafbd2350644eead0ULL, 0x1a2e6d6106f76705ULL}, /* 1e-157 */
    {0xdbac6c247d62a584ULL, 0x20ba08b948b540c6ULL}, /* 1e-156 */
    {0x894bc396ce5da773ULL, 0x94744573cd71487cULL}, /* 1e-155 */
    {0xab9eb47c81f51150ULL, 0xf99156d0c0cd9a9bULL}, /* 1e-154 */
    {0xd686619ba27255a3ULL, 0x37f5ac84f1010142ULL}, /* 1e-153 */
    {0x8613fd0145877586ULL, 0x42f98bd316a0a0c9ULL}, /* 1e-152 */
    {0xa798fc4196e952e8ULL, 0xd3b7eec7dc48c8fbULL}, /* 1e-151 */
    {0xd17f3b51fca3a7a1ULL, 0x08a5ea79d35afb3aULL}, /* 1e-150 */
    {0x82ef85133de648c5ULL, 0x6567b28c2418dd04ULL}, /* 1e-149 */
    {0xa3ab66580d5fdaf6ULL, 0x3ec19f2f2d1f1445ULL}, /* 1e-148 */
    {0xcc963fee10b7d1b4ULL, 0xce7206faf866d957ULL}, /* 1e-147 */
    {0xffbbcfe994e5c620ULL, 0x020e88b9b6808fadULL}, /* 1e-146 */
    {0x9fd561f1fd0f9bd4ULL, 0x01491574121059ccULL}, /* 1e-145 */
    {0xc7caba6e7c5382c9ULL, 0x019b5ad11694703fULL}, /* 1e-144 */
    {0xf9bd690a1b68637cULL, 0xc20231855c398c4fULL}, /* 1e-143 */
    {0x9c1661a651213e2eULL, 0xf9415ef359a3f7b1ULL}, /* 1e-142 */
    {0xc31bfa0fe5698db9ULL, 0xb791b6b0300cf59dULL}, /* 1e-141 */
    {0xf3e2f893dec3f127ULL, 0xa576245c3c103305ULL}, /* 1e-140 */
    {0x986ddb5c6b3a76b8ULL, 0x0769d6b9a58a1fe3ULL}, /* 1e-139 */
    {0xbe89523386091466ULL, 0x09444c680eeca7dcULL}, /* 1e-138 */
    {0xee2ba6c0678b5980ULL, 0x8b955f8212a7d1d3ULL}, /* 1e-137 */
    {0x94db483840b717f0ULL, 0x573d5bb14ba8e323ULL}, /* 1e-136 */
    {0xba121a4650e4ddecULL, 0x6d0cb29d9e931becULL}, /* 1e-135 */
    {0xe896a0d7e51e1567ULL, 0x884fdf450637e2e8ULL}, /* 1e-134 */
    {0x915e2486ef32cd61ULL, 0xf531eb8b23e2edd1ULL}, /* 1e-133 */
    {0xb5b5ada8aaff80b9ULL, 0xf27e666decdba945ULL}, /* 1e-132 */
    {0xe3231912d5bf60e7ULL, 0xef1e000968129396ULL}, /* 1e-131 */
    {0x8df5efabc5979c90ULL, 0x3572c005e10b9c3eULL}, /* 1e-130 */
    {0xb1736b96b6fd83b4ULL, 0x42cf7007594e834dULL}, /* 1e-129 */
    {0xddd0467c64bce4a1ULL, 0x53834c092fa22421ULL}, /* 1e-128 */
    {0x8aa22c0dbef60ee5ULL, 0x94320f85bdc55694ULL}, /* 1e-127 */
    {0xad4ab7112eb3929eULL, 0x793e93672d36ac39ULL}, /* 1e-126 */
    {0xd89d64d57a607745ULL, 0x178e3840f8845748ULL}, /* 1e-125 */
    {0x87625f056c7c4a8cULL, 0xeeb8e3289b52b68dULL}, /* 1e-124 */
    {0xa93af6c6c79b5d2eULL, 0x2a671bf2c2276430ULL}, /* 1e-123 */
    {0xd389b4787982347aULL, 0xb500e2ef72b13d3cULL}, /* 1e-122 */
    {0x843610cb4bf160ccULL, 0x31208dd5a7aec645ULL}, /* 1e-121 */
    {0xa54394fe1eedb8ffULL, 0x3d68b14b119a77d7ULL}, /* 1e-120 */
    {0xce947a3da6a9273fULL, 0x8cc2dd9dd60115cdULL}, /* 1e-119 */
    {0x811ccc668829b888ULL, 0xf7f9ca82a5c0ada0ULL}, /* 1e-118 */
    {0xa163ff802a3426a9ULL, 0x35f83d234f30d908ULL}, /* 1e-117 */
    {0xc9bcff6034c13053ULL, 0x03764c6c22fd0f4aULL}, /* 1e-116 */
    {0xfc2c3f3841f17c68ULL, 0x4453df872bbc531dULL}, /* 1e-115 */
    {0x9d9ba7832936edc1ULL, 0x2ab46bb47b55b3f2ULL}, /* 1e-114 */
    {0xc5029163f384a932ULL, 0xf56186a19a2b20eeULL}, /* 1e-113 */
    {0xf64335bcf065d37eULL, 0xb2b9e84a00b5e92aULL}, /* 1e-112 */
    {0x99ea0196163fa42fULL, 0xafb4312e4071b1baULL}, /* 1e-111 */
    {0xc06481fb9bcf8d3aULL, 0x1ba13d79d08e1e29ULL}, /* 1e-110 */
    {0xf07da27a82c37089ULL, 0xa2898cd844b1a5b3ULL}, /* 1e-109 */
    {0x964e858c91ba2656ULL, 0xc595f8072aef0790ULL}, /* 1e-108 */
    {0xbbe226efb628afebULL, 0x76fb7608f5aac974ULL}, /* 1e-107 */
    {0xeadab0aba3b2dbe6ULL, 0xd4ba538b33157bd1ULL}, /* 1e-106 */
    {0x92c8ae6b464fc970ULL, 0xc4f47436ffed6d62ULL}, /* 1e-105 */
    {0xb77ada0617e3bbccULL, 0xf6319144bfe8c8bbULL}, /* 1e-104 */
    {0xe55990879ddcaabeULL, 0x33bdf595efe2faeaULL}, /* 1e-103 */
    {0x8f57fa54c2a9eab7ULL, 0x6056b97db5eddcd2ULL}, /* 1e-102 */
    {0xb32df8e9f3546565ULL, 0xb86c67dd23695406ULL}, /* 1e-101 */
    {0xdff9772470297ebeULL, 0xa68781d46c43a908ULL}, /* 1e-100 */
    {0x8bfbea76c619ef37ULL, 0xa814b124c3aa49a5ULL}, /* 1e-99 */
    {0xaefae51477a06b04ULL, 0x1219dd6df494dc0eULL}, /* 1e-98 */
    {0xdab99e59958885c5ULL, 0x16a054c971ba1312ULL}, /* 1e-97 */
    {0x88b402f7fd75539cULL, 0xee2434fde7144bebULL}, /* 1e-96 */
    {0xaae103b5fcd2a882ULL, 0x29ad423d60d95ee6ULL}, /* 1e-95 */
    {0xd59944a37c0752a3ULL, 0xb41892ccb90fb6a0ULL}, /* 1e-94 */
    {0x857fcae62d8493a6ULL, 0x908f5bbff3a9d224ULL}, /* 1e-93 */
    {0xa6dfbd9fb8e5b88fULL, 0x34b332aff09446adULL}, /* 1e-92 */
    {0xd097ad07a71f26b3ULL, 0x81dfff5becb95858ULL}, /* 1e-91 */
    {0x825ecc24c8737830ULL, 0x712bff9973f3d737ULL}, /* 1e-90 */
    {0xa2f67f2dfa90563cULL, 0x8d76ff7fd0f0cd05ULL}, /* 1e-89 */
    {0xcbb41ef979346bcbULL, 0xb0d4bf5fc52d0046ULL}, /* 1e-88 */
    {0xfea126b7d78186bdULL, 0x1d09ef37b6784057ULL}, /* 1e-87 */
    {0x9f24b832e6b0f437ULL, 0xf2263582d20b2836ULL}, /* 1e-86 */
    {0xc6ede63fa05d3144ULL, 0x6eafc2e3868df244ULL}, /* 1e-85 */
    {0xf8a95fcf88747d95ULL, 0x8a5bb39c68316ed5ULL}, /* 1e-84 */
    {0x9b69dbe1b548ce7dULL, 0x36795041c11ee545ULL}, /* 1e-83 */
    {0xc24452da229b021cULL, 0x0417a45231669e97ULL}, /* 1e-82 */
    {0xf2d56790ab41c2a3ULL, 0x051d8d66bdc0463cULL}, /* 1e-81 */
    {0x97c560ba6b0919a6ULL, 0x2332786036982be5ULL}, /* 1e-80 */
    {0xbdb6b8e905cb6010ULL, 0xabff1678443e36dfULL}, /* 1e-79 */
    {0xed246723473e3814ULL, 0xd6fedc16554dc497ULL}, /* 1e-78 */
    {0x9436c0760c86e30cULL, 0x065f498df5509adeULL}, /* 1e-77 */
    {0xb94470938fa89bcfULL, 0x07f71bf172a4c196ULL}, /* 1e-76 */
    {0xe7958cb87392c2c3ULL, 0x49f4e2edcf4df1fbULL}, /* 1e-75 */
    {0x90bd77f3483bb9baULL, 0x4e390dd4a190b73dULL}, /* 1e-74 */
    {0xb4ecd5f01a4aa829ULL, 0xe1c75149c9f4e50cULL}, /* 1e-73 */
    {0xe2280b6c20dd5233ULL, 0xda39259c3c721e4fULL}, /* 1e-72 */
    {0x8d590723948a5360ULL, 0xa863b781a5c752f1ULL}, /* 1e-71 */
    {0xb0af48ec79ace838ULL, 0xd27ca5620f3927aeULL}, /* 1e-70 */
    {0xdcdb1b2798182245ULL, 0x071bceba9307719aULL}, /* 1e-69 */
    {0x8a08f0f8bf0f156cULL, 0xe47161349be4a700ULL}, /* 1e-68 */
    {0xac8b2d36eed2dac6ULL, 0x1d8db981c2ddd0c0ULL}, /* 1e-67 */
    {0xd7adf884aa879178ULL, 0xa4f127e2339544f0ULL}, /* 1e-66 */
    {0x86ccbb52ea94baebULL, 0x6716b8ed603d4b16ULL}, /* 1e-65 */
    {0xa87fea27a539e9a6ULL, 0xc0dc6728b84c9ddbULL}, /* 1e-64 */
    {0xd29fe4b18e88640fULL, 0x711380f2e65fc552ULL}, /* 1e-63 */
    {0x83a3eeeef9153e8aULL, 0xe6ac3097cffbdb53ULL}, /* 1e-62 */
    {0xa48ceaaab75a8e2cULL, 0xa0573cbdc3fad228ULL}, /* 1e-61 */
    {0xcdb02555653131b7ULL, 0xc86d0bed34f986b2ULL}, /* 1e-60 */
    {0x808e17555f3ebf12ULL, 0x1d442774411bf42fULL}, /* 1e-59 */
    {0xa0b19d2ab70e6ed7ULL, 0xa49531515162f13bULL}, /* 1e-58 */
    {0xc8de047564d20a8cULL, 0x0dba7da5a5bbad8aULL}, /* 1e-57 */
    {0xfb158592be068d2fULL, 0x11291d0f0f2a98edULL}, /* 1e-56 */
    {0x9ced737bb6c4183eULL, 0xaab9b229697a9f94ULL}, /* 1e-55 */
    {0xc428d05aa4751e4dULL, 0x55681eb3c3d94779ULL}, /* 1e-54 */
    {0xf53304714d9265e0ULL, 0x2ac22660b4cf9957ULL}, /* 1e-53 */
    {0x993fe2c6d07b7facULL, 0x1ab957fc7101bfd6ULL}, /* 1e-52 */
    {0xbf8fdb78849a5f97ULL, 0x2167adfb8d422fccULL}, /* 1e-51 */
    {0xef73d256a5c0f77dULL, 0x69c1997a7092bbbfULL}, /* 1e-50 */
    {0x95a8637627989aaeULL, 0x2218ffec865bb557ULL}, /* 1e-49 */
    {0xbb127c53b17ec15aULL, 0xaa9f3fe7a7f2a2adULL}, /* 1e-48 */
    {0xe9d71b689dde71b0ULL, 0x55470fe191ef4b59ULL}, /* 1e-47 */
    {0x9226712162ab070eULL, 0x354c69ecfb358f17ULL}, /* 1e-46 */
    {0xb6b00d69bb55c8d2ULL, 0xc29f84683a02f2ddULL}, /* 1e-45 */
    {0xe45c10c42a2b3b06ULL, 0x734765824883af95ULL}, /* 1e-44 */
    {0x8eb98a7a9a5b04e4ULL, 0x880c9f716d524dbdULL}, /* 1e-43 */
    {0xb267ed1940f1c61dULL, 0xaa0fc74dc8a6e12cULL}, /* 1e-42 */
    {0xdf01e85f912e37a4ULL, 0x9493b9213ad09977ULL}, /* 1e-41 */
    {0x8b61313bbabce2c7ULL, 0xdcdc53b4c4c25feaULL}, /* 1e-40 */
    {0xae397d8aa96c1b78ULL, 0x541368a1f5f2f7e5ULL}, /* 1e-39 */
    {0xd9c7dced53c72256ULL, 0x691842ca736fb5deULL}, /* 1e-38 */
    {0x881cea14545c7576ULL, 0x81af29be8825d1abULL}, /* 1e-37 */
    {0xaa242499697392d3ULL, 0x221af42e2a2f4616ULL}, /* 1e-36 */
    {0xd4ad2dbfc3d07788ULL, 0x6aa1b139b4bb179bULL}, /* 1e-35 */
    {0x84ec3c97da624ab5ULL, 0x42a50ec410f4eec1ULL}, /* 1e-34 */
    {0xa6274bbdd0fadd62ULL, 0x134e527515322a71ULL}, /* 1e-33 */
    {0xcfb11ead453994bbULL, 0x9821e7125a7eb50dULL}, /* 1e-32 */
    {0x81ceb32c4b43fcf5ULL, 0x7f15306b788f3128ULL}, /* 1e-31 */
    {0xa2425ff75e14fc32ULL, 0x5eda7c8656b2fd72ULL}, /* 1e-30 */
    {0xcad2f7f5359a3b3fULL, 0xf6911ba7ec5fbccfULL}, /* 1e-29 */
    {0xfd87b5f28300ca0eULL, 0x74356291e777ac03ULL}, /* 1e-28 */
    {0x9e74d1b791e07e49ULL, 0x88a15d9b30aacb82ULL}, /* 1e-27 */
    {0xc612062576589ddbULL, 0x6ac9b501fcd57e62ULL}, /* 1e-26 */
    {0xf79687aed3eec552ULL, 0xc57c22427c0addfbULL}, /* 1e-25 */
    {0x9abe14cd44753b53ULL, 0x3b6d95698d86cabdULL}, /* 1e-24 */
    {0xc16d9a0095928a28ULL, 0x8a48fac3f0e87d6cULL}, /* 1e-23 */
    {0xf1c90080baf72cb2ULL, 0xacdb3974ed229cc7ULL}, /* 1e-22 */
    {0x971da05074da7befULL, 0x2c0903e91435a1fcULL}, /* 1e-21 */
    {0xbce5086492111aebULL, 0x770b44e359430a7bULL}, /* 1e-20 */
    {0xec1e4a7db69561a6ULL, 0xd4ce161c2f93cd1aULL}, /* 1e-19 */
    {0x9392ee8e921d5d08ULL, 0xc500cdd19dbc6030ULL}, /* 1e-18 */
    {0xb877aa3236a4b44aULL, 0xf6410146052b783dULL}, /* 1e-17 */
    {0xe69594bec44de15cULL, 0xb3d141978676564cULL}, /* 1e-16 */
    {0x901d7cf73ab0acdaULL, 0xf062c8feb409f5efULL}, /* 1e-15 */
    {0xb424dc35095cd810ULL, 0xac7b7b3e610c736bULL}, /* 1e-14 */
    {0xe12e13424bb40e14ULL, 0xd79a5a0df94f9046ULL}, /* 1e-13 */
    {0x8cbccc096f5088ccULL, 0x06c07848bbd1ba2cULL}, /* 1e-12 */
    {0xafebff0bcb24aaffULL, 0x0870965aeac628b7ULL}, /* 1e-11 */
    {0xdbe6fecebdedd5bfULL, 0x4a8cbbf1a577b2e4ULL}, /* 1e-10 */
    {0x89705f4136b4a598ULL, 0xce97f577076acfcfULL}, /* 1e-9 */
    {0xabcc77118461cefdULL, 0x023df2d4c94583c2ULL}, /* 1e-8 */
    {0xd6bf94d5e57a42bdULL, 0xc2cd6f89fb96e4b3ULL}, /* 1e-7 */
    {0x8637bd05af6c69b6ULL, 0x59c065b63d3e4ef0ULL}, /* 1e-6 */
    {0xa7c5ac471b478424ULL, 0xf0307f23cc8de2acULL}, /* 1e-5 */
    {0xd1b71758e219652cULL, 0x2c3c9eecbfb15b57ULL}, /* 1e-4 */
    {0x83126e978d4fdf3cULL, 0x9ba5e353f7ced916ULL}, /* 1e-3 */
    {0xa3d70a3d70a3d70bULL, 0xc28f5c28f5c28f5cULL}, /* 1e-2 */
    {0xcccccccccccccccdULL, 0x3333333333333333ULL}, /* 1e-1 */
    {0x8000000000000000ULL, 0x0000000000000000ULL}, /* 1e0 */
    {0xa000000000000000ULL, 0x0000000000000000ULL}, /* 1e1 */
    {0xc800000000000000ULL, 0x0000000000000000ULL}, /* 1e2 */
    {0xfa00000000000000ULL, 0x0000000000000000ULL}, /* 1e3 */
    {0x9c40000000000000ULL, 0x0000000000000000ULL}, /* 1e4 */
    {0xc350000000000000ULL, 0x0000000000000000ULL}, /* 1e5 */
    {0xf424000000000000ULL, 0x0000000000000000ULL}, /* 1e6 */
    {0x9896800000000000ULL, 0x0000000000000000ULL}, /* 1e7 */
    {0xbebc200000000000ULL, 0x0000000000000000ULL}, /* 1e8 */
    {0xee6b280000000000ULL, 0x0000000000000000ULL}, /* 1e9 */
    {0x9502f90000000000ULL, 0x0000000000000000ULL}, /* 1e10 */
    {0xba43b74000000000ULL, 0x0000000000000000ULL}, /* 1e11 */
    {0xe8d4a51000000000ULL, 0x0000000000000000ULL}, /* 1e12 */
    {0x9184e72a00000000ULL, 0x0000000000000000ULL}, /* 1e13 */
    {0xb5e620f480000000ULL, 0x0000000000000000ULL}, /* 1e14 */
    {0xe35fa931a0000000ULL, 0x0000000000000000ULL}, /* 1e15 */
    {0x8e1bc9bf04000000ULL, 0x0000000000000000ULL}, /* 1e16 */
    {0xb1a2bc2ec5000000ULL, 0x0000000000000000ULL}, /* 1e17 */
    {0xde0b6b3a76400000ULL, 0x0000000000000000ULL}, /* 1e18 */
    {0x8ac7230489e80000ULL, 0x0000000000000000ULL}, /* 1e19 */
    {0xad78ebc5ac620000ULL, 0x0000000000000000ULL}, /* 1e20 */
    {0xd8d726b7177a8000ULL, 0x0000000000000000ULL}, /* 1e21 */
    {0x878678326eac9000ULL, 0x0000000000000000ULL}, /* 1e22 */
    {0xa968163f0a57b400ULL, 0x0000000000000000ULL}, /* 1e23 */
    {0xd3c21bcecceda100ULL, 0x0000000000000000ULL}, /* 1e24 */
    {0x84595161401484a0ULL, 0x0000000000000000ULL}, /* 1e25 */
    {0xa56fa5b99019a5c8ULL, 0x0000000000000000ULL}, /* 1e26 */
    {0xcecb8f27f4200f3aULL, 0x0000000000000000ULL}, /* 1e27 */
    {0x813f3978f8940985ULL, 0xc000000000000000ULL}, /* 1e28 */
    {0xa18f07d736b90be6ULL, 0xb000000000000000ULL}, /* 1e29 */
    {0xc9f2c9cd04674edfULL, 0x5c00000000000000ULL}, /* 1e30 */
    {0xfc6f7c4045812297ULL, 0xb300000000000000ULL}, /* 1e31 */
    {0x9dc5ada82b70b59eULL, 0x0fe0000000000000ULL}, /* 1e32 */
    {0xc5371912364ce306ULL, 0x93d8000000000000ULL}, /* 1e33 */
    {0xf684df56c3e01bc7ULL, 0x38ce000000000000ULL}, /* 1e34 */
    {0x9a130b963a6c115dULL, 0xc380c00000000000ULL}, /* 1e35 */
    {0xc097ce7bc90715b4ULL, 0xb460f00000000000ULL}, /* 1e36 */
    {0xf0bdc21abb48db21ULL, 0xe1792c0000000000ULL}, /* 1e37 */
    {0x96769950b50d88f5ULL, 0xecebbb8000000000ULL}, /* 1e38 */
    {0xbc143fa4e250eb32ULL, 0xe826aa6000000000ULL}, /* 1e39 */
    {0xeb194f8e1ae525feULL, 0xa23054f800000000ULL}, /* 1e40 */
    {0x92efd1b8d0cf37bfULL, 0xa55e351b00000000ULL}, /* 1e41 */
    {0xb7abc627050305aeULL, 0x0eb5c261c0000000ULL}, /* 1e42 */
    {0xe596b7b0c643c71aULL, 0x926332fa30000000ULL}, /* 1e43 */
    {0x8f7e32ce7bea5c70ULL, 0x1b7dffdc5e000000ULL}, /* 1e44 */
    {0xb35dbf821ae4f38cULL, 0x225d7fd375800000ULL}, /* 1e45 */
    {0xe0352f62a19e306fULL, 0x2af4dfc852e00000ULL}, /* 1e46 */
    {0x8c213d9da502de46ULL, 0xbad90bdd33cc0000ULL}, /* 1e47 */
    {0xaf298d050e4395d7ULL, 0x698f4ed480bf0000ULL}, /* 1e48 */
    {0xdaf3f04651d47b4dULL, 0xc3f32289a0eec000ULL}, /* 1e49 */
    {0x88d8762bf324cd10ULL, 0x5a77f59604953800ULL}, /* 1e50 */
    {0xab0e93b6efee0054ULL, 0x7115f2fb85ba8600ULL}, /* 1e51 */
    {0xd5d238a4abe98069ULL, 0x8d5b6fba67292780ULL}, /* 1e52 */
    {0x85a36366eb71f042ULL, 0xb85925d48079b8b0ULL}, /* 1e53 */
    {0xa70c3c40a64e6c52ULL, 0x666f6f49a09826dcULL}, /* 1e54 */
    {0xd0cf4b50cfe20766ULL, 0x000b4b1c08be3093ULL}, /* 1e55 */
    {0x82818f1281ed44a0ULL, 0x40070ef18576de5bULL}, /* 1e56 */
    {0xa321f2d7226895c8ULL, 0x5008d2ade6d495f2ULL}, /* 1e57 */
    {0xcbea6f8ceb02bb3aULL, 0x640b07596089bb6fULL}, /* 1e58 */
    {0xfee50b7025c36a09ULL, 0xfd0dc92fb8ac2a4bULL}, /* 1e59 */
    {0x9f4f2726179a2246ULL, 0xfe289dbdd36b9a6fULL}, /* 1e60 */
    {0xc722f0ef9d80aad7ULL, 0xbdb2c52d4846810aULL}, /* 1e61 */
    {0xf8ebad2b84e0d58cULL, 0x2d1f76789a58214dULL}, /* 1e62 */
    {0x9b934c3b330c8578ULL, 0x9c33aa0b607714d0ULL}, /* 1e63 */
    {0xc2781f49ffcfa6d6ULL, 0xc340948e3894da04ULL}, /* 1e64 */
    {0xf316271c7fc3908bULL, 0x7410b9b1c6ba1085ULL}, /* 1e65 */
    {0x97edd871cfda3a57ULL, 0x688a740f1c344a53ULL}, /* 1e66 */
    {0xbde94e8e43d0c8edULL, 0xc2ad1112e3415ce8ULL}, /* 1e67 */
    {0xed63a231d4c4fb28ULL, 0xb35855579c11b422ULL}, /* 1e68 */
    {0x945e455f24fb1cf9ULL, 0x70173556c18b1095ULL}, /* 1e69 */
    {0xb975d6b6ee39e437ULL, 0x4c1d02ac71edd4bbULL}, /* 1e70 */
    {0xe7d34c64a9c85d45ULL, 0x9f2443578e6949e9ULL}, /* 1e71 */
    {0x90e40fbeea1d3a4bULL, 0x4376aa16b901ce32ULL}, /* 1e72 */
    {0xb51d13aea4a488deULL, 0x9454549c674241beULL}, /* 1e73 */
    {0xe264589a4dcdab15ULL, 0x396969c38112d22eULL}, /* 1e74 */
    {0x8d7eb76070a08aedULL, 0x03e1e21a30abc35dULL}, /* 1e75 */
    {0xb0de65388cc8ada9ULL, 0xc4da5aa0bcd6b434ULL}, /* 1e76 */
    {0xdd15fe86affad913ULL, 0xb610f148ec0c6141ULL}, /* 1e77 */
    {0x8a2dbf142dfcc7acULL, 0x91ca96cd9387bcc8ULL}, /* 1e78 */
    {0xacb92ed9397bf997ULL, 0xb63d3c80f869abfbULL}, /* 1e79 */
    {0xd7e77a8f87daf7fcULL, 0x23cc8ba1368416f9ULL}, /* 1e80 */
    {0x86f0ac99b4e8dafeULL, 0x965fd744c2128e5cULL}, /* 1e81 */
    {0xa8acd7c0222311bdULL, 0x3bf7cd15f29731f3ULL}, /* 1e82 */
    {0xd2d80db02aabd62cULL, 0x0af5c05b6f3cfe6fULL}, /* 1e83 */
    {0x83c7088e1aab65dcULL, 0x86d9983925861f05ULL}, /* 1e84 */
    {0xa4b8cab1a1563f53ULL, 0xa88ffe476ee7a6c7ULL}, /* 1e85 */
    {0xcde6fd5e09abcf27ULL, 0x12b3fdd94aa19079ULL}, /* 1e86 */
    {0x80b05e5ac60b6179ULL, 0xabb07ea7cea4fa4bULL}, /* 1e87 */
    {0xa0dc75f1778e39d7ULL, 0x969c9e51c24e38deULL}, /* 1e88 */
    {0xc913936dd571c84dULL, 0xfc43c5e632e1c716ULL}, /* 1e89 */
    {0xfb5878494ace3a60ULL, 0xfb54b75fbf9a38dcULL}, /* 1e90 */
    {0x9d174b2dcec0e47cULL, 0x9d14f29bd7c06389ULL}, /* 1e91 */
    {0xc45d1df942711d9bULL, 0xc45a2f42cdb07c6bULL}, /* 1e92 */
    {0xf5746577930d6501ULL, 0x3570bb13811c9b86ULL}, /* 1e93 */
    {0x9968bf6abbe85f21ULL, 0x816674ec30b1e134ULL}, /* 1e94 */
    {0xbfc2ef456ae276e9ULL, 0x61c012273cde5981ULL}, /* 1e95 */
    {0xefb3ab16c59b14a3ULL, 0x3a3016b10c15efe1ULL}, /* 1e96 */
    {0x95d04aee3b80ece6ULL, 0x445e0e2ea78db5edULL}, /* 1e97 */
    {0xbb445da9ca612820ULL, 0xd57591ba51712368ULL}, /* 1e98 */
    {0xea1575143cf97227ULL, 0x0ad2f628e5cd6c42ULL}, /* 1e99 */
    {0x924d692ca61be759ULL, 0xa6c3d9d98fa063a9ULL}, /* 1e100 */
    {0xb6e0c377cfa2e12fULL, 0x9074d04ff3887c93ULL}, /* 1e101 */
    {0xe498f455c38b997bULL, 0xf4920463f06a9bb8ULL}, /* 1e102 */
    {0x8edf98b59a373fedULL, 0xb8db42be7642a153ULL}, /* 1e103 */
    {0xb2977ee300c50fe8ULL, 0xa712136e13d349a8ULL}, /* 1e104 */
    {0xdf3d5e9bc0f653e2ULL, 0xd0d6984998c81c12ULL}, /* 1e105 */
    {0x8b865b215899f46dULL, 0x42861f2dff7d118bULL}, /* 1e106 */
    {0xae67f1e9aec07188ULL, 0x1327a6f97f5c55eeULL}, /* 1e107 */
    {0xda01ee641a708deaULL, 0x17f190b7df336b6aULL}, /* 1e108 */
    {0x884134fe908658b3ULL, 0xcef6fa72eb802322ULL}, /* 1e109 */
    {0xaa51823e34a7eedfULL, 0x42b4b90fa6602beaULL}, /* 1e110 */
    {0xd4e5e2cdc1d1ea97ULL, 0x9361e7538ff836e5ULL}, /* 1e111 */
    {0x850fadc09923329fULL, 0xfc1d309439fb224fULL}, /* 1e112 */
    {0xa6539930bf6bff46ULL, 0x7b247cb94879eae3ULL}, /* 1e113 */
    {0xcfe87f7cef46ff17ULL, 0x19ed9be79a98659cULL}, /* 1e114 */
    {0x81f14fae158c5f6fULL, 0xb0348170c09f3f81ULL}, /* 1e115 */
    {0xa26da3999aef774aULL, 0x1c41a1ccf0c70f62ULL}, /* 1e116 */
    {0xcb090c8001ab551dULL, 0xa3520a402cf8d33aULL}, /* 1e117 */
    {0xfdcb4fa002162a64ULL, 0x8c268cd038370809ULL}, /* 1e118 */
    {0x9e9f11c4014dda7fULL, 0xd798180223226505ULL}, /* 1e119 */
    {0xc646d63501a1511eULL, 0x4d7e1e02abeafe47ULL}, /* 1e120 */
    {0xf7d88bc24209a566ULL, 0xe0dda58356e5bdd9ULL}, /* 1e121 */
    {0x9ae7575969460760ULL, 0xcc8a8772164f96a7ULL}, /* 1e122 */
    {0xc1a12d2fc3978938ULL, 0xffad294e9be37c51ULL}, /* 1e123 */
    {0xf209787bb47d6b85ULL, 0x3f9873a242dc5b65ULL}, /* 1e124 */
    {0x9745eb4d50ce6333ULL, 0x07bf484569c9b91fULL}, /* 1e125 */
    {0xbd176620a501fc00ULL, 0x49af1a56c43c2767ULL}, /* 1e126 */
    {0xec5d3fa8ce427b00ULL, 0x5c1ae0ec754b3141ULL}, /* 1e127 */
    {0x93ba47c980e98ce0ULL, 0x3990cc93c94efec8ULL}, /* 1e128 */
    {0xb8a8d9bbe123f018ULL, 0x47f4ffb8bba2be7bULL}, /* 1e129 */
    {0xe6d3102ad96cec1eULL, 0x59f23fa6ea8b6e1aULL}, /* 1e130 */
    {0x9043ea1ac7e41393ULL, 0x783767c8529724d0ULL}, /* 1e131 */
    {0xb454e4a179dd1878ULL, 0xd64541ba673cee04ULL}, /* 1e132 */
    {0xe16a1dc9d8545e95ULL, 0x0bd69229010c2985ULL}, /* 1e133 */
    {0x8ce2529e2734bb1eULL, 0xe7661b59a0a799f3ULL}, /* 1e134 */
    {0xb01ae745b101e9e5ULL, 0xa13fa23008d18070ULL}, /* 1e135 */
    {0xdc21a1171d42645eULL, 0x898f8abc0b05e08cULL}, /* 1e136 */
    {0x899504ae72497ebbULL, 0x95f9b6b586e3ac57ULL}, /* 1e137 */
    {0xabfa45da0edbde6aULL, 0xfb782462e89c976dULL}, /* 1e138 */
    {0xd6f8d7509292d604ULL, 0xba562d7ba2c3bd49ULL}, /* 1e139 */
    {0x865b86925b9bc5c3ULL, 0xf475dc6d45ba564dULL}, /* 1e140 */
    {0xa7f26836f282b733ULL, 0x719353889728ebe1ULL}, /* 1e141 */
    {0xd1ef0244af236500ULL, 0xcdf8286abcf326d9ULL}, /* 1e142 */
    {0x8335616aed761f20ULL, 0x80bb1942b617f847ULL}, /* 1e143 */
    {0xa402b9c5a8d3a6e8ULL, 0xa0e9df93639df659ULL}, /* 1e144 */
    {0xcd036837130890a2ULL, 0xc92457783c8573f0ULL}, /* 1e145 */
    {0x802221226be55a65ULL, 0x3db6b6ab25d36876ULL}, /* 1e146 */
    {0xa02aa96b06deb0feULL, 0x0d246455ef484293ULL}, /* 1e147 */
    {0xc83553c5c8965d3eULL, 0x906d7d6b6b1a5338ULL}, /* 1e148 */
    {0xfa42a8b73abbf48dULL, 0x3488dcc645e0e806ULL}, /* 1e149 */
    {0x9c69a97284b578d8ULL, 0x00d589fbebac9104ULL}, /* 1e150 */
    {0xc38413cf25e2d70eULL, 0x010aec7ae697b545ULL}, /* 1e151 */
    {0xf46518c2ef5b8cd2ULL, 0x814da799a03da296ULL}, /* 1e152 */
    {0x98bf2f79d5993803ULL, 0x10d088c00426859eULL}, /* 1e153 */
    {0xbeeefb584aff8604ULL, 0x5504aaf005302705ULL}, /* 1e154 */
    {0xeeaaba2e5dbf6785ULL, 0x6a45d5ac067c30c7ULL}, /* 1e155 */
    {0x952ab45cfa97a0b3ULL, 0x226ba58b840d9e7cULL}, /* 1e156 */
    {0xba756174393d88e0ULL, 0x6b068eee6511061bULL}, /* 1e157 */
    {0xe912b9d1478ceb18ULL, 0x85c832a9fe5547a2ULL}, /* 1e158 */
    {0x91abb422ccb812efULL, 0x539d1faa3ef54cc5ULL}, /* 1e159 */
    {0xb616a12b7fe617abULL, 0xa8846794ceb29ff6ULL}, /* 1e160 */
    {0xe39c49765fdf9d95ULL, 0x12a5817a025f47f4ULL}, /* 1e161 */
    {0x8e41ade9fbebc27eULL, 0xeba770ec417b8cf8ULL}, /* 1e162 */
    {0xb1d219647ae6b31dULL, 0xa6914d2751da7037ULL}, /* 1e163 */
    {0xde469fbd99a05fe4ULL, 0x9035a07126510c44ULL}, /* 1e164 */
    {0x8aec23d680043befULL, 0xda218446b7f2a7abULL}, /* 1e165 */
    {0xada72ccc20054aeaULL, 0x50a9e55865ef5195ULL}, /* 1e166 */
    {0xd910f7ff28069da5ULL, 0xe4d45eae7f6b25fbULL}, /* 1e167 */
    {0x87aa9aff79042287ULL, 0x6f04bb2d0fa2f7bdULL}, /* 1e168 */
    {0xa99541bf57452b29ULL, 0xcac5e9f8538bb5acULL}, /* 1e169 */
    {0xd3fa922f2d1675f3ULL, 0xbd776476686ea317ULL}, /* 1e170 */
    {0x847c9b5d7c2e09b8ULL, 0x966a9eca014525eeULL}, /* 1e171 */
    {0xa59bc234db398c26ULL, 0xbc05467c81966f6aULL}, /* 1e172 */
    {0xcf02b2c21207ef2fULL, 0x6b06981ba1fc0b44ULL}, /* 1e173 */
    {0x8161afb94b44f57eULL, 0xe2e41f11453d870aULL}, /* 1e174 */
    {0xa1ba1ba79e1632ddULL, 0x9b9d26d5968ce8cdULL}, /* 1e175 */
    {0xca28a291859bbf94ULL, 0x8284708afc302301ULL}, /* 1e176 */
    {0xfcb2cb35e702af79ULL, 0xa3258cadbb3c2bc1ULL}, /* 1e177 */
    {0x9defbf01b061adacULL, 0xc5f777ec95059b58ULL}, /* 1e178 */
    {0xc56baec21c7a1917ULL, 0xf77555e7ba47022fULL}, /* 1e179 */
    {0xf6c69a72a3989f5cULL, 0x7552ab61a8d8c2baULL}, /* 1e180 */
    {0x9a3c2087a63f639aULL, 0xc953ab1d098779b4ULL}, /* 1e181 */
    {0xc0cb28a98fcf3c80ULL, 0x7ba895e44be95822ULL}, /* 1e182 */
    {0xf0fdf2d3f3c30ba0ULL, 0x9a92bb5d5ee3ae2aULL}, /* 1e183 */
    {0x969eb7c47859e744ULL, 0x609bb51a5b4e4cdaULL}, /* 1e184 */
    {0xbc4665b596706115ULL, 0x78c2a260f221e011ULL}, /* 1e185 */
    {0xeb57ff22fc0c795aULL, 0x56f34af92eaa5815ULL}, /* 1e186 */
    {0x9316ff75dd87cbd9ULL, 0xf6580edbbd2a770dULL}, /* 1e187 */
    {0xb7dcbf5354e9becfULL, 0xf3ee1292ac7514d0ULL}, /* 1e188 */
    {0xe5d3ef282a242e82ULL, 0x70e9973757925a05ULL}, /* 1e189 */
    {0x8fa475791a569d11ULL, 0x0691fe8296bb7843ULL}, /* 1e190 */
    {0xb38d92d760ec4456ULL, 0xc8367e233c6a5653ULL}, /* 1e191 */
    {0xe070f78d3927556bULL, 0x7a441dac0b84ebe8ULL}, /* 1e192 */
    {0x8c469ab843b89563ULL, 0x6c6a928b87331371ULL}, /* 1e193 */
    {0xaf58416654a6babcULL, 0xc785372e68ffd84dULL}, /* 1e194 */
    {0xdb2e51bfe9d0696bULL, 0xf96684fa033fce61ULL}, /* 1e195 */
    {0x88fcf317f22241e3ULL, 0xbbe0131c4207e0fcULL}, /* 1e196 */
    {0xab3c2fddeeaad25bULL, 0x2ad817e35289d93cULL}, /* 1e197 */
    {0xd60b3bd56a5586f2ULL, 0x758e1ddc272c4f8bULL}, /* 1e198 */
    {0x85c7056562757457ULL, 0x0978d2a9987bb1b6ULL}, /* 1e199 */
    {0xa738c6bebb12d16dULL, 0x4bd70753fe9a9e24ULL}, /* 1e200 */
    {0xd106f86e69d785c8ULL, 0x1eccc928fe4145adULL}, /* 1e201 */
    {0x82a45b450226b39dULL, 0x133ffdb99ee8cb8cULL}, /* 1e202 */
    {0xa34d721642b06085ULL, 0xd80ffd2806a2fe6fULL}, /* 1e203 */
    {0xcc20ce9bd35c78a6ULL, 0xce13fc72084bbe0bULL}, /* 1e204 */
    {0xff290242c83396cfULL, 0x8198fb8e8a5ead8eULL}, /* 1e205 */
    {0x9f79a169bd203e42ULL, 0xf0ff9d39167b2c79ULL}, /* 1e206 */
    {0xc75809c42c684dd2ULL, 0xad3f84875c19f797ULL}, /* 1e207 */
    {0xf92e0c3537826146ULL, 0x588f65a93320757dULL}, /* 1e208 */
    {0x9bbcc7a142b17cccULL, 0x77599f89bff4496eULL}, /* 1e209 */
    {0xc2abf989935ddbffULL, 0x9530076c2ff15bcaULL}, /* 1e210 */
    {0xf356f7ebf83552ffULL, 0xfa7c09473bedb2bcULL}, /* 1e211 */
    {0x98165af37b2153dfULL, 0x3c8d85cc85748fb5ULL}, /* 1e212 */
    {0xbe1bf1b059e9a8d7ULL, 0x8bb0e73fa6d1b3a3ULL}, /* 1e213 */
    {0xeda2ee1c7064130dULL, 0xee9d210f9086208cULL}, /* 1e214 */
    {0x9485d4d1c63e8be8ULL, 0x752234a9ba53d457ULL}, /* 1e215 */
    {0xb9a74a0637ce2ee2ULL, 0x926ac1d428e8c96dULL}, /* 1e216 */
    {0xe8111c87c5c1ba9aULL, 0x370572493322fbc8ULL}, /* 1e217 */
    {0x910ab1d4db9914a1ULL, 0xe263676dbff5dd5dULL}, /* 1e218 */
    {0xb54d5e4a127f59c9ULL, 0xdafc41492ff354b4ULL}, /* 1e219 */
    {0xe2a0b5dc971f303bULL, 0xd1bb519b7bf029e2ULL}, /* 1e220 */
    {0x8da471a9de737e25ULL, 0xa31513012d761a2dULL}, /* 1e221 */
    {0xb10d8e1456105daeULL, 0x8bda57c178d3a0b8ULL}, /* 1e222 */
    {0xdd50f1996b947519ULL, 0x2ed0edb1d70888e6ULL}, /* 1e223 */
    {0x8a5296ffe33cc930ULL, 0x7d42948f26655590ULL}, /* 1e224 */
    {0xace73cbfdc0bfb7cULL, 0x9c9339b2effeaaf4ULL}, /* 1e225 */
    {0xd8210befd30efa5bULL, 0xc3b8081fabfe55b1ULL}, /* 1e226 */
    {0x8714a775e3e95c79ULL, 0x9a530513cb7ef58eULL}, /* 1e227 */
    {0xa8d9d1535ce3b397ULL, 0x80e7c658be5eb2f2ULL}, /* 1e228 */
    {0xd31045a8341ca07dULL, 0xe121b7eeedf65fafULL}, /* 1e229 */
    {0x83ea2b892091e44eULL, 0x6cb512f554b9fbcdULL}, /* 1e230 */
    {0xa4e4b66b68b65d61ULL, 0x07e257b2a9e87ac0ULL}, /* 1e231 */
    {0xce1de40642e3f4baULL, 0xc9daed9f54629971ULL}, /* 1e232 */
    {0x80d2ae83e9ce78f4ULL, 0x3e28d48394bd9fe6ULL}, /* 1e233 */
    {0xa1075a24e4421731ULL, 0x4db309a479ed07e0ULL}, /* 1e234 */
    {0xc94930ae1d529cfdULL, 0x211fcc0d986849d8ULL}, /* 1e235 */
    {0xfb9b7cd9a4a7443dULL, 0xe967bf10fe825c4eULL}, /* 1e236 */
    {0x9d412e0806e88aa6ULL, 0x71e0d76a9f1179b1ULL}, /* 1e237 */
    {0xc491798a08a2ad4fULL, 0x0e590d4546d5d81dULL}, /* 1e238 */
    {0xf5b5d7ec8acb58a3ULL, 0x51ef5096988b4e24ULL}, /* 1e239 */
    {0x9991a6f3d6bf1766ULL, 0x5335925e1f5710d6ULL}, /* 1e240 */
    {0xbff610b0cc6edd40ULL, 0xe802f6f5a72cd50cULL}, /* 1e241 */
    {0xeff394dcff8a948fULL, 0x2203b4b310f80a4fULL}, /* 1e242 */
    {0x95f83d0a1fb69cdaULL, 0xb54250efea9b0671ULL}, /* 1e243 */
    {0xbb764c4ca7a44410ULL, 0x6292e52be541c80eULL}, /* 1e244 */
    {0xea53df5fd18d5514ULL, 0x7b379e76de923a12ULL}, /* 1e245 */
    {0x92746b9be2f8552dULL, 0xcd02c30a4b1b644bULL}, /* 1e246 */
    {0xb7118682dbb66a78ULL, 0xc04373ccdde23d5eULL}, /* 1e247 */
    {0xe4d5e82392a40516ULL, 0xf05450c0155accb5ULL}, /* 1e248 */
    {0x8f05b1163ba6832eULL, 0xd634b2780d58bff1ULL}, /* 1e249 */
    {0xb2c71d5bca9023f9ULL, 0x8bc1df1610aeefedULL}, /* 1e250 */
    {0xdf78e4b2bd342cf7ULL, 0x6eb256db94daabe9ULL}, /* 1e251 */
    {0x8bab8eefb6409c1bULL, 0xe52f76493d08ab71ULL}, /* 1e252 */
    {0xae9672aba3d0c321ULL, 0x5e7b53db8c4ad64eULL}, /* 1e253 */
    {0xda3c0f568cc4f3e9ULL, 0x361a28d26f5d8be1ULL}, /* 1e254 */
    {0x8865899617fb1872ULL, 0x81d05983859a776dULL}, /* 1e255 */
    {0xaa7eebfb9df9de8eULL, 0x22446fe467011548ULL}, /* 1e256 */
    {0xd51ea6fa85785632ULL, 0xaad58bdd80c15a9aULL}, /* 1e257 */
    {0x8533285c936b35dfULL, 0x2ac5776a7078d8a0ULL}, /* 1e258 */
    {0xa67ff273b8460357ULL, 0x7576d5450c970ec8ULL}, /* 1e259 */
    {0xd01fef10a657842dULL, 0xd2d48a964fbcd27aULL}, /* 1e260 */
    {0x8213f56a67f6b29cULL, 0x63c4d69df1d6038cULL}, /* 1e261 */
    {0xa298f2c501f45f43ULL, 0x7cb60c456e4b8470ULL}, /* 1e262 */
    {0xcb3f2f7642717714ULL, 0xdbe38f56c9de658cULL}, /* 1e263 */
    {0xfe0efb53d30dd4d8ULL, 0x12dc732c7c55feefULL}, /* 1e264 */
    {0x9ec95d1463e8a507ULL, 0x0bc9c7fbcdb5bf55ULL}, /* 1e265 */
    {0xc67bb4597ce2ce49ULL, 0x4ebc39fac1232f2aULL}, /* 1e266 */
    {0xf81aa16fdc1b81dbULL, 0x226b4879716bfaf5ULL}, /* 1e267 */
    {0x9b10a4e5e9913129ULL, 0x35830d4be6e37cd9ULL}, /* 1e268 */
    {0xc1d4ce1f63f57d73ULL, 0x02e3d09ee09c5c0fULL}, /* 1e269 */
    {0xf24a01a73cf2dcd0ULL, 0x439cc4c698c37313ULL}, /* 1e270 */
    {0x976e41088617ca02ULL, 0x2a41fafc1f7a27ecULL}, /* 1e271 */
    {0xbd49d14aa79dbc83ULL, 0xb4d279bb2758b1e7ULL}, /* 1e272 */
    {0xec9c459d51852ba3ULL, 0x22071829f12ede61ULL}, /* 1e273 */
    {0x93e1ab8252f33b46ULL, 0x35446f1a36bd4afcULL}, /* 1e274 */
    {0xb8da1662e7b00a18ULL, 0xc2958ae0c46c9dbcULL}, /* 1e275 */
    {0xe7109bfba19c0c9eULL, 0xf33aed98f587c52bULL}, /* 1e276 */
    {0x906a617d450187e3ULL, 0xd804d47f9974db3aULL}, /* 1e277 */
    {0xb484f9dc9641e9dbULL, 0x4e06099f7fd21209ULL}, /* 1e278 */
    {0xe1a63853bbd26452ULL, 0xa1878c075fc6968cULL}, /* 1e279 */
    {0x8d07e33455637eb3ULL, 0x24f4b7849bdc1e17ULL}, /* 1e280 */
    {0xb049dc016abc5e60ULL, 0x6e31e565c2d3259dULL}, /* 1e281 */
    {0xdc5c5301c56b75f8ULL, 0x89be5ebf3387ef04ULL}, /* 1e282 */
    {0x89b9b3e11b6329bbULL, 0x5616fb378034f562ULL}, /* 1e283 */
    {0xac2820d9623bf42aULL, 0xab9cba05604232bbULL}, /* 1e284 */
    {0xd732290fbacaf134ULL, 0x5683e886b852bf6aULL}, /* 1e285 */
    {0x867f59a9d4bed6c1ULL, 0xb61271543333b7a2ULL}, /* 1e286 */
    {0xa81f301449ee8c71ULL, 0xa3970da94000a58bULL}, /* 1e287 */
    {0xd226fc195c6a2f8dULL, 0x8c7cd1139000ceeeULL}, /* 1e288 */
    {0x83585d8fd9c25db8ULL, 0x37ce02ac3a008154ULL}, /* 1e289 */
    {0xa42e74f3d032f526ULL, 0x45c183574880a1aaULL}, /* 1e290 */
    {0xcd3a1230c43fb270ULL, 0xd731e42d1aa0ca14ULL}, /* 1e291 */
    {0x80444b5e7aa7cf86ULL, 0x867f2e9c30a47e4cULL}, /* 1e292 */
    {0xa0555e361951c367ULL, 0x281efa433ccd9de0ULL}, /* 1e293 */
    {0xc86ab5c39fa63441ULL, 0x7226b8d40c010558ULL}, /* 1e294 */
    {0xfa856334878fc151ULL, 0x4eb067090f0146aeULL}, /* 1e295 */
    {0x9c935e00d4b9d8d3ULL, 0x912e4065a960cc2cULL}, /* 1e296 */
    {0xc3b8358109e84f08ULL, 0xf579d07f13b8ff37ULL}, /* 1e297 */
    {0xf4a642e14c6262c9ULL, 0x32d8449ed8a73f05ULL}, /* 1e298 */
    {0x98e7e9cccfbd7dbeULL, 0x7fc72ae347688763ULL}, /* 1e299 */
    {0xbf21e44003acdd2dULL, 0x1fb8f59c1942a93cULL}, /* 1e300 */
    {0xeeea5d5004981479ULL, 0xe7a733031f93538bULL}, /* 1e301 */
    {0x95527a5202df0cccULL, 0xf0c87fe1f3bc1437ULL}, /* 1e302 */
    {0xbaa718e68396cffeULL, 0x2cfa9fda70ab1945ULL}, /* 1e303 */
    {0xe950df20247c83feULL, 0xb83947d10cd5df96ULL}, /* 1e304 */
    {0x91d28b7416cdd27fULL, 0xb323cce2a805abbeULL}, /* 1e305 */
    {0xb6472e511c81471eULL, 0x1fecc01b520716adULL}, /* 1e306 */
    {0xe3d8f9e563a198e6ULL, 0xa7e7f0222688dc59ULL}, /* 1e307 */
    {0x8e679c2f5e44ff90ULL, 0xa8f0f615581589b7ULL}, /* 1e308 */
    {0xb201833b35d63f74ULL, 0xd32d339aae1aec25ULL}, /* 1e309 */
    {0xde81e40a034bcf50ULL, 0x07f8808159a1a72eULL}, /* 1e310 */
    /* END GENERATED: pow10 */
        /* clang-format on */
};

/* ---- Fast unrounded scaling (uscale.go) ----------------------------------- */

static double bdfi_svg_float_from_bits(uint64_t u) {
  double v;
  memcpy(&v, &u, sizeof v);
  return v;
}

/* bits.Mul64: the product of two 64-bit numbers, in two halves. */
static void bdfi_svg_float_mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
  uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32;
  uint64_t b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
  uint64_t w0 = a0 * b0;
  uint64_t t = a1 * b0 + (w0 >> 32);
  uint64_t w1 = (t & 0xFFFFFFFFu) + a0 * b1;
  *hi = a1 * b1 + (t >> 32) + (w1 >> 32);
  *lo = a * b;
}

/* bits.Len64: the bits needed to write x. */
static int bdfi_svg_float_len64(uint64_t x) {
  int n = 0;
  for (int step = 32; step > 0; step /= 2) {
    if (x >> step != 0) {
      n += step;
      x >>= step;
    }
  }
  return n + (int)x;
}

/* log2Pow10: floor(x * log2(10)), as (x * 108853) >> 15 of a signed x. */
static int bdfi_svg_float_log2pow10(int x) {
  int32_t v = (int32_t)x * 108853;
  return v >= 0 ? (int)(v >> 15) : -(int)((-v + 32767) >> 15);
}

/* parseFloat64 in uscale.go: rounds d * 10**p to the nearest float64, for a
 * d of at most 19 digits that is not 0 and a p of the table. sign is the
 * sign bit. It returns 1 when the result is an infinity (ErrRange). */
static int bdfi_svg_float_scale(uint64_t d, int p, uint64_t sign, double *out) {
  int b = bdfi_svg_float_len64(d);
  int lp = bdfi_svg_float_log2pow10(p);
  int e = 53 - b - lp;
  if (e > 1074) {
    e = 1074;
  }
  /* prescale */
  const bdfi_svg_float_pow *pm = &bdfi_svg_float_pows[p - BDFI_SVG_FLOAT_POW_MIN];
  int s = -(e - (64 - b) + lp + 3); /* 8 at least */
  if (s >= 64) {
    *out = bdfi_svg_float_from_bits(sign);
    return 0;
  }
  /* uscale: unround(x * 2**e * 10**p) of x with its high bit set */
  uint64_t x = d << (64 - b);
  uint64_t hi, mid, mid2, low, u;
  bdfi_svg_float_mul64(x, pm->hi, &hi, &mid);
  if (((hi >> s) << s) != hi) {
    u = (hi >> s) | 1;
  } else {
    bdfi_svg_float_mul64(x, pm->lo, &mid2, &low);
    (void)low;
    if (mid < mid2) {
      hi--;
    }
    u = (hi >> s) | (uint64_t)(mid - mid2 > 1);
  }
  /* if u.round() >= 1<<53 { u = u.rsh(1); e = e - 1 } */
  int over = u >= (((uint64_t)1 << 55) - 2);
  u = (u >> over) | (u & 1);
  e -= over;
  /* unrounded.round, then pack64(m, -e) */
  uint64_t m = (u + 1 + ((u >> 2) & 1)) >> 2;
  if ((m & ((uint64_t)1 << 52)) == 0) {
    *out = bdfi_svg_float_from_bits(sign | m);
    return 0;
  }
  if (-e >= 0x7FF - 1075) {
    *out = bdfi_svg_float_from_bits(sign | ((uint64_t)0x7FF << 52));
    return 1;
  }
  *out = bdfi_svg_float_from_bits(sign | (m & ~((uint64_t)1 << 52)) | ((uint64_t)(1075 - e) << 52));
  return 0;
}

/* ---- Multiprecision decimal numbers (decimal.go) -------------------------- */

#define BDFI_SVG_FLOAT_DIGITS    800
#define BDFI_SVG_FLOAT_MAX_SHIFT 60 /* maxShift where a uint has 64 bits */

typedef struct bdfi_svg_float_dec {
  uint8_t d[BDFI_SVG_FLOAT_DIGITS]; /* digits, the most significant first */
  int64_t nd;                       /* number of digits used */
  int64_t dp;                       /* decimal point */
  int neg;
  int trunc; /* discarded nonzero digits beyond d[:nd] */
} bdfi_svg_float_dec;

static void bdfi_svg_float_trim(bdfi_svg_float_dec *a) {
  while (a->nd > 0 && a->d[a->nd - 1] == '0') {
    a->nd--;
  }
  if (a->nd == 0) {
    a->dp = 0;
  }
}

/* rightShift: binary shift right (/ 2) by k bits, k <= maxShift. */
static void bdfi_svg_float_right_shift(bdfi_svg_float_dec *a, unsigned k) {
  int64_t r = 0, w = 0;
  uint64_t n = 0;
  /* pick up enough leading digits to cover first shift */
  for (; n >> k == 0; r++) {
    if (r >= a->nd) {
      if (n == 0) {
        a->nd = 0;
        return;
      }
      while (n >> k == 0) {
        n = n * 10;
        r++;
      }
      break;
    }
    n = n * 10 + (uint64_t)(a->d[r] - '0');
  }
  a->dp -= r - 1;
  uint64_t mask = ((uint64_t)1 << k) - 1;
  /* pick up a digit, put down a digit */
  for (; r < a->nd; r++) {
    uint64_t c = (uint64_t)(a->d[r] - '0');
    uint64_t dig = n >> k;
    n &= mask;
    a->d[w++] = (uint8_t)(dig + '0');
    n = n * 10 + c;
  }
  /* put down extra digits */
  while (n > 0) {
    uint64_t dig = n >> k;
    n &= mask;
    if (w < BDFI_SVG_FLOAT_DIGITS) {
      a->d[w++] = (uint8_t)(dig + '0');
    } else if (dig > 0) {
      a->trunc = 1;
    }
    n = n * 10;
  }
  a->nd = w;
  bdfi_svg_float_trim(a);
}

/* leftcheats: for a shift by k, the number of new digits it introduces, and
 * the leading digits of 5**k: one digit fewer for a number whose digits are
 * less than them. */
typedef struct bdfi_svg_float_cheat {
  int delta;
  const char *cutoff;
} bdfi_svg_float_cheat;

static const bdfi_svg_float_cheat bdfi_svg_float_cheats[BDFI_SVG_FLOAT_MAX_SHIFT + 1] = {
    {0, ""},
    {1, "5"},
    {1, "25"},
    {1, "125"},
    {2, "625"},
    {2, "3125"},
    {2, "15625"},
    {3, "78125"},
    {3, "390625"},
    {3, "1953125"},
    {4, "9765625"},
    {4, "48828125"},
    {4, "244140625"},
    {4, "1220703125"},
    {5, "6103515625"},
    {5, "30517578125"},
    {5, "152587890625"},
    {6, "762939453125"},
    {6, "3814697265625"},
    {6, "19073486328125"},
    {7, "95367431640625"},
    {7, "476837158203125"},
    {7, "2384185791015625"},
    {7, "11920928955078125"},
    {8, "59604644775390625"},
    {8, "298023223876953125"},
    {8, "1490116119384765625"},
    {9, "7450580596923828125"},
    {9, "37252902984619140625"},
    {9, "186264514923095703125"},
    {10, "931322574615478515625"},
    {10, "4656612873077392578125"},
    {10, "23283064365386962890625"},
    {10, "116415321826934814453125"},
    {11, "582076609134674072265625"},
    {11, "2910383045673370361328125"},
    {11, "14551915228366851806640625"},
    {12, "72759576141834259033203125"},
    {12, "363797880709171295166015625"},
    {12, "1818989403545856475830078125"},
    {13, "9094947017729282379150390625"},
    {13, "45474735088646411895751953125"},
    {13, "227373675443232059478759765625"},
    {13, "1136868377216160297393798828125"},
    {14, "5684341886080801486968994140625"},
    {14, "28421709430404007434844970703125"},
    {14, "142108547152020037174224853515625"},
    {15, "710542735760100185871124267578125"},
    {15, "3552713678800500929355621337890625"},
    {15, "17763568394002504646778106689453125"},
    {16, "88817841970012523233890533447265625"},
    {16, "444089209850062616169452667236328125"},
    {16, "2220446049250313080847263336181640625"},
    {16, "11102230246251565404236316680908203125"},
    {17, "55511151231257827021181583404541015625"},
    {17, "277555756156289135105907917022705078125"},
    {17, "1387778780781445675529539585113525390625"},
    {18, "6938893903907228377647697925567626953125"},
    {18, "34694469519536141888238489627838134765625"},
    {18, "173472347597680709441192448139190673828125"},
    {19, "867361737988403547205962240695953369140625"},
};

/* prefixIsLessThan: is the leading prefix of the digits less than s? */
static int bdfi_svg_float_prefix_less(const bdfi_svg_float_dec *a, const char *s) {
  for (int64_t i = 0; s[i] != '\0'; i++) {
    if (i >= a->nd) {
      return 1;
    }
    if (a->d[i] != (uint8_t)s[i]) {
      return a->d[i] < (uint8_t)s[i];
    }
  }
  return 0;
}

/* leftShift: binary shift left (* 2) by k bits, k <= maxShift. */
static void bdfi_svg_float_left_shift(bdfi_svg_float_dec *a, unsigned k) {
  int delta = bdfi_svg_float_cheats[k].delta;
  if (bdfi_svg_float_prefix_less(a, bdfi_svg_float_cheats[k].cutoff)) {
    delta--;
  }
  int64_t r = a->nd;         /* read index */
  int64_t w = a->nd + delta; /* write index */
  uint64_t n = 0;
  /* pick up a digit, put down a digit */
  for (r--; r >= 0; r--) {
    n += (uint64_t)(a->d[r] - '0') << k;
    uint64_t quo = n / 10;
    uint64_t rem = n - 10 * quo;
    w--;
    if (w < BDFI_SVG_FLOAT_DIGITS) {
      a->d[w] = (uint8_t)(rem + '0');
    } else if (rem != 0) {
      a->trunc = 1;
    }
    n = quo;
  }
  /* put down extra digits */
  while (n > 0) {
    uint64_t quo = n / 10;
    uint64_t rem = n - 10 * quo;
    w--;
    if (w < BDFI_SVG_FLOAT_DIGITS) {
      a->d[w] = (uint8_t)(rem + '0');
    } else if (rem != 0) {
      a->trunc = 1;
    }
    n = quo;
  }
  a->nd += delta;
  if (a->nd >= BDFI_SVG_FLOAT_DIGITS) {
    a->nd = BDFI_SVG_FLOAT_DIGITS;
  }
  a->dp += delta;
  bdfi_svg_float_trim(a);
}

/* decimal.Shift: binary shift left (k > 0) or right (k < 0). */
static void bdfi_svg_float_shift(bdfi_svg_float_dec *a, int64_t k) {
  if (a->nd == 0) {
    return;
  }
  if (k > 0) {
    while (k > BDFI_SVG_FLOAT_MAX_SHIFT) {
      bdfi_svg_float_left_shift(a, BDFI_SVG_FLOAT_MAX_SHIFT);
      k -= BDFI_SVG_FLOAT_MAX_SHIFT;
    }
    bdfi_svg_float_left_shift(a, (unsigned)k);
  } else if (k < 0) {
    while (k < -BDFI_SVG_FLOAT_MAX_SHIFT) {
      bdfi_svg_float_right_shift(a, BDFI_SVG_FLOAT_MAX_SHIFT);
      k += BDFI_SVG_FLOAT_MAX_SHIFT;
    }
    bdfi_svg_float_right_shift(a, (unsigned)-k);
  }
}

/* shouldRoundUp: if we chop a at nd digits, should we round up? */
static int bdfi_svg_float_round_up(const bdfi_svg_float_dec *a, int64_t nd) {
  if (nd < 0 || nd >= a->nd) {
    return 0;
  }
  if (a->d[nd] == '5' && nd + 1 == a->nd) { /* exactly halfway: round to even */
    if (a->trunc) {
      return 1;
    }
    return nd > 0 && (a->d[nd - 1] - '0') % 2 != 0;
  }
  return a->d[nd] >= '5';
}

/* decimal.RoundedInteger: the integer part, rounded. */
static uint64_t bdfi_svg_float_rounded(const bdfi_svg_float_dec *a) {
  if (a->dp > 20) {
    return UINT64_MAX;
  }
  int64_t i;
  uint64_t n = 0;
  for (i = 0; i < a->dp && i < a->nd; i++) {
    n = n * 10 + (uint64_t)(a->d[i] - '0');
  }
  for (; i < a->dp; i++) {
    n *= 10;
  }
  if (bdfi_svg_float_round_up(a, a->dp)) {
    n++;
  }
  return n;
}

static uint8_t bdfi_svg_float_lower(uint8_t c) {
  return (uint8_t)(c | ('x' - 'X'));
}

/* decimal.set in atof.go: the digits of a number that readFloat took for
 * one. Of the digits before the point it counts the 800 it keeps, not those
 * it drops: the port keeps that. */
static int bdfi_svg_float_set(bdfi_svg_float_dec *b, const char *s, size_t n) {
  size_t i = 0;
  b->neg = 0;
  b->trunc = 0;
  if (i >= n) {
    return 0;
  }
  if (s[i] == '+') {
    i++;
  } else if (s[i] == '-') {
    i++;
    b->neg = 1;
  }
  int sawdot = 0, sawdigits = 0;
  for (; i < n; i++) {
    if (s[i] == '_') {
      continue; /* readFloat already checked underscores */
    }
    if (s[i] == '.') {
      if (sawdot) {
        return 0;
      }
      sawdot = 1;
      b->dp = b->nd;
      continue;
    }
    if (s[i] >= '0' && s[i] <= '9') {
      sawdigits = 1;
      if (s[i] == '0' && b->nd == 0) { /* ignore leading zeros */
        b->dp--;
        continue;
      }
      if (b->nd < BDFI_SVG_FLOAT_DIGITS) {
        b->d[b->nd++] = (uint8_t)s[i];
      } else if (s[i] != '0') {
        b->trunc = 1;
      }
      continue;
    }
    break;
  }
  if (!sawdigits) {
    return 0;
  }
  if (!sawdot) {
    b->dp = b->nd;
  }
  if (i < n && bdfi_svg_float_lower((uint8_t)s[i]) == 'e') {
    i++;
    if (i >= n) {
      return 0;
    }
    int esign = 1;
    if (s[i] == '+') {
      i++;
    } else if (s[i] == '-') {
      i++;
      esign = -1;
    }
    if (i >= n || s[i] < '0' || s[i] > '9') {
      return 0;
    }
    int e = 0;
    for (; i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '_'); i++) {
      if (s[i] == '_') {
        continue;
      }
      if (e < 10000) {
        e = e * 10 + (s[i] - '0');
      }
    }
    b->dp += e * esign;
  }
  return i == n;
}

/* decimal.floatBits for a float64: the bits, and whether it overflowed. */
static uint64_t bdfi_svg_float_bits(bdfi_svg_float_dec *d, int *overflow) {
  /* decimal power of ten to binary power of two */
  static const int powtab[9] = {1, 3, 6, 9, 13, 16, 19, 23, 26};
  const int64_t bias = -1023;
  int64_t exp = 0;
  uint64_t mant = 0;
  *overflow = 0;
  if (d->nd == 0) {
    exp = bias;
  } else if (d->dp > 310) {
    *overflow = 1;
  } else if (d->dp < -330) {
    exp = bias; /* zero */
  } else {
    /* scale by powers of two until in range [0.5, 1.0) */
    while (d->dp > 0) {
      int n = d->dp >= 9 ? 27 : powtab[d->dp];
      bdfi_svg_float_shift(d, -n);
      exp += n;
    }
    while (d->dp < 0 || (d->dp == 0 && d->d[0] < '5')) {
      int n = -d->dp >= 9 ? 27 : powtab[-d->dp];
      bdfi_svg_float_shift(d, n);
      exp -= n;
    }
    /* our range is [0.5,1) but floating point range is [1,2) */
    exp--;
    /* the minimum representable exponent is bias+1: if the exponent is
     * smaller, move it up and adjust d accordingly */
    if (exp < bias + 1) {
      int64_t n = bias + 1 - exp;
      bdfi_svg_float_shift(d, -n);
      exp += n;
    }
    if (exp - bias >= 0x7FF) {
      *overflow = 1;
    } else {
      /* extract 1+mantbits bits */
      bdfi_svg_float_shift(d, 1 + 52);
      mant = bdfi_svg_float_rounded(d);
      /* rounding might have added a bit; shift down */
      if (mant == (uint64_t)2 << 52) {
        mant >>= 1;
        exp++;
        if (exp - bias >= 0x7FF) {
          *overflow = 1;
        }
      }
      /* denormalized? */
      if (!*overflow && (mant & ((uint64_t)1 << 52)) == 0) {
        exp = bias;
      }
    }
  }
  if (*overflow) {
    mant = 0;
    exp = 0x7FF + bias;
  }
  uint64_t bits = mant & (((uint64_t)1 << 52) - 1);
  bits |= ((uint64_t)(exp - bias) & 0x7FF) << 52;
  if (d->neg) {
    bits |= (uint64_t)1 << 63;
  }
  return bits;
}

/* ---- Reading a number (atof.go) ------------------------------------------- */

/* commonPrefixLenIgnoreCase: prefix is lower case. */
static size_t bdfi_svg_float_common(const char *s, size_t n, const char *prefix, size_t plen) {
  size_t m = n < plen ? n : plen;
  for (size_t i = 0; i < m; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c + ('a' - 'A'));
    }
    if (c != prefix[i]) {
      return i;
    }
  }
  return m;
}

/* special: inf, infinity and NaN, possibly signed, at the start of s. */
static int bdfi_svg_float_special(const char *s, size_t n, double *f, size_t *used) {
  if (n == 0) {
    return 0;
  }
  int neg = 0;
  size_t nsign = 0;
  if (s[0] == '+' || s[0] == '-' || s[0] == 'i' || s[0] == 'I') {
    if (s[0] == '+' || s[0] == '-') {
      neg = s[0] == '-';
      nsign = 1;
    }
    size_t m = bdfi_svg_float_common(s + nsign, n - nsign, "infinity", 8);
    /* anything longer than "inf" is ok, but without all of "infinity" only
     * "inf" is consumed */
    if (m > 3 && m < 8) {
      m = 3;
    }
    if (m == 3 || m == 8) {
      *f = bdfi_svg_float_from_bits(neg ? 0xFFF0000000000000ULL : 0x7FF0000000000000ULL);
      *used = nsign + m;
      return 1;
    }
  } else if (s[0] == 'n' || s[0] == 'N') {
    if (bdfi_svg_float_common(s, n, "nan", 3) == 3) {
      *f = bdfi_svg_float_from_bits(0x7FF8000000000001ULL);
      *used = 3;
      return 1;
    }
  }
  return 0;
}

/* underscoreOK in atoi.go: underscores must separate digits, after a base
 * prefix. */
static int bdfi_svg_float_underscore_ok(const char *s, size_t n) {
  char saw = '^'; /* ^ the beginning, 0 a digit or the prefix, _ an underscore, ! none of them */
  size_t i = 0;
  if (n >= 1 && (s[0] == '-' || s[0] == '+')) {
    s++;
    n--;
  }
  int hex = 0;
  if (n >= 2 && s[0] == '0') {
    uint8_t l = bdfi_svg_float_lower((uint8_t)s[1]);
    if (l == 'b' || l == 'o' || l == 'x') {
      i = 2;
      saw = '0';
      hex = l == 'x';
    }
  }
  for (; i < n; i++) {
    uint8_t l = bdfi_svg_float_lower((uint8_t)s[i]);
    if ((s[i] >= '0' && s[i] <= '9') || (hex && l >= 'a' && l <= 'f')) {
      saw = '0';
      continue;
    }
    if (s[i] == '_') {
      if (saw != '0') {
        return 0;
      }
      saw = '_';
      continue;
    }
    if (saw == '_') {
      return 0;
    }
    saw = '!';
  }
  return saw != '_';
}

/* What readFloat reads of a number. */
typedef struct bdfi_svg_float_num {
  uint64_t mantissa;
  int64_t exp;
  int neg, trunc, hex;
} bdfi_svg_float_num;

/* readFloat: a decimal or hexadecimal mantissa and exponent at the start of
 * s. It returns whether the number is valid, and in *used the bytes read. */
static int bdfi_svg_float_read(const char *s, size_t n, bdfi_svg_float_num *out, size_t *used) {
  size_t i = 0;
  int underscores = 0;
  out->mantissa = 0;
  out->exp = 0;
  out->neg = out->trunc = out->hex = 0;
  *used = 0;
  if (i >= n) {
    return 0;
  }
  if (s[i] == '+') {
    i++;
  } else if (s[i] == '-') {
    i++;
    out->neg = 1;
  }
  uint64_t base = 10;
  int max_mant_digits = 19; /* 10^19 fits in uint64 */
  uint8_t exp_char = 'e';
  if (i + 2 < n && s[i] == '0' && bdfi_svg_float_lower((uint8_t)s[i + 1]) == 'x') {
    base = 16;
    max_mant_digits = 16; /* 16^16 fits in uint64 */
    i += 2;
    exp_char = 'p';
    out->hex = 1;
  }
  int sawdot = 0, sawdigits = 0;
  int64_t nd = 0, dp = 0;
  int nd_mant = 0;
  for (; i < n; i++) {
    uint8_t c = (uint8_t)s[i];
    uint8_t l = bdfi_svg_float_lower(c);
    if (c == '_') {
      underscores = 1;
      continue;
    }
    if (c == '.') {
      if (sawdot) {
        break;
      }
      sawdot = 1;
      dp = nd;
      continue;
    }
    if (c >= '0' && c <= '9') {
      sawdigits = 1;
      if (c == '0' && nd == 0) { /* ignore leading zeros */
        dp--;
        continue;
      }
      nd++;
      if (nd_mant < max_mant_digits) {
        out->mantissa = out->mantissa * base + (uint64_t)(c - '0');
        nd_mant++;
      } else if (c != '0') {
        out->trunc = 1;
      }
      continue;
    }
    if (base == 16 && l >= 'a' && l <= 'f') {
      sawdigits = 1;
      nd++;
      if (nd_mant < max_mant_digits) {
        out->mantissa = out->mantissa * 16 + (uint64_t)(l - 'a' + 10);
        nd_mant++;
      } else {
        out->trunc = 1;
      }
      continue;
    }
    break;
  }
  *used = i;
  if (!sawdigits) {
    return 0;
  }
  if (!sawdot) {
    dp = nd;
  }
  if (base == 16) {
    dp *= 4;
    nd_mant *= 4;
  }
  /* The optional exponent moves the decimal point. Of a very long one the
   * first five digits or so are taken. */
  if (i < n && bdfi_svg_float_lower((uint8_t)s[i]) == exp_char) {
    i++;
    *used = i;
    if (i >= n) {
      return 0;
    }
    int esign = 1;
    if (s[i] == '+') {
      i++;
    } else if (s[i] == '-') {
      i++;
      esign = -1;
    }
    *used = i;
    if (i >= n || s[i] < '0' || s[i] > '9') {
      return 0;
    }
    int e = 0;
    for (; i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '_'); i++) {
      if (s[i] == '_') {
        underscores = 1;
        continue;
      }
      if (e < 10000) {
        e = e * 10 + (s[i] - '0');
      }
    }
    dp += e * esign;
  } else if (base == 16) {
    return 0; /* must have exponent */
  }
  *used = i;
  if (out->mantissa != 0) {
    out->exp = dp - nd_mant;
  }
  if (underscores && !bdfi_svg_float_underscore_ok(s, i)) {
    return 0;
  }
  return 1;
}

/* atofHex for a float64: the mantissa and the exponent of a hexadecimal
 * number, rounded. It returns 1 when the result is an infinity. */
static int bdfi_svg_float_hex(const bdfi_svg_float_num *num, double *out) {
  const int64_t max_exp = 1023, min_exp = -1022, bias = -1023;
  uint64_t mantissa = num->mantissa;
  int64_t exp = num->exp + 52; /* mantissa now implicitly divided by 2^52 */
  /* Shift mantissa and exponent to bring the representation into float
   * range: a leading 1-bit, 52 other bits, and two more for rounding, the
   * bottom one saying whether that bit or any later one was non-zero. */
  while (mantissa != 0 && mantissa >> (52 + 2) == 0) {
    mantissa <<= 1;
    exp--;
  }
  if (num->trunc) {
    mantissa |= 1;
  }
  while (mantissa >> (1 + 52 + 2) != 0) {
    mantissa = (mantissa >> 1) | (mantissa & 1);
    exp++;
  }
  /* if the exponent is too negative, denormalize in hopes of making it
   * representable (the -2 is for the rounding bits) */
  while (mantissa > 1 && exp < min_exp - 2) {
    mantissa = (mantissa >> 1) | (mantissa & 1);
    exp++;
  }
  /* round using the two bottom bits */
  uint64_t round = mantissa & 3;
  mantissa >>= 2;
  round |= mantissa & 1; /* round to even (round up if mantissa is odd) */
  exp += 2;
  if (round == 3) {
    mantissa++;
    if (mantissa == (uint64_t)1 << (1 + 52)) {
      mantissa >>= 1;
      exp++;
    }
  }
  if (mantissa >> 52 == 0) { /* denormal or zero */
    exp = bias;
  }
  int range = 0;
  if (exp > max_exp) { /* infinity and range error */
    mantissa = (uint64_t)1 << 52;
    exp = max_exp + 1;
    range = 1;
  }
  uint64_t bits = mantissa & (((uint64_t)1 << 52) - 1);
  bits |= ((uint64_t)(exp - bias) & 0x7FF) << 52;
  if (num->neg) {
    bits |= (uint64_t)1 << 63;
  }
  *out = bdfi_svg_float_from_bits(bits);
  return range;
}

/* atof64: the number at the start of s, and in *used the bytes it took. */
static int bdfi_svg_float_atof(const char *s, size_t n, double *out, size_t *used) {
  if (bdfi_svg_float_special(s, n, out, used)) {
    return 0;
  }
  bdfi_svg_float_num num;
  *out = 0;
  if (!bdfi_svg_float_read(s, n, &num, used)) {
    return -1;
  }
  if (num.hex) {
    return bdfi_svg_float_hex(&num, out);
  }
  uint64_t sign = num.neg ? (uint64_t)1 << 63 : 0;
  if (num.mantissa == 0) {
    *out = bdfi_svg_float_from_bits(sign);
    return 0;
  }
  if (num.exp > BDFI_SVG_FLOAT_POW_MAX) { /* overflow to an infinity */
    *out = bdfi_svg_float_from_bits(sign | ((uint64_t)0x7FF << 52));
    return 1;
  }
  if (num.exp < BDFI_SVG_FLOAT_POW_MIN) { /* underflow to zero */
    *out = bdfi_svg_float_from_bits(sign);
    return 0;
  }
  int range = bdfi_svg_float_scale(num.mantissa, (int)num.exp, sign, out);
  if (!num.trunc) {
    return range;
  }
  /* If digits were dropped from the mantissa but the mantissa and one more
   * convert to the same number, the digits do not matter. */
  double up;
  uint64_t a, b;
  (void)bdfi_svg_float_scale(num.mantissa + 1, (int)num.exp, sign, &up);
  memcpy(&a, out, sizeof a);
  memcpy(&b, &up, sizeof b);
  if (a == b) {
    return range;
  }
  /* slow fallback */
  bdfi_svg_float_dec dec;
  dec.nd = 0;
  dec.dp = 0;
  dec.d[0] = 0;
  if (!bdfi_svg_float_set(&dec, s, *used)) {
    *out = 0;
    return -1;
  }
  int overflow;
  *out = bdfi_svg_float_from_bits(bdfi_svg_float_bits(&dec, &overflow));
  return overflow;
}

int bdfi_parse_float(const char *s, size_t n, double *out) {
  size_t used = 0;
  int r = bdfi_svg_float_atof(s, n, out, &used);
  if (used != n) {
    *out = 0;
    return -1;
  }
  return r;
}
