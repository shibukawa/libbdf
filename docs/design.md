# libbdf の設計メモ

bdf（Go の参照実装と TypeScript のビューア）とは別に、ネイティブのアプリケーションから BDF を扱うための C のライブラリを作る。このメモは、なぜこの形にしたかの記録で、仕様そのものは bdf の `docs/spec.md` にある。

## 1. 目的

- Skia、cairo、QPainter、CoreGraphics のような描画 API を持つアプリケーション（Qt、GTK、.NET MAUI、Cocoa）から、BDF v1 をそのまま開いて描けること。
- 利用者がアップロードした文書を描く用途を含むので、不正な入力に対して参照実装と同じ上限を守り、メモリ安全であること。
- 既存の wire format と opset を変えない。libbdf は読み手であり、書き手ではない。

## 2. 層

```
core      コンテナ（single/split）、manifest（自前の JSON）、Part、deflate-raw（自前）、
          Object のデコード、WOFF2 → SFNT、テキスト run（選択・検索・リンク）、暗号化の開錠、区間
raster    純粋な C のラスタライザ。bdf の raster/imagebdf の移植。FreeType で字形を取る
adapters  core の命令列をフレームワークのキャンバスに流す薄い層
          skia/ (C++)  cairo/ (C)  qt/ (C++)  coregraphics/ (ObjC)
```

core は形式だけを知り、描画を知らない。adapters は描画だけを知り、形式を知らない。raster は adapters の 1 つでもあるが、フレームワークに依存しない唯一の描き手なので、どの環境でも同じ絵を出す基準になる。

core のうち今あるのは読みの部分で、libc 以外に依存しない。JSON の読み手と inflate を自前で持つのは、依存を増やさないためと、単一ヘッダに収めるためである（§6）。例外は WOFF2 の Brotli で、google/brotli 1.1.0 のデコーダだけを `third_party/brotli` に vendoring した（MIT、無改変、出所と SHA-256 は同梱の README）。`BDF_WITH_BROTLI`（既定 ON）で入り、OFF のビルドは WOFF2 を `BDF_ERR_UNSUPPORTED` で断る。WOFF2 の復元そのものは Go の `font/woff2/decode.go` の移植で、glyf と loca の再構成（loca は長形式、`head.indexToLocFormat` を 1 に）、hmtx の再構成、タグ順の並べ替えと 4 バイト境界、チェックサムまで同じにしてあり、参照実装と同じバイト列を出す。Brotli の流れは、ディレクトリが言う大きさとその 1 バイト先までしか読まない（参照実装の `decompress` と同じで、短い流れと末尾の欠けた流れは拒否し、長い分は見ない）。

## 3. 参照実装との関係

