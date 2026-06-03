from PIL import Image
import os

folder = os.path.dirname(os.path.abspath(__file__))
size = (60, 60)

for fname in os.listdir(folder):
    ext = os.path.splitext(fname)[1].lower()
    if ext not in ('.png', '.jpg', '.jpeg'):
        continue
    path = os.path.join(folder, fname)
    is_png = ext == '.png'
    img = Image.open(path).convert('RGBA' if is_png else 'RGB')
    img = img.resize(size, Image.LANCZOS)
    img.save(path)
    print(f'Saved {fname} → {size[0]}×{size[1]}')
