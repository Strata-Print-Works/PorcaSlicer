#!/usr/bin/env python3
"""Generate Porca's synthetic studio, using only local numpy and Pillow.

Eight GGX-prefiltered equirectangular tiles have one-pixel gutters, RGBM range 8.
Coordinates are Z-up; the key matches GLCanvas3D's STATIC_LIGHT_DIR. No reference
photos or MultiSlicer assets are copied. Re-run with the bundled Python runtime.
"""
from pathlib import Path
import math

import numpy as np
from PIL import Image

WIDTH, HEIGHT, LEVELS, SAMPLES = 256, 128, 8, 64


def normalize(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-9)


def radical_inverse(i):
    result, factor = 0.0, 0.5
    while i:
        result += (i & 1) * factor
        factor *= 0.5
        i >>= 1
    return result


def room(direction):
    sky = np.clip(direction[..., 2:3] * 0.5 + 0.5, 0, 1)
    color = np.array([0.10, 0.09, 0.08]) * (1 - sky) + np.array([0.30, 0.32, 0.35]) * sky
    for axis, amplitude, spread, tint in (
        ((-.4, -.6, 1), 4.0, 24.0, (1.0, .98, .95)),
        ((.8, .3, .55), 1.2, 12.0, (.9, .96, 1.0)),
        ((-.3, .8, .6), .8, 18.0, (1.0, 1.0, 1.0)),
    ):
        axis = normalize(np.array(axis, dtype=float))
        softbox = np.exp((direction @ axis - 1) * spread)[..., None]
        color = color + softbox * amplitude * np.array(tint)
    return color


def ggx_half(normal, roughness, index):
    # Importance sampling in the tangent frame. alpha = perceptual roughness².
    alpha = max(roughness * roughness, .002)
    u, v = (index + .5) / SAMPLES, radical_inverse(index)
    cos_theta = math.sqrt((1 - v) / (1 + (alpha * alpha - 1) * v))
    sin_theta = math.sqrt(max(0, 1 - cos_theta * cos_theta))
    axis = np.zeros_like(normal)
    axis[..., 2] = 1
    axis[np.abs(normal[..., 2]) > .99] = (0, 1, 0)
    tangent = normalize(np.cross(axis, normal))
    bitangent = np.cross(normal, tangent)
    return (tangent * (math.cos(2 * math.pi * u) * sin_theta) +
            bitangent * (math.sin(2 * math.pi * u) * sin_theta) + normal * cos_theta)


def radiance_atlas():
    # Row zero is v=0 in the uploaded texture, matching the shader's mapping.
    u = (np.arange(WIDTH) + .5) / WIDTH
    v = (np.arange(HEIGHT) + .5) / HEIGHT
    phi, theta = np.meshgrid((u - .5) * 2 * math.pi, v * math.pi)
    normal = np.stack((np.sin(theta) * np.cos(phi), np.sin(theta) * np.sin(phi), np.cos(theta)), axis=-1)
    tiles = []
    for level in range(LEVELS):
        roughness = level / (LEVELS - 1)
        if level == 0:
            filtered = room(normal)
        else:
            total = np.zeros_like(normal)
            weight = np.zeros((*normal.shape[:2], 1))
            for sample in range(SAMPLES):
                half = ggx_half(normal, roughness, sample)
                light = normalize(2 * np.sum(normal * half, axis=-1, keepdims=True) * half - normal)
                nl = np.maximum(np.sum(normal * light, axis=-1, keepdims=True), 0)
                total += room(light) * nl
                weight += nl
            filtered = total / np.maximum(weight, 1e-9)
        multiplier = np.ceil(np.clip(filtered.max(axis=-1, keepdims=True) / 8, 1 / 255, 1) * 255) / 255
        rgbm = np.concatenate((np.clip(filtered / (multiplier * 8), 0, 1), multiplier), axis=-1)
        tile = np.pad(rgbm, ((1, 1), (1, 1), (0, 0)), mode="edge")
        # Horizontal gutters wrap at the longitude seam.
        tile[1:-1, 0] = rgbm[:, -1]
        tile[1:-1, -1] = rgbm[:, 0]
        tiles.append(tile)
    return np.rint(np.concatenate(tiles, axis=0) * 255).astype(np.uint8)


def brdf_lookup():
    size = 128
    nv, roughness = np.meshgrid((np.arange(size) + .5) / size, (np.arange(size) + .5) / size)
    view = np.stack((np.sqrt(1 - nv * nv), np.zeros_like(nv), nv), axis=-1)
    # Integrate the split-sum Fresnel coefficients for a GGX specular lobe.
    a = np.zeros_like(nv)
    b = np.zeros_like(nv)
    for index in range(SAMPLES):
        phi = 2 * math.pi * ((index + .5) / SAMPLES)
        xi = radical_inverse(index)
        alpha = np.maximum(roughness * roughness, .002)
        hz = np.sqrt((1 - xi) / (1 + (alpha * alpha - 1) * xi))
        hs = np.sqrt(np.maximum(0, 1 - hz * hz))
        half = np.stack((hs * math.cos(phi), hs * math.sin(phi), hz), axis=-1)
        vh = np.maximum(np.sum(view * half, axis=-1), 0)
        light = 2 * vh[..., None] * half - view
        nl = np.maximum(light[..., 2], 0)
        k = alpha * .5
        gv = nv / (nv * (1 - k) + k)
        gl = nl / np.maximum(nl * (1 - k) + k, 1e-9)
        visibility = gv * gl * vh / np.maximum(hz * nv, 1e-9)
        fresnel = (1 - vh) ** 5
        a += (1 - fresnel) * visibility
        b += fresnel * visibility
    rgb = np.stack((a / SAMPLES, b / SAMPLES, np.zeros_like(a)), axis=-1)
    return np.rint(np.clip(rgb, 0, 1) * 255).astype(np.uint8)


if __name__ == "__main__":
    output = Path(__file__).resolve().parents[1] / "resources" / "lighting"
    output.mkdir(exist_ok=True)
    Image.fromarray(radiance_atlas()).save(output / "studio-radiance.png")
    Image.fromarray(brdf_lookup()).save(output / "studio-brdf.png")
    print(f"Studio resources written to {output}")
