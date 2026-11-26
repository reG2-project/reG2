# Writes one frame design's pictures as the game embeds them (frame, glass, screen mask at 1x and 2x), the way
# make_all.py writes the study's: e.g. the slim frame E the minimap uses.
# usage: python3 -I export_frame.py <design function in designs.py> <out dir>
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image
import designs
from mm_lib import SS, downsample


def soft(arr, scale):
    H, W = arr.shape
    return np.asarray(Image.fromarray(arr.astype(np.float32), 'F').resize(
        (int(round(W / SS * scale)), int(round(H / SS * scale))), Image.BOX))


def main():
    d = getattr(designs, sys.argv[1])()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for s in (1, 2):
        downsample(d.rgb, d.canvas.alpha, s, 128).save(os.path.join(out, 'frame_%dx.png' % s))
        grgb, ga = d.glass
        rgb = np.dstack([soft(grgb[..., i], s) for i in range(3)])
        a = soft(ga, s)
        Image.fromarray(np.dstack([np.clip(rgb, 0, 1) * 255, np.clip(a, 0, 1) * 255]).astype(np.uint8), 'RGBA').save(
            os.path.join(out, 'glass_%dx.png' % s))
        m = soft((d.screen_sd < 0).astype(np.float32), s)
        Image.fromarray((m * 255).astype(np.uint8), 'L').save(os.path.join(out, 'screen_mask_%dx.png' % s))


if __name__ == '__main__':
    main()
