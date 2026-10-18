"""Deterministic partitioner: decide which boards hold which task images, and
who leads. No model involved.

Plans can also come from an assistant through the MCP server (mcp_server.py)
or from a hand-written plan.json. Every plan, whatever its source, has this
shape and goes through validate() before anything is generated:

  {"lead": board_id,
   "assignments": [{"task": name, "boards": [board_id, ...]}, ...],
   "rationale": "...", "source": "baseline" | "mcp" | "file:<path>"}

The baseline is also the yardstick: a proposed plan should beat it (fewer
single-board tasks, a better lead) or the baseline is the better choice.
"""


def eligible_boards(task, inventory):
    """Boards with every capability the task needs and enough RAM."""
    return [b["id"] for b in inventory
            if set(task["needs"]) <= set(b["spec"]["flags"])
            and task["ram_kb"] <= b["spec"]["ram_kb"]]


def baseline_partition(analysis, inventory):
    lead = max(inventory, key=lambda b: (b["spec"]["ram_kb"], b["spec"]["cores"]))
    assignments = [{"task": t["name"], "boards": eligible_boards(t, inventory)}
                   for t in analysis["tasks"]]
    return {
        "lead": lead["id"],
        "assignments": assignments,
        "rationale": "Deterministic baseline: each task is resident on every board that "
                     "meets its needs; the board with the most RAM leads.",
        "source": "baseline",
    }
