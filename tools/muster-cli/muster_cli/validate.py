"""The validator has the final word. The LLM proposes; this disposes.

Errors block the build. Warnings ship with the plan and show up in the
wiring diagram and the terminal report.
"""


def validate(plan, analysis, inventory):
    errors, warnings = [], []
    boards = {b["id"]: b for b in inventory}
    tasks = {t["name"]: t for t in analysis["tasks"]}

    # ---- source-level safety: these cannot be fixed by any placement -------
    for t in analysis["tasks"]:
        if t["pointer_params"]:
            errors.append(f"task '{t['name']}' takes pointer/reference parameters "
                          f"({t['params']}); the other board cannot see the caller's memory. "
                          f"Pass values instead.")
        if t["globals_touched"]:
            errors.append(f"task '{t['name']}' touches global(s) "
                          f"{', '.join(t['globals_touched'])}; there is no shared memory "
                          f"across boards. Pass them as arguments or return values.")

    # ---- plan structure ----------------------------------------------------
    if plan.get("lead") not in boards:
        errors.append(f"lead board '{plan.get('lead')}' is not in the inventory")

    seen = set()
    for a in plan.get("assignments", []):
        name = a.get("task")
        if name not in tasks:
            errors.append(f"plan mentions unknown task '{name}'")
            continue
        seen.add(name)
        if not a.get("boards"):
            errors.append(f"task '{name}' is not resident on any board")
        for bid in a.get("boards", []):
            if bid not in boards:
                errors.append(f"task '{name}' assigned to unknown board '{bid}'")
                continue
            spec = boards[bid]["spec"]
            missing = set(tasks[name]["needs"]) - set(spec["flags"])
            if missing:
                errors.append(f"task '{name}' needs {', '.join(sorted(missing))} "
                              f"but '{bid}' does not have it")
            if tasks[name]["ram_kb"] > spec["ram_kb"]:
                errors.append(f"task '{name}' needs {tasks[name]['ram_kb']} KB RAM "
                              f"but '{bid}' has {spec['ram_kb']} KB")
        if len(a.get("boards", [])) == 1:
            warnings.append(f"task '{name}' lives on one board only; it cannot be "
                            f"re-placed if that board fails")

    for name in tasks:
        if name not in seen:
            errors.append(f"task '{name}' is missing from the plan")

    # ---- per-board RAM budget across every resident task -------------------
    for bid, b in boards.items():
        total = sum(tasks[a["task"]]["ram_kb"] for a in plan.get("assignments", [])
                    if a.get("task") in tasks and bid in a.get("boards", []))
        if total > b["spec"]["ram_kb"]:
            errors.append(f"board '{bid}' would need {total} KB RAM for its resident "
                          f"tasks but has {b['spec']['ram_kb']} KB")

    # ---- electrical rules --------------------------------------------------
    volts = {b["spec"]["logic_v"] for b in inventory}
    if len(volts) > 1:
        warnings.append("mixed logic levels in the system "
                        f"({', '.join(str(v) + ' V' for v in sorted(volts))}). CAN is safe because "
                        "each transceiver isolates logic levels, but any direct SPI accelerator "
                        "link between boards of different voltage NEEDS a level shifter.")

    return errors, warnings
