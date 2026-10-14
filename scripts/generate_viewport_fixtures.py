#!/usr/bin/env python3
"""Write deterministic, credential-free geometry for later native visual checks."""
import json
import math
from pathlib import Path


def box(x, y, z, width, depth, height):
    vertices = [(x, y, z), (x + width, y, z), (x + width, y + depth, z), (x, y + depth, z),
                (x, y, z + height), (x + width, y, z + height),
                (x + width, y + depth, z + height), (x, y + depth, z + height)]
    triangles = []
    for a, b, c, d in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
                       (3, 7, 6, 2), (0, 4, 7, 3), (1, 2, 6, 5)):
        triangles.extend(((vertices[a], vertices[b], vertices[c]), (vertices[a], vertices[c], vertices[d])))
    return triangles


def prism(outline, height):
    triangles = []
    # Fixtures use convex outlines, so a centre fan is sufficient for the caps.
    centre = tuple(sum(p[i] for p in outline) / len(outline) for i in (0, 1))
    for i, p in enumerate(outline):
        q = outline[(i + 1) % len(outline)]
        a, b = (*p, 0), (*q, 0)
        c, d = (*q, height), (*p, height)
        triangles.extend(((a, b, c), (a, c, d), ((*centre, 0), b, a), ((*centre, height), d, c)))
    return triangles


def sphere(radius, slices=128, rings=64):
    def point(row, col):
        latitude = math.pi * row / rings
        longitude = 2 * math.pi * col / slices
        return (radius * math.sin(latitude) * math.cos(longitude),
                radius * math.sin(latitude) * math.sin(longitude), radius * (1 + math.cos(latitude)))
    triangles = []
    for row in range(rings):
        for col in range(slices):
            a, b, c, d = point(row, col), point(row + 1, col), point(row + 1, col + 1), point(row, col + 1)
            if row != 0:
                triangles.append((a, b, d))
            if row != rings - 1:
                triangles.append((b, c, d))
    return triangles


def write_stl(path, triangles):
    lines = ["solid viewport_fixture"]
    for a, b, c in triangles:
        u, v = [b[i] - a[i] for i in range(3)], [c[i] - a[i] for i in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        length = math.sqrt(sum(value * value for value in n))
        if length < 1e-12:
            raise ValueError(f"Degenerate triangle in {path.name}")
        lines.append("facet normal " + " ".join(f"{value / length:.8g}" for value in n))
        lines.append(" outer loop")
        for vertex in (a, b, c):
            lines.append("  vertex " + " ".join(f"{value:.8g}" for value in vertex))
        lines.extend((" endloop", "endfacet"))
    lines.append("endsolid viewport_fixture")
    path.write_text("\n".join(lines) + "\n")


def main():
    output = Path(__file__).resolve().parents[1] / "artifacts" / "prepare-studio-fixtures"
    output.mkdir(parents=True, exist_ok=True)
    shapes = {
        "beveled_cube": prism([(3, 0), (27, 0), (30, 3), (30, 27), (27, 30), (3, 30), (0, 27), (0, 3)], 30),
        "sphere": sphere(20),
        "cylinder": prism([(15 * math.cos(2 * math.pi * i / 96), 15 * math.sin(2 * math.pi * i / 96)) for i in range(96)], 35),
        # Composite fixtures are display geometry, not slicing/printability qualification.
        "pocket": box(0, 0, 0, 30, 30, 3) + box(0, 0, 3, 3, 30, 17) + box(27, 0, 3, 3, 30, 17)
                  + box(3, 0, 3, 24, 3, 17) + box(3, 27, 3, 24, 3, 17),
        "thin_wall": box(0, 0, 0, 40, .8, 30),
        "overhang": box(0, 0, 0, 8, 15, 30) + box(32, 0, 0, 8, 15, 30) + box(0, 0, 30, 40, 15, 5),
    }
    for name, triangles in shapes.items():
        write_stl(output / f"{name}.stl", triangles)
    manifest = {
        "units": "mm", "up": "z", "geometry": [f"{name}.stl" for name in shapes],
        "colors": {"white": "#FFFFFF", "black": "#111111", "blue": "#1875D1"},
        "bed_examples": [
            {"name": "Rectangle", "origin": [0, 0], "outline": [[0, 0], [180, 0], [180, 180], [0, 180]]},
            {"name": "Circle", "origin": [220, 0], "outline": [[90 + 90 * math.cos(i * 2 * math.pi / 64), 90 + 90 * math.sin(i * 2 * math.pi / 64)] for i in range(64)]},
            {"name": "Concave", "origin": [450, 30], "outline": [[0, 0], [200, 0], [200, 120], [100, 120], [100, 180], [0, 180]]},
        ],
        "use": "Manual visual fixtures only; no printer destination or print commands.",
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Wrote {len(shapes)} geometry fixtures to {output}")


if __name__ == "__main__":
    main()
