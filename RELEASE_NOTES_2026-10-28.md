# X68K Tab — 2026.10.28 R6 / source update

**Target:** M5Stack Tab5 / ESP32-P4 / ESP-IDF 5.5.4  
**Source build verified:** `802a10bf915d221ae91c950029d5e812ad4d085a`  
**Native CI:** [GitHub Actions #40661572999](https://github.com/Layer812/X68KTab5/actions/runs/40661572999) — all steps PASS  
**Physical confirmation:** Owner reported, on 2026-10-28, that MDX playback no longer exhibited the previously audible stutter after installing the R6 app-only image.  
**M5Burner R6 Share Code:** `8i9Cp1C6ufGpGC5J` (provided by the project owner on 2026-10-28; hosted-image SHA-256 not independently verified against tested R6 app BIN). Previous share: `wOuCl6ZAtFxUpdy6`.

## 日本語

### 主な更新

- **M5Unified 0.2.28**（固定SHA `368ad60807161625aa65baad37a4a3af2a7eccea`）と **M5GFX 0.2.35**（固定SHA `65c816a38386062d2c3dc47d41237a9083acbc75`）へ更新。
- Tab5の ST7121 / ST7123 / ILI9881C 検出・パネル設定、DSI DoubleFB、タッチ、既存の描画最適化を維持。
- Tab5 ES8388のDAC MCLK/LRCK設定を**256fs**へ修正。
- M5Unified Speakerのバッファ解放通知をCPU0音声フィーダへ接続し、待機時間を短縮。
- **R6:** PCMリングに再生可能なチャンクが既にあり、Speakerスロットが空いている場合、追加のFM/ADPCM取り込みよりもSpeakerへの送出を優先。R5では残っていたMDXのモタツキが、R6の実機再生では報告されませんでした。
- 元のguest 68000/周辺クロック、YM2151 KEY ON・位相リセット、R61 FIR25高速化、MIDI、HumanXDF領域、ストレージ構成を変更しません。

### 検証と範囲

- **Native CI PASS:** ESP-IDF 5.5.4 / ESP32-P4ビルド、既存YM2151回帰、FIR一致検査、DSI/ライブラリ固定版検査。
- **実機:** MDX音声のモタツキ解消を所有者が確認。複数の他ソフト・各種ディスプレイ個体・長時間連続再生までを再度網羅した検証ではありません。
- **M5Burner:** R6 Share Code `8i9Cp1C6ufGpGC5J` を開発者から受領。コードの掲載は配布BINの独立ハッシュ照合を意味しません。
- 今回のリポジトリ文書更新は、上記**実機確認済みR6ソースのファームウェア処理を変更しません**。文書・VERSION更新後のコミットは、確認済みの実機BINと完全に同一のビルド成果物であるとは主張しません。
- R5のDMA idle-padding省略経路は比較の再現性のため残っていますが、**R5単独で残留モタツキを改善した事実はありません**。
- 連続音声監査・プロファイル機構はRelease設定で無効。画面起動時セルフチェックと64KiBのCPUプリデコードSRAMは正常動作のため維持。
- アプリ更新は **factory app領域 `0x10000` のみ**で、bootloader、partition table、HumanXDF(`0x810000`)を変更しません。GitHub Actionsは書き込みを行いません。

## English

The R6 source keeps the pinned M5Unified 0.2.28 / M5GFX 0.2.35 Tab5 migration, panel-compatibility overlays, ES8388 256fs correction, and Speaker buffer-release notification. **R6 sends a ready PCM chunk to an available Speaker slot before doing another potentially expensive upstream pull.** After R5 did not resolve the remaining playback stutter, the owner reported stutter-free MDX playback with the R6 app-only image on a physical Tab5. No measured performance percentage or universal compatibility is claimed.

All GitHub Actions native and regression checks passed on the precise R6 source commit linked above. Documentation-only release commits are not represented as a separately re-flashed build. The project owner supplied R6 M5Burner Share Code `8i9Cp1C6ufGpGC5J` after the source release. The hosted image has not been independently SHA-256 matched to the physically tested R6 application BIN; the previous code `wOuCl6ZAtFxUpdy6` remains a separate older build. No copyrighted ROM, Human68k image, or commercial disk/game data is distributed.
