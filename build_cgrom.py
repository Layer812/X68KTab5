from PIL import Image, ImageDraw, ImageFont

# X68000 CGROMサイズ (768KB)
ROM_SIZE = 0xC0000
CGROM_FILE = "CGROM.DAT"

# WSL上のフリーフォントのパス (環境に合わせて変更してください)
FONT_JP_PATH = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
FONT_EN_PATH = "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"

def get_jis_char(ku, ten):
    """区点番号からJIS文字を取得する (EUC-JP経由)"""
    high = ku + 0xA0
    low = ten + 0xA0
    try:
        return bytes([high, low]).decode('euc_jp')
    except UnicodeDecodeError:
        return ' ' # 未定義領域は全角スペースで代替

def draw_char_to_bytes(char, font, width, height, offset_y=0):
    """文字を描画し、X68000形式のビット配列(bytearray)にパッキングする"""
    # 1bit(モノクロ)モードでキャンバスを作成
    img = Image.new("1", (width, height), 0)
    draw = ImageDraw.Draw(img)
    
    # 文字を描画 (anchor="lt" で左上基準に配置)
    draw.text((0, offset_y), char, font=font, fill=1, anchor="lt")
    
    data = bytearray()
    for y in range(height):
        # 1行につき (width / 8) バイト
        for x_byte in range(0, width, 8):
            bits = 0
            for i in range(8):
                if x_byte + i < width:
                    if img.getpixel((x_byte + i, y)):
                        bits |= (1 << (7 - i)) # MSBから順番にビットを立てる
            data.append(bits)
    return data

def main():
    print(f"CGROMジェネレータを開始します...")
    
    # フォントの読み込み
    try:
        font_16 = ImageFont.truetype(FONT_JP_PATH, 16)
        font_24 = ImageFont.truetype(FONT_JP_PATH, 24)
        font_8x16 = ImageFont.truetype(FONT_EN_PATH, 16)
    except IOError:
        print("エラー: フォントファイルが見つかりません。パスを確認してください。")
        return

    # 全領域を RTE (0x4E73) で初期化したバッファを作成
    rom_data = bytearray(ROM_SIZE)
    for i in range(0, ROM_SIZE, 2):
        rom_data[i] = 0x4E
        rom_data[i+1] = 0x73

    # --- 1. 半角ANK文字 (8x16) の生成 ---
    print("半角ANK文字 (8x16) を生成中...")
    ank_offset = 0x3A800
    for code in range(0x00, 0x100):
        # ASCII文字とカタカナ領域のみを処理 (制御文字などは代替文字)
        char = chr(code) if 0x20 <= code <= 0x7E else ' '
        # ※本来半角カナ領域(0xA1-0xDF)の処理も必要ですが簡略化しています
        
        char_data = draw_char_to_bytes(char, font_8x16, 8, 16)
        idx = ank_offset + (code * 16)
        rom_data[idx : idx + 16] = char_data

    # --- 2. 漢字領域 (16x16 / 24x24) の生成ロジック ---
    def process_kanji_block(start_ku, end_ku, offset_16, offset_24):
        for ku in range(start_ku, end_ku + 1):
            for ten in range(1, 95):
                char = get_jis_char(ku, ten)
                
                # ブロック内の相対インデックス (1区あたり94文字)
                char_idx = ((ku - start_ku) * 94) + (ten - 1)
                
                # 16x16 全角 (1文字32バイト)
                # ※ Notoフォントは上部に少し余白ができるため offset_y=-2 で微調整
                data_16 = draw_char_to_bytes(char, font_16, 16, 16, offset_y=-2)
                idx_16 = offset_16 + (char_idx * 32)
                rom_data[idx_16 : idx_16 + 32] = data_16
                
                # 24x24 全角 (1文字72バイト)
                data_24 = draw_char_to_bytes(char, font_24, 24, 24, offset_y=-3)
                idx_24 = offset_24 + (char_idx * 72)
                rom_data[idx_24 : idx_24 + 72] = data_24

    # 非漢字 (1〜8区)
    print("全角非漢字 (1〜8区) を生成中...")
    process_kanji_block(1, 8, 0x00000, 0x40000)

    # JIS第一水準漢字 (16〜47区)
    print("JIS第一水準漢字 (16〜47区) を生成中...")
    process_kanji_block(16, 47, 0x05E00, 0x4D380)

    # JIS第二水準漢字 (48〜84区)
    print("JIS第二水準漢字 (48〜84区) を生成中...")
    process_kanji_block(48, 84, 0x1D600, 0x82180)

    # --- 2.5 CG ROM オフセット 0x0008 に CLR.L D0 を埋め込む ---
    # TRAP #15 → CG ROM 0x0008 へのジャンプで D0=0 (成功) にしてから RTE する
    # CLR.L D0 = 42 80
    rom_data[0x0008] = 0x42
    rom_data[0x0009] = 0x80
    # 0x000A は既に RTE (4E 73) で埋まっている

    # --- 3. ファイルへの書き出し ---
    print(f"ファイル {CGROM_FILE} を書き出しています...")
    with open(CGROM_FILE, "wb") as f:
        f.write(rom_data)
        
    print("完了しました。")

if __name__ == "__main__":
    main()