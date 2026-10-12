# libbdf の設計メモ

bdf（Go の参照実装と TypeScript のビューア）とは別に、ネイティブのアプリケーションから BDF を扱うための C のライブラリを作る。このメモは、なぜこの形にしたかの記録で、仕様そのものは bdf の `docs/spec.md` にある。

## 1. 目的

- Skia、cairo、QPainter、CoreGraphics のような描画 API を持つアプリケーション（Qt、GTK、.NET MAUI、Cocoa）から、BDF v1 をそのまま開いて描けること。
- 利用者がアップロードした文書を描く用途を含むので、不正な入力に対して参照実装と同じ上限を守り、メモリ安全であること。
- 既存の wire format と opset を変えない。libbdf は読み手であり、書き手ではない。

## 2. 層

```
core      コンテナ（single/split）、manifest（自前の JSON）、Part、deflate-raw（自前）、
          Object のデコード、WOFF2 → SFNT、テキスト run（選択・検索・リンク）、暗号化（任意）
raster    純粋な C のラスタライザ。bdf の raster/imagebdf の移植。FreeType で字形を取る
adapters  core の命令列をフレームワークのキャンバスに流す薄い層
          skia/ (C++)  cairo/ (C)  qt/ (C++)  coregraphics/ (ObjC)
```

core は形式だけを知り、描画を知らない。adapters は描画だけを知り、形式を知らない。raster は adapters の 1 つでもあるが、フレームワークに依存しない唯一の描き手なので、どの環境でも同じ絵を出す基準になる。

core のうち今あるのは読みの部分で、libc 以外に依存しない。JSON の読み手と inflate を自前で持つのは、依存を増やさないためと、単一ヘッダに収めるためである（§6）。

## 3. 参照実装との関係

- bdf の Go 実装が参照実装で、libbdf は検査される側である。`go/` の cgo ハーネスが同じバイト列を両方に通し、受理と拒否、manifest の構造、Part の復号したバイト列、資源表、命令列、エラーの位置と文言まで一致することを要求する。
- 入力は固定した版の bdf モジュールの testdata（`go/go.mod` の require）。全変換器の出力が揃っている。
- 依存は libbdf → bdf の一方向。bdf は libbdf を知らない。
- デコーダは Go の `decode.go`、`buf.go`、`path.go`、`paint.go`、`font.go`、`reader.go`、`manifest.go` を検査の順序まで含めて写した。Go の reader は最初の失敗を覚えて以後の読み取りを 0 にする。C の `bdfi_reader` も同じ振る舞いにしてあり、だから切り詰めた入力でも同じ位置・同じ文言で失敗する。
- manifest は encoding/json が参照実装の構造体を埋めるのと同じ規則で読む。知らないメンバーは無視、null は無いのと同じ、型の違うメンバーはエラー、重複したメンバーは最後が効く、整数のフィールドは整数リテラルだけ、float32 の範囲を超える数はエラー。合わせないと決めたのは、encoding/json がメンバー名を大文字小文字を無視して照合することと、base64 の中の改行を無視すること。どちらも仕様にない encoding/json の癖で、libbdf はそこだけ厳しい。差分 fuzz（`FuzzDoc`）は「libbdf が受理するものを参照実装が拒否しない」ことと「両方が開いた文書の Part が同じに読める」ことを要求し、逆向きは要求しない。
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
- **差分テスト**（`go/`）: testdata の全文書、全 Part、全 Object を Go と C の両方で読んで比べる。Linux では `go test -asan` で C 側も計装する。cgo が C ソースを直接コンパイルするので、ハーネスとライブラリは常に同じコミットになる。
- **適合テスト**: bdf の `bdf conformance` が書く suite を `tests/conformance.c` が Go なしで流す（§7）。
- **fuzz は明示実行**: CI では回さない。探索は時間が青天井で CI に向かないという判断で、bdf の運用（パーサを変えたときに数分回す）と同じにした。探索は 2 系統ある。カバレッジ誘導とリーク検出は C の libFuzzer（`fuzz/`、object・inflate・manifest・doc の 4 本）、Go の参照実装との差分は `go test -fuzz`（`FuzzObject`、`FuzzInflate`、`FuzzDoc`）。見つかった入力は手で `fuzz/regress/<target>/`（ctest の `replay-<target>` が流す）か `go/testdata/fuzz/`（`go test` が流す）に入れて再生テストにする。`fuzz/corpus/` は fuzzer が育てる作業用のコーパスで、コミットしない。CI の `fuzz` ジョブは `workflow_dispatch` で分数を指定したときだけ動く。

