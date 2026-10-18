# セキュリティとピークメモリの監査記録

実施日: 2026-10-18。比較元: `1e1d433`。優先順位は安全性、ピークメモリ、実行速度の順とした。公開 API のシグネチャと ABI は変更していない。大きすぎる部品の拒否は新しい動作制限になる。

## 調査と修正

自前の C 実装 42 ファイルを対象に、入力サイズと整数演算、メモリの所有権、確保失敗時の後始末、展開処理、キャッシュの寿命、暗号の表引きを確認した。静的解析、既存の参照実装との比較、サニタイザー、ファジングに加え、確保を順番に失敗させる試験を追加した。

| 問題 | 修正と確認 |
| --- | --- |
| 部品配列を `realloc` すると、画像キャッシュの所有者ポインタが古い領域を参照する | 配列移動後に所有者と状態フラグの参照先を更新。比較元で ASan の heap-use-after-free を再現し、常に移動するアロケーターで修正後を確認した |
| オブジェクトキャッシュの初期化途中で確保に失敗すると、解放処理が NULL 配列を走査する | 配列の存在を確認してから走査。比較元で ASan/UBSan の NULL 参照を再現した |
| WOFF2 の出力拡張に失敗すると、4 バイト境界へのパディングが無限ループになる | パディングを最大 3 バイトの一括追加に変更。203 点のグリフと確保失敗で比較元の停止を再現した |
| WOFF2 座標の累積加算が符号付き整数をオーバーフローする | 64 ビットで加算して 32 ビット範囲を検査。正負それぞれの境界を越える入力を回帰テストにした |
| JSON ノードの確保サイズ計算が 32 ビット環境で桁あふれし得る | 検査付き配列確保に統一。共通確保関数でも `PTRDIFF_MAX` を越える積を拒否し、文字列終端の `n + 1` も検査する |
| AES の S-box と GHASH が秘密値で表を引く | AES に BearSSL の Boolean S-box 回路を取り込み、GHASH を固定 128 ステップのマスク演算へ変更。秘密値に依存する表アクセスを削除した |
| 巨大な部品や WOFF2 再構成出力を要求できる | 部品の保存サイズ・展開サイズと WOFF2 出力に設定可能な上限を適用。部品は読み込みコールバックを呼ぶ前にも検査する |
| GCM の入力長がカウンター・ビット長の表現範囲を越える | 規格の上限を処理前に検査。32 ビット環境でも最終ブロックのオフセットが折り返さないよう更新する |

AES の変更はキャッシュ表引きによる漏洩を減らすためのもの。生成機械語の定数時間性、電力解析・電磁波解析への耐性、暗号認証を保証するものではない。AES 回路の出典、原本の SHA-256、MIT ライセンスは [third_party/bearssl](../third_party/bearssl/README.md) に記録した。追加のリンク依存はない。

## メモリを減らした箇所

- PNG は展開した全走査線を保持せず、最大 32 KiB の deflate 履歴窓と 2 行の作業領域を使う。最終 RGBA 画像は従来どおり確保する。Adam7、フィルター、CRC、Adler32、入力終端の検査を維持した。
- WOFF2 は固定約 704 KiB のグリフ作業領域をやめ、そのフォントで必要になった最大の点数・輪郭数まで確保する。空グリフと複合グリフだけなら、この領域は不要。輪郭終端は 16 ビットで保持する。
- 部品索引はハッシュと添字のコピーを作らず、添字だけを in-place heapsort する。重複名の「最後が有効」という動作を維持し、検索も線形の巻き戻しをなくして O(log n) にした。
- arena の標準チャンクを 64 KiB から 4 KiB に縮小した。大きな要求は必要な大きさで確保し、チャンクサイズはビルド時に変更できる。
- 暗号化部品はタグ検証を完了した後、保存データの領域内で復号する。平文全体を保持するためだけの 2 本目の確保をなくした。タグ不一致なら平文を書かない。
- deflate は非圧縮区間を `memcpy` で転送し、後方参照は出力領域を区間単位で確保する。
- 丸い線端・接合部は出力配列へ直接書き込み、256 点・4 KiB のスタック上の一時配列をなくした。浮動小数点の丸めを含む従来の向き判定を維持した。

### 測定値

`tests/memory_internal.c --measure` で同一の入力とアロケーターを比較した。数値は同時に生存する**要求ヒープバイト数**の最大値であり、プロセス RSS ではない。入力 fixture、スタック、アロケーターの管理領域、Brotli 自身の確保は含めない。`realloc` は必ず別領域へ移し、移動中の新旧領域の重複も数える。以下は Brotli なしの single-header ビルドである。

