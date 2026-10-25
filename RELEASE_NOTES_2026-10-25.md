# X68K Tab — 2026.10.25 Tab5 release / リリースノート

**公開日 / Date:** 2026-10-25  
**プラットフォーム / Target:** M5Stack Tab5 / ESP32-P4  
**区分 / Status:** Tab5実機での初期確認済みソース更新（非公式X68000エミュレータ） / Tab5 smoke-tested source update  
**初回実機確認版ソース / Original tested firmware source:** `5704225d65cfd94d5f0d951253a359db6cda6aed`  
**プレビュー開発:** [R61移植PR #2](https://github.com/Layer812/X68KTab5/pull/2), [FIR最適化PR #3](https://github.com/Layer812/X68KTab5/pull/3)

> 本リリース番号はGitHubソースの公開識別子です。初回実機確認に使用したBINは、上記のソースコミットをGitHub Actionsでビルドしたものです。リリース文書追加によってBINは変更していません。ESP-IDFが起動ログへ出力するアプリのビルド識別子は、そのBIN生成時のGitコミット情報のままです。
>
> Version `2026.10.25` identifies this published source update. The proven BIN was built from the exact source commit above. Documentation/version metadata commits have **not** been recompiled into or flashed onto the tested BIN.

## 最新更新 / Latest update

- YM2151のKEY ON（M2/C1）対応と、新しい発音時の位相リセットを修正。実機で再生を確認し、現時点でプチノイズは確認されていません。
- [ネイティブビルド・回帰テストPASS](https://github.com/Layer812/X68KTab5/actions/runs/40200136475)。最新ソースコミット：`468ee6a38ef119addab2dc6a2f5132eb63a672a1`。

## 以前の更新 / Previous changes

- **YM2151音色・音程:** vgmM5 R58〜R61を参照し、誤った×2位相変調の修正、KF上位6ビット、DT1（KC依存）およびDT2比率の修正、旧2タップ平滑化を解除。
- **高音域安定性:** 高KC/MUL時のfloat→uint32範囲外変換をR61参照のmod 2^32処理で安全化。
- **ネイティブYM2151:** 4MHz／44.1kHz PCM出力の場合、FMの内部合成を62.5kHzへ変更。25タップFIR・Q32補間をFMにのみ適用し、最終PCM出力は44.1kHzのまま。
- **速度対策:** FIR25係数、計算精度、タップ順序を維持し、折り返し処理を低コスト化。追加Internal SRAMはYM2151状態あたり約200バイト。
- **維持した仕様:** X68000 guest CPU 12MHz、周辺10MHz、YM2151 WRITE BUSY、Timer/IRQ、CT1/ADPCMクロック、MIDI（YM3802/MidMod/SAM2695）、画面/USB/SD/HostFS、HumanXDF構成。
- **変更対象:** YM2151専用バックエンド＋R41/R47ヘッダ＋FIR検証コード。**vgmM5リポジトリは未変更。**

## ビルド・検証の根拠 / Evidence

- ESP-IDF **v5.5.4、ESP32-P4ネイティブビルドPASS**: [GitHub Actions #40171386894](https://github.com/Layer812/X68KTab5/actions/runs/40171386894)（コミット `5704225...`）。
- FIR出力一致テスト350,000サンプルPASS、144組のYM2151 PCMホスト回帰PASS、ASan/UBSanによる検証PASS。
- 実機でのapp-only書き込み・読戻し: **PASS**（SHA-256完全一致）、事前16MiBバックアップ: **PASS**、HumanXDF事前事後SHA-256: **一致**。
- **初回実機確認BIN** `px68k_tab5.bin`: **2,422,528 bytes**、SHA-256 `e86d8d1abf52e7d83413b79f7f21e672be2f690cd74c495f229f325de09c12a0`。
- パーティションテーブルSHA-256 `fbf9c389a19abcf7da6568711994305feb05643050bb7f67b835ddfb8044f9f5`（変更なし）。appは `0x10000`、HumanXDFは `0x810000`。
- 実機のユーザー確認: **音質は安定、複数ソフトが起動**。旧R61での若干の速度低下について、FIR高速化版は**「よさそう」**との初期評価。
- 制約: **定量的速度比較、CPU0負荷、長時間再生、全種類のLCD／USB／MIDI／MDX・PDX同時回帰は未完了**。実機結果ZIPのシリアルログは起動初期（約3.3KB）で、詳細テレメトリではありません。全環境での完全互換や改善率は保証しません。

## 配布 / Distribution

- ソース: [Layer812/X68KTab5](https://github.com/Layer812/X68KTab5)
- **最新M5Burner Share Code:** `wOuCl6ZAtFxUpdy6`（開発者提供。配布BINとのハッシュ一致は独立検証していません）。
- **従来の2026.10.25版コード:** `2cODB4oLECo97g0u`。
- 以前のProduction M5Burnerコード: `tLkdgjcPoEoGbGEt`（2026.10.09までのPDX/ADPCM・LCD・MIDI更新を含む既存版）。
- Github Actionsアーティファクトは保存期限があるため、恒久的なBIN配布先とは扱いません。
- X68000の独自ROM、Human68k、商用ソフトウェア／音楽／ディスクイメージは再配布しません。権利関係は `THIRD_PARTY_NOTICES.md` 等を参照してください。

## ユーザー作業用ディレクトリ / Local development directories

`F:\build\archive`: 原本と16MiBバックアップを保管。  
`F:\build\tmp`: 展開・実行・報告ZIP作成。  
`F:\build\release`: 確定したリリース物のみ保存。

実機の差分アップデートに使ったapp-only安全書込みは、元のパーティションとHumanXDFを維持する方式です。GitHubのファームウェアは一般利用者に対して未知のフラッシュ構成への無条件書込みを推奨するものではありません。
