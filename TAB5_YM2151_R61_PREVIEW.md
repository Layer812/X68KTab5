# X68K Tab — YM2151 R61 FIX1 Tab5プレビュー / R61 FIX1 YM2151 Preview

> [!NOTE]
> **この文書はR61移植初期の歴史的プレビュー記録です。** 後続のFIR高速化版が2026-10-25にESP32-P4でビルドされ、実機へのapp-only書込み・HumanXDF保持・音質安定・複数ソフト起動が確認されました。最新情報は[2026.10.25リリースノート](RELEASE_NOTES_2026-10-25.md)を参照してください。以下の「実機未検証」は移植初期時点の記述です。


**状態 / Status:** **ソースコード開発プレビュー（実機未検証） / Source preview — NOT device verified**  
**対象 / Target:** M5Stack Tab5 / ESP32-P4  
**参照 / Reference:** User-provided `vgmM5_R61_FIX1_PRODUCTION_LOCK_20261025.zip` (2026-10-25). Its source package and firmware evidence checksums were independently verified.  
**安定版 / Current stable:** [main](https://github.com/Layer812/X68KTab5/tree/main)

> このブランチはM5Burner用完成ファームではありません。**ESP-IDF 5.5.4／ESP32-P4ネイティブビルドはPASSしましたが、Tab5実機での音質、CPU0負荷、音切れ、長時間再生は未確認**です。既存のProduction公開版は変更していません。
>
> This is **NOT** a device-verified firmware release. ESP-IDF 5.5.4 / ESP32-P4 native build has PASSED, but physical Tab5 sound/performance testing and flashing have NOT been performed. The existing Production code and M5Burner share code remain unchanged.

## 何を取り込んだか / Backport scope

| 系列 | 内容 |
| --- | --- |
| R33 | YM2151位相変調の不要な×2を解除 |
| R36 | R61元コードと同じDT2比率 |
| R37 | YM2151旧2サンプル平滑化を解除 |
| R38 | KFレジスタ上位6ビットを使用 |
| R39 | KC依存DT1テーブルと計算 |
| **R59/R61** | **高音域のuint32範囲外位相増分をmod 2^32へ安全に変換** |
| **R41/R47/R61** | **4MHz YM2151のみ内部62,500Hz合成、25タップFIR、Q32線形補間で44,100Hz出力** |
| R41/R61 | 内部合成速度を変更しても旧44.1kHzのEG壁時計速度を維持 |

変更対象は `components/px68k/fmgen/vgmm5_ym2151.cpp` と参照元から取り込んだ以下の2ヘッダのみです。

- `components/px68k/fmgen/r41_ym2151_native.h`（R61原本と同一）
- `components/px68k/fmgen/r47_ym2151_fir25.h`（R61原本と同一）

CMakeのソースリスト、`fmg_wrap.cpp`、guest CPU、YM2151 WRITE BUSY / Timer / IRQ、CT1/ADPCMクロック、YM3802 MIDI、USB、LCD、パーティション設定、vgmM5リポジトリは変更していません。

**R61のM5.Speaker送信後待機設定は移植していません。** X68K Tabは独立した音声キュー/I2S経路を使い、R61実機のノイズ解消を同じ条件で再現できるとは限りません。FIRはFM出力にだけ適用され、ADPCMミックスにはかかりません。

## 有効条件 / Native path guard

`X68KTAB_YM2151_R61_NATIVE_FIR_PREVIEW` はこのプレビューブランチでは標準で **1**。YM2151クロックが **4,000,000Hz**、最終PCMが **44,100Hz** のときだけ62,500Hz＋FIRへ切り替えます。11,025/22,050Hzや他クロックでは従来の合成経路を使います。

調査目的で `-DX68KTAB_YM2151_R61_NATIVE_FIR_PREVIEW=0` とすれば、R59位相安全化を維持したまま従来の44.1kHz合成経路と比較できます。レート切替・Resetでは内部FIR/補間状態を明示的に初期化します。

## 確認した項目 / Host evidence

- R61の**実際の `fm_engine.c`**を独立コンパイルして位相増分を比較: **82,944 / 82,944一致**（4MHz / 3.579545MHz、出力11.025/22.05/44.1kHz、KC/KF/DT1/DT2/MULの組合せ）。
- **PCM分割連続性144 / 144 PASS**: 8アルゴリズム × 通常FM・PM/AM LFO・CH7ノイズ × KF3値 × DT1/DT2条件2値。一括4,000フレーム vs 1フレームずつ vs 127フレーム単位が完全一致。
- **ASan / UBSan / float-cast-overflow: PASS**（PCM連続性、Reset、レート切替、R58で検出した高KC値）。
- **GCC 14.2 / Clang++ ホスト構文チェック: PASS**。Clangは既存のGCC独自`optimize`属性を無視します。
- `R58`と`R61`の**旧44.1kHz経路のPCM比較: 同一**（今回の既知テスト信号、非オーバーフロー）。
- 提供されたR61保存ZIP・ネイティブBIN・ELF: SHA-256一致。**vgmM5の実機ビルド・書き込み成功**と**ユーザーによるプチプチノイズ解消報告**は、X68K Tab側の実機検証に代わるものではありません。

### 未実施 / Not done

- Tab5実機フラッシュ、MDX/PDX音声、画面/USB/MIDI同時動作、音声キューのunderrun調査、実機負荷とIRAM余裕の評価。
- 実機ではCPU0に**約1.42倍のYM2151 FMティックと25タップFIR**の負荷が増えるため、ゲストCPU1の時間基準と音声連続性を維持できることを一括して確認する必要があります。
- ソフトウェアに関する新たなROM、ディスクイメージ、ゲーム音楽データは追加していません。

## ESP-IDFネイティブビルド結果 / Native-build evidence

- **GitHub Actions PASS:** [ESP-IDF 5.5.4 / ESP32-P4 (2026-10-25)](https://github.com/Layer812/X68KTab5/actions/runs/40109972436)
- **Commit:** `b630a8265becf2034f123eaaf60e0ce4de6bc603`
- **App image:** `px68k_tab5.bin` — 2,422,416 bytes; SHA-256: `2b5ff584cd5c93fe5c4c3fb563b60628224e225d1247deb19488b1aec0564208`
- **ESP32-P4 ELF:** `px68k_tab5.elf` — 21,917,768 bytes; verified RISC-V ELF32.
- **Partition validation:** factory app at `0x10000` (8MiB), HumanXDF at `0x810000` (0x140000 bytes). App occupies 29% of its partition; 71% available.
- **Artifacts:** [Verified BIN/ELF/bootloader/partition-table ZIP](https://github.com/Layer812/X68KTab5/actions/runs/40109972436/artifacts/12566127902) (GitHub Actions retention seven days).
- Source checksum/host PCM continuity checks passed; the CI compile produced compiler warnings but **no errors**.
- **No device flash or actual Tab5 sound test was done**. Device acceptance and performance are not established.

## ソース取得 / Checkout

```sh
git clone --recurse-submodules -b tab5-ym2151-r61-preview https://github.com/Layer812/X68KTab5.git
cd X68KTab5
```

詳細の従来手順・著作権・依存関係は`README.md`、`README_en.md`、`THIRD_PARTY_NOTICES.md`および`VGMM5_YM2151_NOTICE.md`を参照してください。

This preview only changes X68KTab5. **`Layer812/vgmM5` has not been modified.**
