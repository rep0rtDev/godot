#!/usr/bin/env python3
"""Compare upscaled shots against a native one: mean absolute difference, outlier counts and grayscale SSIM.

Usage: compare_images.py native.png candidate.png [candidate2.png ...]
"""

import sys

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view
from PIL import Image


def ssim(a, b):
    k = np.exp(-((np.arange(11) - 5) ** 2) / (2 * 1.5**2))
    k /= k.sum()
    kernel = np.outer(k, k)

    def blur(x):
        return np.einsum("ijkl,kl->ij", sliding_window_view(x, (11, 11)), kernel)

    mu_a, mu_b = blur(a), blur(b)
    va, vb, cov = blur(a * a) - mu_a**2, blur(b * b) - mu_b**2, blur(a * b) - mu_a * mu_b
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    return float((((2 * mu_a * mu_b + c1) * (2 * cov + c2)) / ((mu_a**2 + mu_b**2 + c1) * (va + vb + c2))).mean())


def main():
    ref = Image.open(sys.argv[1]).convert("RGB")
    ref_rgb = np.asarray(ref).astype(float)
    ref_l = np.asarray(ref.convert("L")).astype(float)
    for path in sys.argv[2:]:
        img = Image.open(path).convert("RGB")
        d = np.abs(np.asarray(img).astype(float) - ref_rgb).max(axis=2)
        print(
            f"{path}: mean abs {np.abs(np.asarray(img).astype(float) - ref_rgb).mean():.3f} diff>16 {int((d > 16).sum())} diff>48 {int((d > 48).sum())} SSIM {ssim(np.asarray(img.convert('L')).astype(float), ref_l):.4f}"
        )


if __name__ == "__main__":
    main()
