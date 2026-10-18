"""The validator has the final word. Whoever proposes a plan - the baseline,
an assistant over MCP, a hand-written file - this disposes.

Errors block generation. Warnings ship with the plan and show up in the
terminal report, the MCP result and the wiring notes.
"""
from .codegen import RESERVED


def _plan_shape(plan, errors):
    """Reject malformed plans before looking at their contents."""
    if not isinstance(plan, dict):
        errors.append("plan must be a JSON object with 'lead', 'assignments', 'rationale'")
        return False
    if not isinstance(plan.get("lead"), str):
        errors.append("plan 'lead' must be a board ID (string)")
    items = plan.get("assignments")
    if not isinstance(items, list):
        errors.append("plan 'assignments' must be a list of {task, boards}")
        return False
    for i, item in enumerate(items):
        if (not isinstance(item, dict) or not isinstance(item.get("task"), str)
                or not isinstance(item.get("boards"), list)
                or not all(isinstance(b, str) for b in item["boards"])):
            errors.append(f'assignment #{i} must be {{"task": str, "boards": [str, ...]}}')
            return False
    return True


def _source_checks(analysis, errors, warnings):
    """Problems in the program itself; no placement can fix these."""
    if not analysis["tasks"]:
        errors.append("no MUSTER_TASK functions found in the source")
    for t in analysis["tasks"]:
        errors += t.get("errors", [])
        warnings += t.get("warnings", [])
        if t["name"] in RESERVED or t["name"].startswith("tr_"):
            errors.append(f"task '{t['name']}' clashes with the generated muster_{t['name']}(); "
                          f"rename it")
        for p in t.get("param_list", []):
            if p["name"].startswith("mg_"):
                errors.append(f"task '{t['name']}' parameter '{p['name']}': the mg_ prefix is "
                              f"reserved for generated code; rename it")
        if t["globals_touched"]:
            errors.append(f"task '{t['name']}' touches global(s) "
                          f"{', '.join(t['globals_touched'])}; there is no shared memory "
                          f"across boards. Pass them as arguments or return values "
                          f"(const globals are fine).")
    for d in analysis.get("diagnostics", []):
        warnings.append(f"compiler: {d}")


def _runtime_checks(analysis, boards, errors):
    """Every call must fit the smallest board's message buffer."""
    rt = {bid: b["spec"]["runtime"] for bid, b in boards.items()}
    if not rt:
        return rt
    small = min(rt, key=lambda bid: rt[bid]["max_msg"])
    max_args, max_ret = rt[small]["max_msg"] - 6, rt[small]["max_msg"] - 5
    for t in analysis["tasks"]:
        if t.get("args_bytes") is not None and t["args_bytes"] > max_args:
            errors.append(f"task '{t['name']}' arguments encode to {t['args_bytes']} bytes; "
                          f"'{small}' carries at most {max_args} (MUSTER_MAX_MSG - 6)")
        if t.get("ret_bytes") is not None and t["ret_bytes"] > max_ret:
            errors.append(f"task '{t['name']}' result encodes to {t['ret_bytes']} bytes; "
                          f"'{small}' carries at most {max_ret} (MUSTER_MAX_MSG - 5)")
    max_tasks = min(r["max_tasks"] for r in rt.values())
    if len(analysis["tasks"]) > max_tasks:
        errors.append(f"{len(analysis['tasks'])} tasks, but MUSTER_MAX_TASKS is {max_tasks}")
    return rt


def validate(plan, analysis, inventory):
    errors, warnings = [], []
    boards = {b["id"]: b for b in inventory}
    tasks = {t["name"]: t for t in analysis["tasks"]}
    if len(boards) != len(inventory):
        errors.append("board IDs in the inventory must be unique")

    _source_checks(analysis, errors, warnings)
    rt = _runtime_checks(analysis, boards, errors)
    if not _plan_shape(plan, errors):
        return errors, warnings

    # ---- plan structure ----------------------------------------------------
    lead = plan["lead"]
    if lead not in boards:
        errors.append(f"lead board '{lead}' is not in the inventory "
                      f"(boards: {', '.join(boards)})")
    elif len(boards) > rt[lead]["max_nodes"]:
        errors.append(f"lead '{lead}' tracks at most {rt[lead]['max_nodes']} boards "
                      f"(MUSTER_MAX_NODES) but the inventory has {len(boards)}")

    seen = set()
    for a in plan["assignments"]:
        name, held = a["task"], a["boards"]
        if name not in tasks:
            errors.append(f"plan mentions unknown task '{name}' (tasks: {', '.join(tasks)})")
            continue
        if name in seen:
            errors.append(f"task '{name}' appears twice in the plan")
        seen.add(name)
        if not held:
            errors.append(f"task '{name}' is not resident on any board")
        if len(set(held)) != len(held):
            errors.append(f"task '{name}' lists a board twice")
        for bid in held:
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
        if len(set(held)) == 1:
            warnings.append(f"task '{name}' lives on one board only; it cannot be "
                            f"re-placed if that board fails")

    for name in tasks:
        if name not in seen:
            errors.append(f"task '{name}' is missing from the plan")

    # ---- per-board RAM budget across every resident task -------------------
    for bid, b in boards.items():
        total = sum(tasks[a["task"]]["ram_kb"] for a in plan["assignments"]
                    if a["task"] in tasks and bid in a["boards"])
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
