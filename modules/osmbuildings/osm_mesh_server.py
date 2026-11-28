import math
import socket
import struct
from contextlib import asynccontextmanager
from pathlib import Path
from typing import List, Tuple

import mapbox_vector_tile
import numpy as np
import requests
import urllib3.util.connection as urllib3_cn
from fastapi import FastAPI, Response
from shapely import constrained_delaunay_triangles
from shapely.geometry import Polygon, shape

# Force IPv4 to avoid Errno 101 on systems without IPv6 routing
urllib3_cn.allowed_gai_family = lambda: socket.AF_INET

CACHE_DIR = Path("./cache")
CACHE_DIR.mkdir(exist_ok=True)

# WGS84 ellipsoid constants (matches OpenSpace Earth globe)
WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563
WGS84_E2 = WGS84_F * (2.0 - WGS84_F)

TILE_URL_TEMPLATE: str = ""


def resolve_openfreemap_template() -> str:
    """Fetches the current OpenFreeMap Planet TileJSON to get the active PBF URL template."""
    try:
        r = requests.get("https://tiles.openfreemap.org/planet", timeout=10)
        r.raise_for_status()
        tiles = r.json().get("tiles", [])
        if tiles:
            print(f"[Info] Resolved OpenFreeMap vector tile URL: {tiles[0]}")
            return tiles[0]
    except Exception as e:
        print(f"[Warn] Could not resolve OpenFreeMap TileJSON ({e}), using fallback.")
    return "https://tiles.openfreemap.org/planet/20261122_124456_pt/{z}/{x}/{y}.pbf"


@asynccontextmanager
async def lifespan(app: FastAPI):
    global TILE_URL_TEMPLATE
    TILE_URL_TEMPLATE = resolve_openfreemap_template()
    yield


app = FastAPI(lifespan=lifespan)


def tile_to_bbox(z: int, x: int, y: int) -> Tuple[float, float, float, float]:
    """Returns (south, west, north, east) in degrees for slippy tile (z, x, y)."""
    n = 2.0 ** z
    west = x / n * 360.0 - 180.0
    east = (x + 1) / n * 360.0 - 180.0
    north = math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * y / n))))
    south = math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * (y + 1) / n))))
    return south, west, north, east


def geodetic_to_ecef(lat_deg: float, lon_deg: float, alt_m: float) -> Tuple[float, float, float]:
    """Converts WGS84 (lat, lon, alt) to Earth-Centered Earth-Fixed (ECEF) meters."""
    lat = math.radians(lat_deg)
    lon = math.radians(lon_deg)
    sin_lat = math.sin(lat)
    cos_lat = math.cos(lat)
    N = WGS84_A / math.sqrt(1.0 - WGS84_E2 * sin_lat * sin_lat)
    x = (N + alt_m) * cos_lat * math.cos(lon)
    y = (N + alt_m) * cos_lat * math.sin(lon)
    z = (N * (1.0 - WGS84_E2) + alt_m) * sin_lat
    return x, y, z


def fetch_ground_elevation(lat: float, lon: float) -> float:
    """Fetches approximate terrain elevation in meters for the tile center."""
    try:
        resp = requests.get(
            f"https://api.open-meteo.com/v1/elevation?latitude={lat}&longitude={lon}",
            timeout=6,
        )
        resp.raise_for_status()
        return float(resp.json()["elevation"][0])
    except Exception:
        return 0.0


def parse_height(props: dict) -> float:
    """Extracts building height in meters from OpenMapTiles / OSM vector tile properties."""
    for key in ("render_height", "height"):
        if key in props and props[key] is not None:
            try:
                val = str(props[key]).lower().replace("m", "").strip()
                return max(3.0, float(val))
            except ValueError:
                pass
    for key in ("building:levels", "levels"):
        if key in props and props[key] is not None:
            try:
                return max(3.0, float(props[key]) * 3.5)
            except ValueError:
                pass
    return 10.0