- bdf の Go 実装が参照実装で、libbdf は検査される側である。`go/` の cgo ハーネスが同じバイト列を両方に通し、受理と拒否、manifest の構造、Part の復号したバイト列、資源表、命令列、エラーの位置と文言まで一致することを要求する。
- 入力は固定した版の bdf モジュールの testdata（`go/go.mod` の require）。全変換器の出力が揃っている。
- 依存は libbdf → bdf の一方向。bdf は libbdf を知らない。
- デコーダは Go の `decode.go`、`buf.go`、`path.go`、`paint.go`、`font.go`、`reader.go`、`manifest.go` を検査の順序まで含めて写した。Go の reader は最初の失敗を覚えて以後の読み取りを 0 にする。C の `bdfi_reader` も同じ振る舞いにしてあり、だから切り詰めた入力でも同じ位置・同じ文言で失敗する。
- manifest は encoding/json が参照実装の構造体を埋めるのと同じ規則で読む。知らないメンバーは無視、null は無いのと同じ、型の違うメンバーはエラー、重複したメンバーは最後が効く、整数のフィールドは整数リテラルだけ、float32 の範囲を超える数はエラー。合わせないと決めたのは、encoding/json がメンバー名を大文字小文字を無視して照合することと、base64 の中の改行を無視すること。どちらも仕様にない encoding/json の癖で、libbdf はそこだけ厳しい。差分 fuzz（`FuzzDoc`）は「libbdf が受理するものを参照実装が拒否しない」ことと「両方が開いた文書の Part が同じに読める」ことを要求し、逆向きは要求しない。
- テキストの抽出（`src/text.c`）は Go の `text.go` の移植で、歩き方、区切り（MARK と位置からの推定）、ALT_TEXT の扱い、上限（USE の深さ 64、再利用した Object から読む命令 2^27、run 2^22）、壊れた子 Object の扱い（そこまでの run を残して親の走査を続ける）まで同じにした。同じ上限を View の最上位 Object で数え続けること（`bdfi_use_limits`）も同じである。リンク（LINK の矩形の外接矩形と url）は Go の参照実装になく、TS の `text.ts` と `textlayer.ts` に合わせた。読み上げ用の構造（見出し・リスト・表・図）はまだ移していない。
- **位置の計算は積和を融合しない**。行列の積と点の変換は `a*b + c*d + e` の形で、コンパイラはこれを FMA（1 回だけ丸める積和）にしてよいことになっている。Go は arm64 で融合し、amd64 では融合しない。つまり参照実装自身がアーキテクチャで最後のビットが違う。libbdf は積を `volatile float` に置いてから足すことで、どのコンパイラ・どの CPU でも融合させない。位置が環境で変わらないことを優先し、参照実装とは amd64 でビット単位に一致する（bdf の fixture も amd64 を基準にしている）。差分テストは amd64 ではビット一致、arm64 では許容差つきで位置を比べ、位置から推定した区切りは「位置がビット一致なら一致」を要求する。Apple silicon では `GOARCH=amd64 CGO_ENABLED=1 go test` が Rosetta 経由で厳密な方を回す。
- Part 名の集合（どの子を歩いたか）は crit-bit 木で持つ。名前は文書が自由に選べるので、ハッシュ表だと衝突させて遅くできる。木なら 1 回 128 段までで済む。文書の Object のキャッシュは manifest の Part の番号で引く配列にして、表そのものを持たない。
- View のテキスト（`bdf_doc_view_text`）は `indexRuns`、検索用テキスト（`bdf_doc_search_text`）は `SearchText` の移植。シートのタイルのキーは参照実装が書く形（符号なし、先頭の 0 なしの `x,y`）だけを受け付ける。参照実装は `fmt.Sscanf` で読むので `01,0` のような形も通るが、その先で名前の引き直しに失敗するか同じタイルを 2 度読むかで、意味のある結果にならない。正規化つきの検索（NFKC、大文字小文字、かな）は Unicode の表が要るのでまだ入れていない。ホストの正規化（ICU、Foundation、Qt、.NET）で `bdf_doc_view_text` の結果を引けば足りる。
- inflate は参照実装（klauspost の flate）と受理が揃うようにした。不完全な Huffman 符号は長さ 1 の符号が 1 つだけの場合に限って受け付け、最後のブロックの後ろは読まない。`FuzzInflate` は両方向の一致を要求する。

## 4. C ABI の規則

`include/bdf/bdf.h` が ABI のすべてで、C++、Swift、C#（P/Invoke）、Go（cgo でも purego でも）、Python から束縛できるように次の規則を守る。

- ハンドルは不透明ポインタで、対になる `*_free` を持つ。
- 構造体を値で渡したり返したりしない。小さな POD（`bdf_path`、`bdf_paint`、`bdf_font`、`bdf_view`、`bdf_page`、`bdf_part_entry`、`bdf_header`、`bdf_error`）はポインタ経由で埋め、メモリを所有しない。
- 文字列は長さつきの UTF-8。便宜のため NUL も終端に置くが、中に NUL を含みうる。
- 大域状態を持たない。別スレッドの別文書は互いに触れない。
- 命令の読み取りは反復子（`bdf_instr`）と型つきのアクセサで行い、共用体を ABI に出さない。
- 入力はコールバック（`bdf_source`）で供給する。確保は実際のファイルの大きさに従い、manifest が言う大きさには従わない（参照実装と同じ）。コールバックは「大きさを尋ねる」と「埋める」の 2 段で、境界をまたいでメモリの所有を渡さない。
- `bdf_error.message` は静的文字列。参照実装が数値を埋め込む 3 つの文言（opset、flags、opcode）は数値なしにしてある。