| 測定ケース | 比較元 | 修正後 | 削減率 |
| --- | ---: | ---: | ---: |
| 1024 × 1024、RGBA 16-bit PNG の復号 | 16,779,360 B | 4,244,578 B | 74.7% |
| WOFF2 の 3 点グリフ再構成 | 721,928 B | 1,061 B | 99.85% |
| テスト文書の読み込み・文字抽出・検索・32 × 32 描画 | 143,508 B | 53,961 B | 62.4% |
| 4096 部品の索引構築だけ | 131,072 B | 32,768 B | 75.0% |
| テストオブジェクトの等倍描画 | 626,734 B | 626,734 B | 0% |

PNG の確保回数は 19 回から 5 回、索引構築は 2 回から 1 回。GCM コンテキストの `sizeof` は 1,136 B から 256 B になった。小さなグリフの削減率はフォント全体や最大サイズのグリフの削減率ではない。

測定環境は macOS arm64、Homebrew Clang 23.1.0、`-std=c11 -O2 -ffp-contract=off`。`--bench` を各版 5 回実行した CPU 時間の中央値は次のとおり。

| 処理 | 比較元 | 修正後 |
| --- | ---: | ---: |
| 上記 PNG の復号・画素検査（1 回につき 10 画像） | 30.480 ms/画像 | 23.713 ms/画像 |
| AES-256 の 65,536 ブロック処理 | 7.282 ms/MiB | 40.406 ms/MiB |

この PNG では約 22% 短縮した。一方、秘密値による表引きをなくした portable AES は約 5.5 倍の時間を要する。AES 単体の値なので、GCM やパスワード解除全体の速度を表すものではない。組み込み CPU の性能はこの測定から推定しない。

同じコンパイラーと最適化で `-fstack-usage` を付けた single-header オブジェクトでは、`bdfi_stroker_round` のフレームは **4,240 B → 160 B**。変更後の最大の単独フレームは `bdf_text_search_new` の 2,848 B だった。呼び出し階層、再帰、外部ライブラリを合算した最大スタック使用量ではないため、RTOS タスクのスタック設定には実機での確認が必要となる。

## 検証

- ASan + UBSan、警告をエラーにするビルドで CTest **40/40 成功**。分割ソース、single header、回帰入力の再生を含む。
- Brotli なしの Release ビルドでも **40/40 成功**。
- `go test -count=1 ./...` が成功。Go 参照実装との画像・暗号・文書・SVG 等の比較を含む。
- 確保失敗の試験は、各確保を一度だけ失敗させる場合と、その確保以降すべてを失敗させる場合を走査。PNG、WOFF2、文書・文字・検索、索引、描画、キャッシュ移動、フォント、SVG について、生存ブロックが残らないことを検査した。
- 上記 PNG は 5 MiB 以下、小さいグリフは 16 KiB 以下、索引は添字配列 1 本以下というピークメモリの回帰検査も追加した。`--measure` では比較元も測れるよう、この上限検査と確保失敗の走査を省略する。
- ストリーム展開を 1 / 7 / 4096 バイトの入力分割、32 KiB の最大後方参照、履歴窓の折り返し、出力拒否で検査した。
- 全 13 個の libFuzzer ターゲットを ASan + UBSan で実行。比較元側の調査段階では各 90 秒で 48,477,886 回、修正段階では各 120 秒で 49,960,339 回実行し、クラッシュ・サニタイザー検出はなかった。最大入力長は 131,072 バイト。ファジング用の計算量上限を適用しているため、通常上限付近の負荷試験を代替するものではない。
- 最終調整後、`unlock` / `woff2` / `inflate` / `image` / `render` を各 60 秒、計 11,054,138 回追加実行して成功した。描画のスタック削減後には CTest 全件と Go 比較試験も再実行した。
- Clang Static Analyzer を自前の 42 ファイルに適用。初回 18 件の警告のうち、確保失敗時のキャッシュ解放を実不具合として再現・修正した。最終解析には 17 件残る。JSON の失敗戻り値や初期化、空配列と件数の関係、描画領域と確保の関係などを追跡したが、到達可能な不正入力としては再現していない。警告ゼロ、全経路の証明という意味ではない。
- 小さいリソース上限を指定した MinSizeRel ビルドが成功。CMake install のライセンス同梱、配布スクリプトの構文、Conan の Python 構文、.NET の XML も確認した。

32 ビット Linux の `-m32` + ASan/UBSan ジョブを CI に追加した。今回のローカル環境では実行していない。Android、iOS、Windows、実機 MCU/RTOS での今回の変更の再検証も未実施である。

## 組み込み向けの設定

既定値は汎用機向けに大きい。例えば描画上限は 64 Mi ピクセルであり、浮動小数点キャンバスだけで 1 GiB になり得る。用途に合わせて低く設定する。

以下はビルド確認済みの設定例。RAM 数 MiB 以内を保証する設定ではなく、搭載 RAM と受け入れる文書に合わせてさらに調整する出発点である。

