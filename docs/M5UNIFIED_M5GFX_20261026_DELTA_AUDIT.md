# M5Unified 0.2.19 → 0.2.28 / M5GFX 0.2.26 → 0.2.35
## Tab5差分監査：MDX再生のモタツキを安定版判定から分離

更新日：2026-10-26 JST
対象：`Layer812/X68KTab5` の開発用 Draft PR #10。現行 Production/main と公開 BIN は変更しない。

### 実機で確認できたこと（利用者報告）
- sfxvi と mmdsp は起動。
- ディスクの読み込みは体感で大きく高速化。
- **MDX再生のモタツキは残存**。最初のライブラリ更新BINだけでなく、その後の音声キュー変更BINでも同様。
- 同一音源の旧新版比較、実PCM速度・ハードウェアサンプルクロック測定、CPU0音声デッドラインのログは未取得。**原因特定・MDX安定版判定は未完了。**

### 固定したライブラリSHA
- M5Unified 0.2.19 `4fb444784c85791e0b0207701392b42be234b2e7` → 0.2.28 `368ad60807161625aa65baad37a4a3af2a7eccea`
- M5GFX 0.2.26 `729297d6e3d657ddc1ec5189bac2f2ea68828085` → 0.2.35 `65c816a38386062d2c3dc47d41237a9083acbc75`
- ESP-IDF 5.5.4 / ESP32-P4。安定版YM2151のKEY ON・位相リセット、FIR、guest/IRQ/MDX実行タイミングは未変更。

### 音声差分：M5Unified

| 論点 | 旧版と新版の実装差分 | 判定・次に見るところ |
| --- | --- | --- |
| P4 I2S実レート | 旧Speakerは20 MHz基準のraw divider・実数換算を使う。新版は`M5UNIFIED_I2S_DRIVER_MANAGED_CLK`で`_cfg.sample_rate`をI2S driverへ渡し、補間側も公称レートを使う。MCLK設定も変更。 | **維持**（公式修正）。実サンプルレート、I2S送出継続性、供給側44100 Hzとの一致は実機未測定。テンポ変化を断定しない。 |
| ES8388/AMP電源 | 旧終了コールバックは主にAMP disable＋codec reg8のみ。新版はAMP OFF → DAC mute → 1 ms → DAC/CHIP power-down。開始時レジスタテーブルは同一。 | **維持**。不明なクリックノイズと関連する可能性があり、X68KTab5の50 ms cold restart＋20 ms settleとの共存確認は必要。 |
| `Speaker.isPlaying(channel)` | 旧版は2スロットの`repeat`数を返す。新版は2スロットの状態が`empty`以外なら数える。**新版の現在再生中のスロットは`wav_phase_playing`のままなのでカウント対象**。別`current`構造体へのPCMメタデータコピーだけではこのスロットは解放されない。 | **重要な訂正**：新APIが「待機中だけ」を数えるという推定は誤り。先の`speaker_queue + isPlaying()`修正は二重計数を招くため**撤回**。旧来の二重バッファ扱いを維持。 |
| `playRaw`契約 | 新版はslot claim、atomic state／lock、`begin_raw`の競合防止、真の失敗時falseなどを追加。I2Sへの音声書き込み／Mixerは大枠を維持するが実行コストとキュー境界は変わった。 | **要監視**。CPU0での`playRaw`待機時間／再生欠落／ring残量とMDXモタツキが相関するか未測定。副作用の推測修正は行わない。 |
| 音声アプリ側 | `src/tab5_audio.cpp`の44.1 kHz、512f chunk、1536/1024f watermark、3 play buffers、prio4、CPU0 pinned等はProductionから変更しない。 | **保護**。同時にゲスト/ADPCM/CPU1タイミングを変更しない。 |

