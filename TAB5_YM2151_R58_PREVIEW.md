# Tab5向け YM2151 更新版（vgmM5 R58参照） / YM2151 R58-based Preview for Tab5

> [!NOTE]
> **この文書はR58段階の歴史的プレビュー記録です。** 2026.10.25版ではR61 FIX1の高音域修正・ネイティブFIRとFIR高速化まで進み、実機で音質安定・複数ソフト起動の初期確認が行われました。現在の状態は[2026.10.25リリースノート](RELEASE_NOTES_2026-10-25.md)を参照してください。以下の「未実施」はR58当時の状況です。


**公開区分 / Status:** ソースコード・プレビュー（開発版） / Source preview (development version)  
**対象 / Target:** M5Stack Tab5 / ESP32-P4, X68K Tab  
**公開日 / Published:** 2026-10-25  
**ベース:** [X68K Tab Production](https://github.com/Layer812/X68KTab5/tree/main)  
**vgmM5参照元 / Upstream reference:** [Layer812/vgmM5](https://github.com/Layer812/vgmM5), privately supplied R58 `firmware/src/fm_engine.c`

> **重要 / Important:** このブランチのYM2151変更はホスト上で検証していますが、**ESP-IDFでのTab5用ネイティブビルドと実機の音声・動作は未検証**です。完成版ファームウェアや動作保証済みのM5Burner版ではありません。既存の公開・実機確認済みProductionは`main`に残しています。
>
> These YM2151 changes were tested on a host computer only. **No ESP-IDF native build, Tab5 playback test or firmware flashing has been performed for this preview.** The existing Production version remains on `main`.

## 変更内容 / Changes

このブランチは**X68K TabのYM2151専用バックエンドだけ**を更新しています。vgmM5全体を移植したものではありません。

| 参照 | YM2151専用の修正 / YM2151-only change |
| --- | --- |
| R33 | FMオペレータの不要な位相変調2倍処理を解除 / Remove accidental x2 phase modulation |
| R36 | DT2音程比率をR58の実装と一致させる / Match R58 DT2 frequency ratios |
| R37 | 古い2サンプル平滑化を解除 / Remove legacy two-sample output smoothing |
| R38 | KFレジスタの上位6ビットを使用 / Read upper six KF register bits |
| R39 | KC依存のDT1テーブルと加算計算をR58に合わせる / Match R58 KC-dependent DT1 lookup and offset |

**変更した実装ファイル:** `components/px68k/fmgen/vgmm5_ym2151.cpp`

ゲストCPUの12MHz動作・周辺10MHz・YM2151 WRITE BUSY・Timer/IRQ・CT1/ADPCMクロック切替・MIDI・CPU0/CPU1の役割・44.1kHz音声出力は変更していません。R41以降の62.5kHzネイティブ合成やFIRはこのブランチには移植していません。

Guest timing, WRITE BUSY, timers/IRQ, CT1/ADPCM, MIDI, CPU affinity and the existing 44.1kHz output path are unchanged. **R41 native-rate synthesis and FIR are not included.**

## 検証状況 / Test status

- R58元の`fm_engine.c`を独立コンパイルし、YM2151の位相増分を比較: **36,864 / 36,864一致**（GCCの結果）。
- 定義域内のSanitizer比較: **23,040 / 23,040一致**。
- PCMホスト回帰: 通常FM・LFO・CH7ノイズ **144ケースPASS**。
- DT1/DT2変更時のPCMホスト試験: **96ケースPASS**。
- **ESP-IDFネイティブビルド: 未実施 / not tested.**
- **Tab5実機再生: 未確認 / not tested.**

### 既知の制約 / Known limitation

R58の参照実装と本バックポートの両方に、極端な高音域・高倍率設定で浮動小数点から`uint32_t`へ範囲外変換する可能性が残っています。例：4MHz、44.1kHz、KC=`0x7E`、KF=`0xFC`、DT1=3、MUL=15。Sanitizerで元のR58コアにおいて再現済みです。変換時の正しいエミュレーション仕様を確認するまで、推測のクランプやラップ処理を追加していません。

An inherited high-pitch float-to-uint32 conversion overflow was reproduced in the original R58 core and remains unresolved in this preview. This is **not** a production-verified fix.

## ソースの取得 / Source checkout

```bash
git clone --recurse-submodules -b tab5-ym2151-r58-preview https://github.com/Layer812/X68KTab5.git
cd X68KTab5
```

ESP-IDF 5.5.x以降の既存X68K Tabビルド環境と、`README.md`のビルド手順を参照してください。現時点ではソースを公開しているだけで、**この更新版の完成BIN・M5Burner Share Codeは提供していません**。

Refer to the regular README and its build dependencies. **No preview BIN or M5Burner Share Code is provided.**

## 権利・謝辞 / Attribution

- X68K Tab: [Layer812/X68KTab5](https://github.com/Layer812/X68KTab5)
- YM2151実装の参照元: [Layer812/vgmM5](https://github.com/Layer812/vgmM5)
- PX68K / Musashi / M5Unified / M5GFX / MidMod: 既存の `THIRD_PARTY_NOTICES.md` および `components/px68k/fmgen/VGMM5_YM2151_NOTICE.md` を参照

この公開に伴う **vgmM5本体の変更はありません**。X68000 ROM、Human68kのディスクイメージ、商用ソフトウェアなども追加していません。

No `vgmM5` repository changes or copyrighted disk/ROM assets are included in this update.