```sh
cmake -S . -B build-small \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DBDF_BUILD_TESTS=OFF -DBDF_WITH_BROTLI=OFF -DBDF_WERROR=ON \
  -DBDF_MAX_MANIFEST_SIZE=1048576 \
  -DBDF_MAX_PART_SIZE=4194304 \
  -DBDF_MAX_WOFF2_SIZE=4194304 \
  -DBDF_MAX_RENDER_PIXELS=65536 \
  -DBDF_MAX_DECODE_PIXELS=1048576 \
  -DBDF_MAX_PICTURE_PIXELS=2097152 \
  -DBDF_MAX_SVG_RASTER_PIXELS=65536 \
  -DBDF_MAX_JSON_DEPTH=64 \
  -DBDF_MAX_KDF_ITERATIONS=1000000
cmake --build build-small --parallel
```

`BDF_WITH_BROTLI=OFF` では WOFF2 フォントを直接開けない。必要なら ON にするか、対応する非圧縮フォントを用意する。JSON の深さや KDF 回数の上限を下げると、それを越える正当な文書も拒否される。KDF の計算を途中で切り上げて弱める設定ではない。

CMake では正の十進整数を指定し、空欄なら既定値を使う。single header では `BDF_IMPLEMENTATION` を含む翻訳単位のコンパイラー定義、または include 前の `#define` で同じ設定を渡す。分割ソースも全翻訳単位に同じ値を指定する。

| 設定 | 単位・意味 |
| --- | --- |
| `BDF_MAX_MANIFEST_SIZE` | manifest の入力バイト数 |
| `BDF_MAX_PART_SIZE` | 部品 1 個の保存バイト数と展開バイト数。既定 256 MiB。超過は `BDF_ERR_TOO_LARGE` |
| `BDF_MAX_WOFF2_SIZE` | WOFF2 の展開テーブル・再構成出力の上限。既存の WOFF2 処理に合わせ、超過は `BDF_ERR_FORMAT` |
| `BDF_MAX_RENDER_PIXELS` | 描画出力の画素数 |
| `BDF_MAX_DECODE_PIXELS` | 画像復号の画素数 |
| `BDF_MAX_PICTURE_PIXELS` | 復号済み画像キャッシュの画素予算 |
| `BDF_MAX_SVG_RASTER_PIXELS` | SVG 内部ラスタライズの基準画素数。スケール丸めで最大約 2 倍になり得る |
| `BDF_MAX_JSON_DEPTH` | JSON の入れ子深度。再帰によるスタック消費も抑える |
| `BDF_ARENA_CHUNK_SIZE` | arena の標準確保バイト数。既定 4096 |
| `BDF_MAX_KDF_ITERATIONS` | パスワード鍵導出の最大反復回数 |
| `BDF_MAX_REUSED_INSTRUCTIONS`, `BDF_MAX_REUSED_DRAWN` | 再利用オブジェクトの処理量 |
| `BDF_MAX_SHADOW_BLUR`, `BDF_MAX_JPEG_SCANS`, `BDF_MAX_SVG_ELEMENTS` | ぼかし、JPEG scan、SVG 描画要素数の処理量 |

描画は `bdf_render_region` で必要な領域だけを描く。256 × 256 でも基本キャンバスは約 1 MiB、最終 RGBA は 256 KiB 必要になる。32 × 32 ならそれぞれ 16 KiB と 4 KiB。透明レイヤー、マスク、影、フォント、展開したオブジェクト、画像キャッシュ、呼び出し側が保持する入力は別途加算される。

個別リソースの上限はライブラリ全体のヒープ上限ではない。確実な総 RAM 制限、実行時間制限、スタック上限が必要な製品では、実文書による実機測定と、呼び出し側の割当予算・処理期限の管理が引き続き必要となる。今回、総量を管理する新しい allocator API やハードウェア AES バックエンドは追加していない。

## 再実行

```sh
cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug \
  -DBDF_WERROR=ON -DBDF_SANITIZE=address,undefined
cmake --build build-check --parallel
ctest --test-dir build-check --output-on-failure
go -C go test -count=1 ./...

cmake -S . -B build-measure -DCMAKE_BUILD_TYPE=Release \
  -DBDF_WITH_BROTLI=OFF
cmake --build build-measure --target test_memory_internal --parallel
./build-measure/tests/test_memory_internal --measure

# 上記の時間測定と同じ最適化オプションで測る場合
clang -std=c11 -O2 -ffp-contract=off -Ibuild-measure/single \
  tests/memory_internal.c -o /tmp/libbdf-memory-bench
/tmp/libbdf-memory-bench --bench
```

比較元の測定も、同じ `tests/memory_internal.c` を比較元から生成した single header に対してコンパイルし、`--measure` / `--bench` で実行した。引数なしでは修正済み不具合の確保失敗テストも走るため、比較元は正常終了しない。

暗号の検査条件は [FIPS 197](https://csrc.nist.gov/pubs/fips/197/final)、[SP 800-38D](https://csrc.nist.gov/pubs/sp/800/38/d/final)、[BearSSL の定数時間実装の説明](https://www.bearssl.org/constanttime.html) を参照した。