## 5. テストの方針

- **単体テスト**（`tests/`、ctest）: 参照実装の `bdf_test.go` と `malformed_test.go` の事例を写したもの。切り詰め、未知の opcode、巨大な個数、不正な参照、manifest の数値の範囲、Part の範囲、inflate の上限。
- **サニタイザ**: CI は単体テストを ASan と UBSan 付きでもう 1 度回す。C ではこれが最も安い安全網で、既存の入力で起きるメモリ破壊の大半がここで出る。
- **差分テスト**（`go/`）: testdata の全文書、全 Part、全 Object、全フォントを Go と C の両方で読んで比べる。Linux では `go test -asan` で C 側も計装する。cgo が C ソースを直接コンパイルするので、ハーネスとライブラリは常に同じコミットになる。`go/` にあるのは `src/` と `third_party/brotli` の C ファイルへのシンボリックリンクで、`#include` で束ねたラッパーではない。Go のビルドキャッシュはパッケージ内のファイルの中身しか見ず、include 先の変更に気づかないので、ラッパー方式では C を直しても古いオブジェクトで `go test` が通ってしまう（実際に一度起きた）。リンクなら中身を読むのでキャッシュが追従する。
- **適合テスト**: bdf の `bdf conformance` が書く suite を `tests/conformance.c` が Go なしで流す（§7）。
- **fuzz は明示実行**: CI では回さない。探索は時間が青天井で CI に向かないという判断で、bdf の運用（パーサを変えたときに数分回す）と同じにした。探索は 2 系統ある。カバレッジ誘導とリーク検出は C の libFuzzer（`fuzz/`、object・inflate・manifest・doc・woff2・text・unlock の 7 本）、Go の参照実装との差分は `go test -fuzz`（`FuzzObject`、`FuzzInflate`、`FuzzDoc`、`FuzzWOFF2`、`FuzzText`、`FuzzIndexPart`、`FuzzUnlock`、`FuzzSealed`、`FuzzNFC`、`FuzzGCM`、`FuzzP256`）。互いを何度も描く Object は「再利用した命令 2^27」の上限で止まるが、そこまで数秒かかる。fuzz のビルドは `BDF_MAX_REUSED_INSTRUCTIONS` を 65536 に下げ、単体テストは内部の `max_reused` を下げて、この経路を速く通す。見つかった入力は手で `fuzz/regress/<target>/`（ctest の `replay-<target>` が流す）か `go/testdata/fuzz/`（`go test` が流す）に入れて再生テストにする。`fuzz/corpus/` は fuzzer が育てる作業用のコーパスで、コミットしない。CI の `fuzz` ジョブは `workflow_dispatch` で分数を指定したときだけ動く。`go test -fuzz` は新しい経路に届いた入力を 1 件あたり最大 60 秒かけて最小化するので、文書まるごとを入力にする `FuzzUnlock` は実行数が伸びず止まって見える（0 execs/sec）。`-fuzzminimizetime 20x` を付けて回す。

cgo を選んで purego にしなかったのは、`go build -asan` が cgo にしか効かないことと、同じツリーの C ソースを静的に結合したかったからである。利用者向けに cgo なしの Go 束縛を出すなら、そのときは purego で別に作る。

## 6. 単一ヘッダとの両立

配布形態の 1 つとして、コア層を 1 本の `bdf.h` にできるようにしてある。開発は分割ソースのままで、`tools/amalgamate.py` が公開ヘッダ、内部ヘッダ、ソースをこの順に連結する。含めるだけなら宣言だけが見え、`BDF_IMPLEMENTATION` を定義した 1 つの翻訳単位にライブラリ全体が入る。そのための規則は次のとおり。

