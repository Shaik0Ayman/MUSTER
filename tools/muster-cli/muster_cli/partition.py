# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
"""Deterministic partitioner: decide which boards hold which task images, and
who leads. No model involved.

Plans can also come from an assistant through the MCP server (mcp_server.py)
or from a hand-written plan.json. Every plan, whatever its source, has this
shape and goes through validate() before anything is generated:

  {"lead": board_id,
   "assignments": [{"task": name, "boards": [board_id, ...]}, ...],
   "modules": [{"module": name, "boards": [candidate ids], "count": n}, ...],
   "rationale": "...", "source": "baseline" | "mcp" | "file:<path>"}

`modules` is present only when the program has MUSTER_MODULEs. `boards` are
the boards that MAY run the module; the lead picks `count` of the live ones at
run time and moves a module when its board is lost.

The baseline is also the yardstick: a proposed plan should beat it (fewer
single-board tasks, a better lead) or the baseline is the better choice.
"""


def eligible_boards(task, inventory, module=False):
    """Boards with every capability the task needs and enough RAM (for a
    module, also with room for the module bookkeeping)."""
    return [b["id"] for b in inventory
            if set(task["needs"]) <= set(b["spec"]["flags"])
            and task["ram_kb"] <= b["spec"]["ram_kb"]
            and (not module or b["spec"]["runtime"].get("max_modules", 0) > 0)]


def baseline_partition(analysis, inventory):
    lead = max(inventory, key=lambda b: (b["spec"]["ram_kb"], b["spec"]["cores"]))
    assignments = [{"task": t["name"], "boards": eligible_boards(t, inventory)}
                   for t in analysis["tasks"]]
    plan = {
        "lead": lead["id"],
        "assignments": assignments,
        "rationale": "Deterministic baseline: each task is resident on every board that "
                     "meets its needs; the board with the most RAM leads.",
        "source": "baseline",
    }
    if analysis.get("modules"):
        mods = []
        for m in analysis["modules"]:
            boards = eligible_boards(m, inventory, module=True)
            mods.append({"module": m["name"], "boards": boards,
                         "count": max(1, min(m["count"], len(boards)))})
        plan["modules"] = mods
        plan["rationale"] += (" Each module may run on every board that meets its needs; "
                              "the lead picks the boards at run time.")
    return plan
