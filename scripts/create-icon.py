"""将用户提供的 PNG 转为带透明通道的多尺寸 Windows 图标。

仅用于开发时更新图标，需要 Pillow；应用运行和构建使用已提交的资源，
不需要 Python。示例：python scripts/create-icon.py
"""

from pathlib import Path
import argparse
import shutil

from PIL import Image


def main() -> None:
    parser = argparse.ArgumentParser(description="生成光剑曲谱制作的 Windows 图标")
    parser.add_argument("source", nargs="?", type=Path,
                        help="可选的原始 PNG 路径；会原样复制到资源目录")
    args = parser.parse_args()
    icon_dir = Path(__file__).resolve().parent.parent / "resources" / "icons"
    icon_dir.mkdir(parents=True, exist_ok=True)
    png_path = icon_dir / "app.png"
    if args.source and args.source.resolve() != png_path.resolve():
        shutil.copyfile(args.source, png_path)
    with Image.open(png_path) as original:
        image = original.convert("RGBA")
        image.save(icon_dir / "app.ico", format="ICO",
                   sizes=[(size, size) for size in (16, 24, 32, 48, 64, 128, 256)])
    print(f"已生成：{icon_dir / 'app.ico'}")


if __name__ == "__main__":
    main()