根拠：[旧Speaker_Class.cpp](https://github.com/m5stack/M5Unified/blob/4fb444784c85791e0b0207701392b42be234b2e7/src/utility/Speaker_Class.cpp) ／ [新Speaker_Class.inl](https://github.com/m5stack/M5Unified/blob/0.2.28/src/utility/Speaker_Class.inl) ／ [新Speaker_Class.hpp](https://github.com/m5stack/M5Unified/blob/0.2.28/src/utility/Speaker_Class.hpp) ／ [P4 I2S選択マクロ](https://github.com/m5stack/M5Unified/blob/0.2.28/src/utility/m5unified_i2s.h) ／ [新ES8388初期化・停止](https://github.com/m5stack/M5Unified/blob/0.2.28/src/M5Unified.inl)

### 表示・タッチ差分：M5GFX

| 論点 | 新版実装・差分 | 判定・次に見るところ |
| --- | --- | --- |
| ST7121/ST7123/ILI9881C | 公式の3種パネル識別を維持。ST touch FW probe 最大60回＋GT911 ACK判定、ソフトI2C probe後のHWバス復帰。 | **取り込み**。独自の古い20回probe対策を重複適用しない。起動時の検出が済んだ後は通常の描画時間に直接かかわらない。 |
| DSI timingとlane | ST7121 965Mbps/70MHz/H40-2-40/V24-20-200＋ACK off/clock HS、ST7123 1040Mbps/78MHz/V4-2-320、ILI9881C 730Mbps/60MHz/H140-40-40/V20-4-20＋ACK off。 | **維持**。実機で検証した各variantのマージン値。3種類すべてで最新候補の動作が証明されたわけではない。 |
| DPI DoubleFB | 最新`Panel_DSI.inl`自体が`num_fbs=2`。ただしX68KTab5で使う2枚目ポインタと生DPI handleの公開は標準にはない。 | **標準FB確保を利用、ポインタ公開のみ独自overlayで保持**。2枚目を二重確保していないことをCIで検査。 |
| DSIバス初期化 | `Bus_DSI`実装が.cpp→.inlへ移動。DSI configのゼロ初期化を実施。PHY clock sourceは旧版の明示指定から新デフォルトへ。 | **検証要**。ESP-IDF設定下で同等のPHY sourceに解決されるかは実機クロック検証なし。 |
| LCD panel init | `getResetDelayBeforeDpi()`によるpanel-specific起動待機が追加。 | **取り込み**。基本は初期化時の変更で、継続MDX演奏の一定周期のモタツキとは直結しない。 |
| UI/描画負荷 | M5GFXの構成・統合TU、描画・I2C・SPI周辺に大量の修正。X68KTab5自体のCPU0 presenter、dirty-row/PPA、FIR、CPU1実行部は変更なし。 | **未確定**。コンパイル成功だけではCPU0実負荷の増減を保証できない。 |

根拠：[旧M5GFX](https://github.com/m5stack/M5GFX/tree/0.2.26) ／ [新M5GFX](https://github.com/m5stack/M5GFX/tree/0.2.35) ／ [Tab5検出](https://github.com/m5stack/M5GFX/blob/0.2.35/src/M5GFX.cpp) ／ [DPI](https://github.com/m5stack/M5GFX/blob/0.2.35/src/lgfx/v1/platforms/esp32p4/Panel_DSI.inl) ／ [DSI bus](https://github.com/m5stack/M5GFX/blob/0.2.35/src/lgfx/v1/platforms/esp32p4/Bus_DSI.inl)

### 安定版の判断・現在の方針
1. **MDXモタツキが再現しているためPR #10はDraft／Productionには統合しない。** 今回のソースレビューだけで直ったとは扱わない。
2. 音声`isPlaying`の誤った推定修正は戻し、`src/tab5_audio.cpp`を基準Productionとバイト同一に復元済み。現在の開発ブランチはライブラリ、LCD overlay、CI、監査文書だけを変える。
3. M5UnifiedのP4 I2SクロックとES8388正常終了処理は根拠があるので不用意に戻さず、音声出力の実テンポ／CPU0 deadlineなどの客観証拠を優先する。
4. ディスク読み込み高速化は有望だが、ライブラリ差分だけから原因や速度改善率を推測しない。
5. 次の実機依頼は、通常版としての具体的な修正を絞りネイティブビルドまで通した後にまとめる。計測専用ビルド、小刻みな再検証、フルフラッシュ、バックアップ作成は行わない。

CI／移行対象: https://github.com/Layer812/X68KTab5/pull/10
