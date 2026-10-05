"""Wiring diagram generator.

Every pin it draws comes from db/boards.yaml. The LLM is never consulted:
a hallucinated pin here is not a bug, it is a burnt board.
"""
from pathlib import Path
import yaml
from wireviz.wireviz import parse

LOGIC_COLORS = ["YE", "GN", "BU", "OG", "VT", "WH", "RD", "BK"]


def _esp32_can(b, i, conn, cables, links, db):
    pins = b["spec"]["can"]["pins"]
    pw = b["spec"]["power"]
    t = f"T{i}"
    conn[b["id"]] = {"type": b["spec"]["name"], "subtype": b["id"],
                     "pinlabels": [pins["tx"], pins["rx"], pw["vcc"], pw["gnd"]]}
    conn[t] = {"type": db["modules"]["sn65hvd230"]["name"],
               "pinlabels": db["modules"]["sn65hvd230"]["pins"]}
    w = f"L{i}"
    cables[w] = {"wirecount": 4, "colors": ["YE", "GN", "RD", "BK"], "category": "bundle"}
    links.append([{b["id"]: [pins["tx"], pins["rx"], pw["vcc"], pw["gnd"]]},
                  {w: [1, 2, 3, 4]},
                  {t: ["CTX", "CRX", "3V3", "GND"]}])
    return t


def _mcp2515_can(b, i, conn, cables, links, db):
    pins = b["spec"]["can"]["pins"]
    pw = b["spec"]["power"]
    t = f"T{i}"
    mod = db["modules"]["mcp2515_tja1050"]
    order = [pins["sck"], pins["miso"], pins["mosi"], pins["cs"], pins["int"], pw["vcc"], pw["gnd"]]
    conn[b["id"]] = {"type": b["spec"]["name"], "subtype": b["id"], "pinlabels": order}
    conn[t] = {"type": mod["name"], "pinlabels": mod["pins"]}
    w = f"L{i}"
    cables[w] = {"wirecount": 7, "colors": ["YE", "GN", "BU", "OG", "VT", "RD", "BK"],
                 "category": "bundle"}
    links.append([{b["id"]: order},
                  {w: list(range(1, 8))},
                  {t: ["SCK", "SO", "SI", "CS", "INT", "VCC", "GND"]}])
    return t


def build_wiring(inventory, db, out_dir, warnings=()):
    conn, cables, links, transceivers = {}, {}, [], []
    for i, b in enumerate(inventory, start=1):
        if b["spec"]["can"]["native"]:
            transceivers.append(_esp32_can(b, i, conn, cables, links, db))
        else:
            transceivers.append(_mcp2515_can(b, i, conn, cables, links, db))

    # CAN bus: daisy-chain transceivers, 120 ohm termination at both physical ends
    conn["R_A"] = {"type": "120 Ω termination", "subtype": "bus end A", "pinlabels": ["CANH", "CANL"]}
    conn["R_B"] = {"type": "120 Ω termination", "subtype": "bus end B", "pinlabels": ["CANH", "CANL"]}
    chain = ["R_A"] + transceivers + ["R_B"]
    for k in range(len(chain) - 1):
        w = f"CAN{k + 1}"
        cables[w] = {"wirecount": 2, "colors": ["OG", "WH"], "gauge": "twisted pair",
                     "category": "bundle"}
        links.append([{chain[k]: ["CANH", "CANL"]}, {w: [1, 2]}, {chain[k + 1]: ["CANH", "CANL"]}])

    doc = {
        "metadata": {"title": "Muster wiring plan"},
        "connectors": conn,
        "cables": cables,
        "connections": links,
    }
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "wiring.yaml").write_text(yaml.safe_dump(doc, sort_keys=False, allow_unicode=True))
    parse(doc, output_formats=("svg", "png"), output_dir=out_dir, output_name="wiring")

    notes = [
        "Use twisted pair for CANH/CANL; keep stubs from bus to each transceiver short.",
        "Fit exactly two 120 Ω terminators, one at each physical end of the bus.",
        "All boards must share a common ground.",
    ] + list(warnings)
    (out_dir / "WIRING_NOTES.txt").write_text("\n".join(f"- {n}" for n in notes) + "\n")
    return out_dir / "wiring.svg"
