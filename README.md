# gridtool

**A lightweight image grid composer for Windows.**

フォルダ内の画像を64×64ピクセルに統一し、グリッド上に自由に配置して、1枚のBMP画像として出力するツールです。
画像をドラッグ＆ドロップするだけで、簡単に画像グリッドを作成できます。

## Features

- **Batch Image Loading** — フォルダ内の画像をまとめて読み込み
- **Automatic Resizing** — 画像を64×64ピクセルに縮小・拡大
- **Drag & Drop** — 画像をドラッグ＆ドロップしてグリッドに配置
- **Flexible Grid** — 行数・列数をそれぞれ1〜64の範囲で設定
- **Easy Editing** — 配置済み画像の移動、入れ替え、削除に対応
- **BMP Export** — グリッド全体を24-bit BMPとして出力

## Supported Image Formats

- PNG
- JPEG / JPG
- BMP
- TGA
- GIF

## Usage

### 1. Open a Folder

「フォルダを開く」をクリックし、画像が保存されているフォルダを選択します。
フォルダ内の対応画像が読み込まれ、左側のパネルにサムネイルとして一覧表示されます。

### 2. Arrange Images

左側のサムネイルを右側のグリッドへドラッグ＆ドロップして配置します。

- 配置済みのマスにドロップすると画像を置き換えます。
- グリッド内の画像を別のマスへドラッグすると移動できます。
- マスを右クリックすると、そのマスの画像を削除できます。
- 「全クリア」で配置をすべてリセットできます。

### 3. Configure the Grid

行数と列数を指定して、グリッドのサイズを変更できます。
出力画像のサイズは以下のようになります。

`Width = Columns × 64`
`Height = Rows × 64`

例えば、8列 × 8行の場合は512 × 512ピクセルになります。

### 4. Export

「出力」をクリックし、保存先を指定するとBMP画像が生成されます。
空のマスは黒色で出力されます。

## Build

### Requirements

- Windows 10 / 11
- C++20 compatible compiler (MSVC recommended)
- CMake 3.24 or later
- DirectX 12 compatible GPU

### Build with CMake

```bash
git clone https://github.com/esu0108/gridtool.git
cd gridtool

cmake -S . -B build
cmake --build build --config Release
```

外部ライブラリはCMake FetchContentによって自動的に取得されます。

## Dependencies

This project uses the following libraries:

- [Dear ImGui](https://github.com/ocornut/imgui) — Immediate Mode GUI
- [stb_image](https://github.com/nothings/stb) — Image loading
- [stb_image_resize2](https://github.com/nothings/stb_image_resize) — Image resizing

## Technology

- C++20
- Windows API
- DirectX 12
- Dear ImGui
- CMake