- 内部の識別子はすべて `bdfi_`（マクロは `BDFI_`）で始める。単一ヘッダでは含める側の翻訳単位に見えるので、衝突しない名前が要る。
- ファイルをまたいで共有する内部関数は `BDF_INTERNAL` を付ける。ライブラリのビルドでは空（隠し可視性）、単一ヘッダでは `static`。共有する内部の配列は持たず、関数で引く（`bdfi_dc_term`）。
- `BDF_STATIC` を定義すると `BDF_API` が `static` になり、公開関数も翻訳単位の外に出ない。
- 実装は C11。配列の指示初期化子（`bdfi_op_table`）と `void *` の暗黙変換を使っているので、C++ の翻訳単位には実装を入れない（宣言だけ含め、実装は 1 つの .c に置く）。
- CMake は Python があるとき `bdf_single_header` ターゲットで生成し、単体テストを単一ヘッダに対してもライブラリと同じ警告（`-Werror`）で回す。生成物は CI の成果物（`bdf-single-header`）にもなる。

第三者ライブラリ自体は単一ヘッダに入れない。WOFF2 の復元のコードは入っていて、Brotli は `BDF_HAVE_BROTLI` を定義して libbrotlidec（か `third_party/brotli`）を一緒にコンパイル・リンクすれば使え、定義しなければ `BDF_ERR_UNSUPPORTED` を返す。ラスタや画像コーデックも同じ扱いにする。

## 7. 適合テストの形式（bdf conformance）

bdf の `conformance` パッケージが、入力と「参照実装がそれをどう扱うか」の組を書き出す。期待値は参照実装を実際に走らせて記録したもので、各事例は「何を示すために書いたか」の判定（受理か拒否か）も持ち、生成時に両者が食い違えば生成が失敗する。だから suite が参照実装から離れることはない。

```
<dir>/suite.json
<dir>/inputs/<id>.bin
```

`suite.json` は `{"format": "bdf-conformance/1", "bdf": 1, "opset": 1, "generator": "...", "cases": [...]}`。事例は `id`、`kind`、`input`、`note`、`expect` を持つ。

| kind | expect |
|---|---|
| `single`（単一ファイルの文書） | `open`: `ok`/`error`。`ok` のとき `locked`、`views`（`{id, kind, pages}` の列）、`parts`（個数）、`reads`（`{h, read, size}`: 読むべき Part とその結果）、`allParts`（全 Part が読める） |
| `object`（Object Part） | `decode`: `ok`/`error`。`ok` のとき `walk`（全命令が読めるか）、`instructions`（終わりかエラーまでに読めた命令数） |
| `paths`（パス集の Part） | `decode`、`paths`（個数） |

`message` は参照実装のエラー文言で、人が読むためのもの。読み手が再現する必要はない。

事例は、ヘッダ（マジック、版、フラグ、予約バイト、manifest の範囲、deflate、暗号化フラグの整合）、manifest の数値の範囲、manifest の JSON（拒否するものと受け付けるもの）、Part の範囲と inflate の上限、書き手が作る文書、Object（切り詰めの全接頭辞、未知の opcode、巨大な個数、不正な参照）、パス集、そして testdata の全文書（受理）である。

libbdf 側は `tests/conformance.c` が suite を読んで同じ検査をする。suite は bdf の CI の成果物（`bdf-conformance`）とリリースの添付（`bdf-conformance-<tag>.tar.gz`）として配られる。libbdf の CI でこれを回すのは、その添付を持つ bdf の版に `go/go.mod` を上げてからにする。

## 8. 配り方（計画）

ソースは CMake ベースのこのリポジトリ 1 つで、配る先ごとに経路を変える。