def triangulate_roof_ccw(poly: Polygon) -> List[Tuple[Tuple[float, float], Tuple[float, float], Tuple[float, float]]]:
    """Triangulates a polygon into CCW (lon, lat) triangles using Shapely/GEOS."""
    tris_geom = constrained_delaunay_triangles(poly)
    triangles = []
    for tri in tris_geom.geoms:
        coords = list(tri.exterior.coords)[:-1]
        if len(coords) != 3:
            continue
        if not tri.exterior.is_ccw:
            coords = coords[::-1]
        triangles.append((coords[0], coords[1], coords[2]))
    return triangles


def build_tile_mesh(z: int, x: int, y: int) -> bytes:
    """
    Fetches a pre-tiled MVT (.pbf) from OpenFreeMap (max native zoom 14),
    clips/maps to requested (z, x, y), and extrudes 3D buildings into ECEF binary mesh.
    """
    south, west, north, east = tile_to_bbox(z, x, y)
    center_lat = 0.5 * (south + north)
    center_lon = 0.5 * (west + east)

    ground_alt = fetch_ground_elevation(center_lat, center_lon)
    origin_ecef = geodetic_to_ecef(center_lat, center_lon, ground_alt)

    # OpenFreeMap max zoom is 14; if OpenSpace requests z=15, fetch parent z=14 tile
    src_z = min(z, 14)
    dz = z - src_z
    src_x = x >> dz
    src_y = y >> dz

    url = TILE_URL_TEMPLATE.format(z=src_z, x=src_x, y=src_y)
    resp = requests.get(url, timeout=15)
    if resp.status_code == 404:
        return struct.pack("<II3d", 0, 0, *origin_ecef)
    resp.raise_for_status()

    decoded = mapbox_vector_tile.decode(resp.content)
    building_layer = decoded.get("building", {})
    features = building_layer.get("features", [])
    extent = float(building_layer.get("extent", 4096))

    src_south, src_west, src_north, src_east = tile_to_bbox(src_z, src_x, src_y)

    vertices: List[float] = []
    indices: List[int] = []
    vert_offset = 0

    for feat in features:
        geom_dict = feat.get("geometry")
        if not geom_dict:
            continue
        geom = shape(geom_dict)
        if geom.is_empty:
            continue

        polys = [geom] if geom.geom_type == "Polygon" else (
            list(geom.geoms) if geom.geom_type == "MultiPolygon" else []
        )

        height = parse_height(feat.get("properties", {}))
        base_alt = ground_alt - 15.0
        roof_alt = ground_alt + height

        for poly in polys:
            if not poly.is_valid:
                poly = poly.buffer(0)
            if poly.is_empty or poly.geom_type != "Polygon":
                continue

            if not poly.exterior.is_ccw:
                raw_pts = list(poly.exterior.coords)[::-1][:-1]
            else:
                raw_pts = list(poly.exterior.coords)[:-1]

            if len(raw_pts) < 3:
                continue

            # Convert MVT tile-local coordinates to (lon, lat)
            ext_coords = []
            for tx, ty in raw_pts:
                u_src = tx / extent
                v_src = 1.0 - (ty / extent)
                lon = src_west + u_src * (src_east - src_west)
                lat = src_north - v_src * (src_north - src_south)
                ext_coords.append((lon, lat))

            # Skip buildings outside the requested z=15 sub-tile
            c_lon = sum(p[0] for p in ext_coords) / len(ext_coords)
            c_lat = sum(p[1] for p in ext_coords) / len(ext_coords)
            if not (west <= c_lon <= east and south <= c_lat <= north):
                continue

            geo_poly = Polygon(ext_coords)
            if not geo_poly.is_valid:
                geo_poly = geo_poly.buffer(0)
            if geo_poly.is_empty or geo_poly.geom_type != "Polygon":
                continue

            p0 = np.array(geodetic_to_ecef(c_lat, c_lon, ground_alt))
            p1 = np.array(geodetic_to_ecef(c_lat, c_lon, ground_alt + 10.0))
            up = (p1 - p0) / np.linalg.norm(p1 - p0)

            # 1. Roof triangles
            roof_tris = triangulate_roof_ccw(geo_poly)
            for tri in roof_tris:
                for lon, lat in tri:
                    rx, ry, rz = geodetic_to_ecef(lat, lon, roof_alt)
                    u = (lon - west) / (east - west)
                    v = (north - lat) / (north - south)
                    vertices.extend([
                        float(rx - origin_ecef[0]),
                        float(ry - origin_ecef[1]),
                        float(rz - origin_ecef[2]),
                        float(up[0]), float(up[1]), float(up[2]),
                        float(u), float(v),
                    ])
                indices.extend([vert_offset, vert_offset + 1, vert_offset + 2])
                vert_offset += 3

            # 2. Extruded Wall Quads
            n_pts = len(ext_coords)
            for i in range(n_pts):
                lon0, lat0 = ext_coords[i]
                lon1, lat1 = ext_coords[(i + 1) % n_pts]

                t0 = np.array(geodetic_to_ecef(lat0, lon0, roof_alt)) - np.array(origin_ecef)
                t1 = np.array(geodetic_to_ecef(lat1, lon1, roof_alt)) - np.array(origin_ecef)
                b0 = np.array(geodetic_to_ecef(lat0, lon0, base_alt)) - np.array(origin_ecef)
                b1 = np.array(geodetic_to_ecef(lat1, lon1, base_alt)) - np.array(origin_ecef)

                edge = t1 - t0
                wall_n = np.cross(edge, up)
                norm_len = np.linalg.norm(wall_n)
                wall_n = wall_n / norm_len if norm_len > 1e-6 else up

                u0 = (lon0 - west) / (east - west)
                v0 = (north - lat0) / (north - south)
                u1 = (lon1 - west) / (east - west)
                v1 = (north - lat1) / (north - south)

                for pos, u, v in [(t0, u0, v0), (b0, u0, v0), (b1, u1, v1), (t1, u1, v1)]:
                    vertices.extend([
                        float(pos[0]), float(pos[1]), float(pos[2]),
                        float(wall_n[0]), float(wall_n[1]), float(wall_n[2]),
                        float(u), float(v),
                    ])

                indices.extend([
                    vert_offset + 0, vert_offset + 1, vert_offset + 2,
                    vert_offset + 0, vert_offset + 2, vert_offset + 3,
                ])
                vert_offset += 4

    v_arr = np.array(vertices, dtype=np.float32)
    i_arr = np.array(indices, dtype=np.uint32)

    header = struct.pack(
        "<II3d",
        len(vertices) // 8,
        len(indices),
        origin_ecef[0],
        origin_ecef[1],
        origin_ecef[2],
    )
    print(
            f"[OK] Built tile {z}/{x}/{y} "
            f"(lat={center_lat:.4f}, lon={center_lon:.4f}, alt={ground_alt:.1f}m): "
            f"{len(indices)//3} tris"
        )
    return header + v_arr.tobytes() + i_arr.tobytes()


@app.get("/tile/{z}/{x}/{y}.bin")
def get_tile(z: int, x: int, y: int):
    cache_file = CACHE_DIR / f"{z}_{x}_{y}.bin"
    if cache_file.exists():
        south, west, north, east = tile_to_bbox(z, x, y)
        print(f"[Cache] Serving {z}/{x}/{y} (lat={0.5*(south+north):.4f}, lon={0.5*(west+east):.4f})")
        return Response(content=cache_file.read_bytes(), media_type="application/octet-stream")

    try:
        blob = build_tile_mesh(z, x, y)
    except Exception as e:
        print(f"[Error] Failed to build tile {z}/{x}/{y}: {e}")
        return Response(status_code=503)

    cache_file.write_bytes(blob)
    return Response(content=blob, media_type="application/octet-stream")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=8765)