cgo を選んで purego にしなかったのは、`go build -asan` が cgo にしか効かないことと、同じツリーの C ソースを静的に結合したかったからである。利用者向けに cgo なしの Go 束縛を出すなら、そのときは purego で別に作る。

## 6. 単一ヘッダとの両立

配布形態の 1 つとして、コア層を 1 本の `bdf.h` にできるようにしてある。開発は分割ソースのままで、`tools/amalgamate.py` が公開ヘッダ、内部ヘッダ、ソースをこの順に連結する。含めるだけなら宣言だけが見え、`BDF_IMPLEMENTATION` を定義した 1 つの翻訳単位にライブラリ全体が入る。そのための規則は次のとおり。

- 内部の識別子はすべて `bdfi_`（マクロは `BDFI_`）で始める。単一ヘッダでは含める側の翻訳単位に見えるので、衝突しない名前が要る。
- ファイルをまたいで共有する内部関数は `BDF_INTERNAL` を付ける。ライブラリのビルドでは空（隠し可視性）、単一ヘッダでは `static`。共有する内部の配列は持たず、関数で引く（`bdfi_dc_term`）。
- `BDF_STATIC` を定義すると `BDF_API` が `static` になり、公開関数も翻訳単位の外に出ない。
- 実装は C11。配列の指示初期化子（`bdfi_op_table`）と `void *` の暗黙変換を使っているので、C++ の翻訳単位には実装を入れない（宣言だけ含め、実装は 1 つの .c に置く）。
- CMake は Python があるとき `bdf_single_header` ターゲットで生成し、単体テストを単一ヘッダに対してもライブラリと同じ警告（`-Werror`）で回す。生成物は CI の成果物（`bdf-single-header`）にもなる。

ラスタや WOFF2 のような第三者ライブラリに依存する層は単一ヘッダに入れない。

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

ABI は SONAME（`libbdf.so.0`）と `bdf_version()` を持ち、コンパイラが違っても壊れないことを CI の MSVC と clang の両方のビルドで確かめる。小さい依存（Brotli のデコーダ、plutosvg）は vendoring して既定ビルドを自己完結にし、大きい依存（FreeType、画像コーデック、Mbed TLS）は `find_package` で探してなければ `FetchContent` に落ちる、という二段構えにする。

## 9. 道筋

1. ~~コンテナと manifest（JSON）、Part の取得（single、split、コールバック）、deflate-raw、spec の上限。~~ 済み。
2. ~~機械可読の適合テスト。~~ bdf 側に `conformance` パッケージと `bdf conformance` を足した。libbdf の CI への組み込みは bdf の次のリリース後。
3. WOFF2 → SFNT（Go の `font/woff2` と fontTools のテストベクタで固める）。
4. テキスト run、リンク、テキスト索引の検索。
5. 暗号化（Mbed TLS か platform の vtable）。
6. ラスタライザの移植と、bdf の golden との比較。
7. アダプタ。最初は Skia（Chrome の Canvas 2D の実体なので TS 版との差が最も小さく、SkiaSharp 経由で MAUI もすぐ使える）。

## 10. 参考値

検討の過程で測ったもの。bdf の Go 実装を macOS arm64 の c-shared にすると 6.8 MB、A4 を 2 px/unit で描いて約 30 ms。TS の Canvas アダプタは 621 行、テキスト層 869 行、コア 2,550 行で、C 版の規模の見積もりの根拠にしている。testdata の 101 文書では命令の 99.9 % が 16 種で、GROUP、MASK、SHADOW、BLEND≠0、ALPHA≠1 はそれぞれ数文書にしか現れない。