| 相手 | 経路 |
|---|---|
| C/C++ 一般 | 単一ヘッダ `bdf.h`（コア層） |
| Qt、C++ 一般 | `find_package(bdf)`、`FetchContent`、vcpkg と Conan のポート |
| GTK | pkg-config（`bdf.pc`）、Meson の wrap |
| .NET MAUI | NuGet（`runtimes/<rid>/native/` にバイナリ、P/Invoke の managed アセンブリ、SkiaSharp 上のアダプタ） |
| Cocoa、Swift | `swift-bdf` リポジトリの Swift Package（xcframework の binaryTarget） |

ABI は SONAME（`libbdf.so.0`）と `bdf_version()` を持ち、コンパイラが違っても壊れないことを CI の MSVC と clang の両方のビルドで確かめる。小さい依存（Brotli のデコーダ、plutosvg）は vendoring して既定ビルドを自己完結にし、大きい依存（FreeType、画像コーデック）は `find_package` で探してなければ `FetchContent` に落ちる、という二段構えにする。

## 9. 道筋

1. ~~コンテナと manifest（JSON）、Part の取得（single、split、コールバック）、deflate-raw、spec の上限。~~ 済み。
2. ~~機械可読の適合テスト。~~ bdf 側に `conformance` パッケージと `bdf conformance` を足した。libbdf の CI への組み込みは bdf の次のリリース後。
3. ~~WOFF2 → SFNT。~~ 済み。testdata の全フォントで参照実装とバイト単位で一致、差分 fuzz（`FuzzWOFF2`）あり。hmtx 変換は参照実装のエンコーダが書かないので、手で組んだ流れの単体テストで押さえた（`tests/woff2_internal.c`）。
4. ~~テキスト run、リンク、テキスト索引。~~ 済み（View のテキストと検索用テキスト、cue 索引も）。残りは読み上げ用の構造（MARK の見出し・リスト・表・図）と、正規化つきの検索。
5. ~~暗号化。~~ 済み。パスワードと ECDH の開錠、区間（§3.6）の合成。暗号の部品は自前（§11）。
6. ラスタライザの移植と、bdf の golden との比較。
7. アダプタ。最初は Skia（Chrome の Canvas 2D の実体なので TS 版との差が最も小さく、SkiaSharp 経由で MAUI もすぐ使える）。

## 10. 参考値

検討の過程で測ったもの。bdf の Go 実装を macOS arm64 の c-shared にすると 6.8 MB、A4 を 2 px/unit で描いて約 30 ms。TS の Canvas アダプタは 621 行、テキスト層 869 行、コア 2,550 行で、C 版の規模の見積もりの根拠にしている。testdata の 101 文書では命令の 99.9 % が 16 種で、GROUP、MASK、SHADOW、BLEND≠0、ALPHA≠1 はそれぞれ数文書にしか現れない。

## 11. 暗号化と区間

**部品は自前で持つ**。当初は Mbed TLS か platform の API（CommonCrypto、CNG、OpenSSL）を vtable で差し替える案だったが、やめた。読み手に要るのは開錠だけで、SHA-256、HMAC、PBKDF2、HKDF、AES-256、AES-KW のアンラップ、GCM の復号、P-256 の ECDH の 8 つに限られる。乱数も署名も要らない（ECDH の秘密鍵は呼び出し側が OS の乱数で作って渡す。`bdf_p256_public_key` はその公開鍵を返すだけ）。`src/crypto.c` は 900 行あまりで libc にしか依存せず、単一ヘッダにそのまま入る。外部ライブラリを 1 つ足すと、単一ヘッダ、vcpkg、NuGet、xcframework のすべてで「暗号ライブラリをどう持ち込むか」が利用者の問題になる。その代価に見合わないと判断した。

**検証**。部品ごとに Go の標準ライブラリと突き合わせている（`go/crypto_test.go`: ランダムな入力での一致と、`FuzzGCM`、`FuzzP256`）。C だけのテストは、Go が計算した既知解（RFC 4231、6070、5869、3394、FIPS 197 の入力を含む）を `tests/crypto_vectors.h` に持つ。文書の単位では、testdata の全文書を参照実装で暗号化して両方の読み手で開け、manifest と全 Part を比べる。改竄は 1 バイトずつ変えて、開くものと拒否するものが一致することを確かめる。

