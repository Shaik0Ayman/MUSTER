# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
"""Which boards share one firmware image, and what node ID each gets.

Default (legacy) project: one PlatformIO env per board, node ID = position in
the inventory.

Shared-image project (`shared_images: true` in boards.yaml): every board whose
type can read a chip-unique number (`uid: efuse_mac` in the board DB) and is
not the lead is flashed with ONE image per board type. It takes its node ID
from that number at run time (muster_set_uid), so three ESP32s are three
copies of the same build. Everything else keeps its own env and a fixed ID
from 200 up (the lead is always node 0). Hashed IDs are 1..199, so the two
ranges cannot collide.
"""

FIXED_BASE = 200


def project_shared(inventory):
    return any(b.get("shared_images") for b in inventory)


def is_shared(b, lead):
    return bool(b.get("shared_images")) and b["spec"].get("uid") == "efuse_mac" \
        and b["id"] != lead


def _env(name):
    return name.replace("-", "_")


def images(inventory, lead):
    """Ordered list of images: {env, boards[], node_id|None, shared}.

    node_id is None for a shared image (IDs come from the chip)."""
    out, by_type = [], {}
    fixed = 0
    shared_project = project_shared(inventory)
    for i, b in enumerate(inventory):
        if is_shared(b, lead):
            img = by_type.get(b["type"])
            if img is None:
                img = {"env": _env(b["type"]), "boards": [], "node_id": None, "shared": True}
                by_type[b["type"]] = img
                out.append(img)
            img["boards"].append(b)
            continue
        if not shared_project:
            node_id = i
        elif b["id"] == lead:
            node_id = 0
        else:
            node_id, fixed = FIXED_BASE + fixed, fixed + 1
        out.append({"env": _env(b["id"]), "boards": [b], "node_id": node_id, "shared": False})
    return out


def env_for(b, inventory, lead):
    for img in images(inventory, lead):
        if any(x["id"] == b["id"] for x in img["boards"]):
            return img["env"]
    raise KeyError(b["id"])
