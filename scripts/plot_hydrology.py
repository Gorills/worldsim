#!/usr/bin/env python3
"""Render worldsim_hydrology_dump CSVs; requires matplotlib (no runtime dependency)."""
import argparse
import csv
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LogNorm
import numpy as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
args = parser.parse_args()
folder = args.directory
meta = json.loads((folder / "summary.json").read_text())

def columns(name):
    with (folder / name).open(newline="") as source:
        rows = list(csv.DictReader(source))
    return {key: np.array([float(row[key]) for row in rows]) for key in rows[0]}

plt.rcParams.update({"font.size": 10, "axes.spines.top": False,
                     "axes.spines.right": False, "figure.facecolor": "#f8fafc",
                     "axes.facecolor": "white", "savefig.facecolor": "#f8fafc"})
h = columns("history.csv")
fig, axes = plt.subplots(3, 2, figsize=(13, 10), layout="constrained")
fig.suptitle(f"WorldSim basin hydrology · seed {meta['seed']} · {meta['days']} days", fontsize=17)
for key, label in [("snow_m3", "Snow"), ("soil_m3", "Root zone"),
                   ("groundwater_m3", "Groundwater"), ("surface_m3", "Surface")]:
    axes[0, 0].plot(h["day"], h[key] / 1e9, label=label)
axes[0, 0].set(title="Persistent land-water stores", ylabel="km³")
axes[0, 0].legend(ncol=2)
for key, label in [("precipitation_m3", "Precipitation"),
                   ("evaporation_m3", "Evaporation + transpiration"),
                   ("ocean_export_m3", "Ocean export")]:
    axes[0, 1].plot(h["day"], np.diff(h[key], prepend=h[key][0]) / 1e9, label=label)
axes[0, 1].set(title="Daily external water exchanges", ylabel="km³/day")
axes[0, 1].legend()
axes[1, 0].plot(h["day"], h["reach_discharge_m3_day"] / 86400, color="#2563eb")
axes[1, 0].set(title="Monitored downstream reach · daily mean", ylabel="m³/s")
axes[1, 1].plot(h["day"], h["reach_level_m"], label="Water surface", color="#0891b2")
axes[1, 1].plot(h["day"], h["reach_spill_m"], label="Potential outlet sill", color="#a16207", linestyle="--")
axes[1, 1].set(title="Monitored water level", ylabel="m above sea datum")
axes[1, 1].legend()
axes[2, 0].plot(h["day"], h["basin_water_m3"] / 1e9, color="#0f766e")
axes[2, 0].set(title="Monitored catchment · all water stores", ylabel="km³")
axes[2, 1].plot(h["day"], h["budget_residual_m3"] / 1e9, color="#7c3aed")
axes[2, 1].set(title=f"Water-budget residual · max relative {meta['max_relative_water_budget_error']:.2e}", ylabel="km³")
for ax in axes.flat:
    ax.set_xlabel("Simulation day")
    ax.grid(alpha=0.2)
fig.savefig(folder / "history.png", dpi=150)
plt.close(fig)

m = columns("map.csv")
fig, axes = plt.subplots(2, 2, figsize=(14, 8), layout="constrained")
fig.suptitle(f"Authoritative hydrology · day {meta['days']} · reference level {meta['reference_level']}", fontsize=17)
ocean = m["bed_m"].reshape(180, 360) < 0
for ax, key, title, unit, cmap in [
    (axes[0, 0], "depth_m", "Stored surface-water depth", "m", "Blues"),
    (axes[0, 1], "discharge_m3_day", "Routed discharge · final interval", "m³/day", "viridis"),
    (axes[1, 0], "snow_depth_m", "Snow water equivalent", "m", "PuBu"),
    (axes[1, 1], "groundwater_depth_m", "Shallow groundwater / land area", "m", "YlGnBu"),
]:
    data = np.ma.masked_where(ocean, m[key].reshape(180, 360))
    valid = data.compressed()
    positive = valid[valid > 0]
    norm = None
    if key in ("depth_m", "discharge_m3_day") and positive.size:
        lo = max(float(positive.min()), float(positive.max()) * 1e-5)
        hi = max(lo * 10, float(positive.max()))
        data = np.ma.masked_where(data <= 0, data)
        norm = LogNorm(vmin=lo, vmax=hi)
    ax.set_facecolor("#dce7ef")
    im = ax.imshow(data, extent=(-180, 180, -90, 90), origin="upper", cmap=cmap,
                   interpolation="nearest", norm=norm, aspect="auto")
    ax.set(title=title, xlabel="Longitude", ylabel="Latitude")
    fig.colorbar(im, ax=ax, label=unit, shrink=0.85)
fig.savefig(folder / "maps.png", dpi=150)
plt.close(fig)
print(folder / "history.png")
print(folder / "maps.png")