**サイドチャネルへの立場**。

- P-256 のスカラー倍は秘密鍵に依存しない。完全な加法公式（Renes–Costello–Batina、a = −3。Go の `nistec` と同じ）を使い、常に倍算と加算の両方を計算して、結果を定数時間の選択で取る。体の演算は 32 ビット limb の Montgomery 乗算で、分岐は最後の減算の選択だけ（これもマスク）。
- GCM のタグ、AES-KW の検査値の比較は定数時間。タグが合わなければ平文は 1 バイトも書かない。
- AES は S-box の表引き、GHASH は 4 ビットの表引きで、**キャッシュ タイミングには耐性がない**。Go の `crypto/aes` も AES-NI のない環境では同じ表引きに落ちる。読み手は自分の文書を自分の鍵で開けるだけなので、攻撃者が同じ機械で時間を測りながら復号を何度も走らせる場面は想定しにくい、という判断である。共有ホストで他人の文書を復号するサーバーに使うなら、この前提は成り立たない。
- 鍵と、鍵から作った表は、使い終わったら `bdfi_wipe`（volatile 経由の書き込み）で消す。パスワードの NFC 形もそうする。

**パスワードの NFC**。spec はパスワードを NFC にして UTF-8 にすると定める。`src/unicode_nfc.h` は `golang.org/x/text` の表から `go/genunicode` が作る（Unicode 17.0.0、分解 2,081 件、結合クラス 403 区間、合成 961 対、表は約 55 KB）。ハングルは算術。`FuzzNFC` で x/text と突き合わせて、2 つ食い違いが出た。

- x/text は結合文字が 30 個を超えて続くと U+034F（CGJ）を挟む（Stream-Safe Text Format）。これは NFC そのものではなく、ブラウザの `String.prototype.normalize('NFC')` は挟まない。libbdf は挟まない。つまり **結合文字を 31 個以上続けたパスワード** は、Go の書き手と TS の読み手の間で鍵が食い違う（bdf 側の課題）。libbdf は TS と同じ鍵になる。差分テストはこの入力を飛ばす。
- 不正な UTF-8 のうち、末尾で足りないマルチバイトの先頭バイトは、x/text が残りをそのまま写す。libbdf はこれに合わせた（パスワードが正しい UTF-8 なら関係しない）。

**上限**。`iter` は 1〜10,000,000、password スロットの合計も 10,000,000 まで、ecdh スロットは 16 個まで。参照実装と同じ値で、同じ順に検査する。遅い CPU 向けに `BDF_MAX_KDF_ITERATIONS` をビルド時に下げられる（fuzz のビルドは 2,048）。

**開錠後の文書**。`bdf_doc_unlock` が成功すると、封印された manifest が文書の manifest になり、`bdf_doc_view` などはそれを返す。外側の manifest は Part を取りに行くためだけに残る。封印された manifest の `parts[]` が指す `sealed` が外側に無ければ開錠の時点で拒否する（参照実装は Part を読むときに気づくが、結果は同じ）。開錠は文書を書き換えるので、スレッド間で共有する前に済ませる。

**区間（§3.6）**。区間の文書は形としては 1 つの文書で、全ページの枠を持ち、自分のページにだけレイヤがある。`bdf_doc_add_segment(doc, segment)` は、segment のページを doc の同じ位置に入れ、segment の Part を doc の検索対象に足す。先に来た区間が持つ Part（共有のフォントやマスター）は後の区間に入っていないので、Part は「自分、次に足された区間の順」で探す。所有権は doc に移り、`bdf_doc_free(doc)` がまとめて解放する。渡した文書をさらに別の文書へ足すこと、渡した文書に区間を足すことは `BDF_ERR_ARG` で断る（Part の通し番号がずれるのと、二重解放を防ぐため）。view の id とページ数が違う文書は「別の文書の区間」として拒否する。区間ごとに鍵が違ってよく、平文の文書に区間を足すこともできる。
