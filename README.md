# tinytts

超軽量の日本語音声合成 (Klatt型フォルマント合成)。C言語1ファイル、依存なし。

## ビルド

CMake (Windows / Linux / macOS 共通):

    cmake -B build
    cmake --build build --config Release

実行ファイルは `build/tinytts` (Windows の Visual Studio では `build/Release/tinytts.exe`)。
MinGW / MSYS2 の場合は `cmake -B build -G "MinGW Makefiles"` のように ジェネレータを指定します。

CMake なしでも単体でビルドできます:

    cc -O2 -o tinytts src/tts.c -lm

## 使い方

    tinytts [-s 速度] [-p 基本周波数Hz] [-o out.wav|-] "テキスト"

`-o -` で 16kHz/16bit/モノラルの生PCMを標準出力へ出します。テキストを省略すると標準入力から読みます (UTF-8)。

再生:

    # Windows (PowerShell)
    .\build\tinytts.exe -o out.wav "こんにちは。"; (New-Object Media.SoundPlayer "$PWD\out.wav").PlaySync()

    # Linux
    ./tinytts -o - "こんにちは。" | aplay -q -f S16_LE -r 16000 -c 1

## 入力

- ひらがな・カタカナ・ローマ字
- `'` : アクセント核の直後に置く (例: `ha'shi`)。無指定は平板型
- `、` `,` 短い間 / `。` `.` 長い間 / `？` `?` 上昇調
- 助詞は発音どおりに書く (は→わ, へ→え, を→お)
- 漢字は非対応

## 構成

テキスト → 拍 → セグメント(フォルマント目標) → 2.5msフレーム → 音声サンプル

## 今後

固定小数点化 (マイコン向け) / WASM化 / 簡易アクセント辞書
