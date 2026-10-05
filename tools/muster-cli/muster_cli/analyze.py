"""Static analysis of the user's single main program.

Prototype implementation: a brace-aware scanner, good enough for annotated
Arduino-style sketches. The production version swaps this for libclang, which
resolves macros, aliasing and includes properly. The output contract (the
dicts returned below) stays the same, so nothing downstream changes.

What it extracts is FACT, not judgement:
  - every function marked MUSTER_TASK(...) and its declared needs
  - whether a task takes pointer/reference parameters (unsafe to split)
  - which file-level globals each task reads or writes (unsafe to split)
"""
import re

TASK_RE = re.compile(
    r"MUSTER_TASK\s*\(([^)]*)\)\s*"            # annotation arguments
    r"([\w:<>\s\*&]+?)\s+(\w+)\s*\(([^)]*)\)\s*\{"  # return type, name, params
)
IDENT_RE = re.compile(r"\b[A-Za-z_]\w*\b")
KEYWORDS = {
    "if", "else", "for", "while", "return", "switch", "case", "break",
    "continue", "sizeof", "do", "struct", "const", "static", "volatile",
}


def _strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def _match_brace(src, open_idx):
    depth = 0
    for i in range(open_idx, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unbalanced braces in source")


def _top_level_globals(src):
    """Variable declarations at brace depth 0 that are not functions."""
    names, depth, line = set(), 0, ""
    for ch in src:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            line = ""
            continue
        if depth != 0:
            continue
        if ch in ";\n":
            stmt = line.strip()
            line = ""
            if (not stmt or stmt.startswith("#") or "(" in stmt
                    or stmt.startswith(("typedef", "using", "struct", "MUSTER"))):
                continue
            stmt = stmt.split("=")[0].split("[")[0].strip()
            parts = stmt.replace("*", " ").split()
            if len(parts) >= 2:
                names.add(parts[-1])
        else:
            line += ch
    return names


def _parse_args(arg_text):
    out = {"needs": [], "cost": 100, "ram_kb": 1}
    for part in [p.strip() for p in arg_text.split(",") if p.strip()]:
        if "=" not in part:
            continue
        key, val = [s.strip() for s in part.split("=", 1)]
        if key == "needs":
            out["needs"] = [v.strip().lower() for v in val.split("|") if v.strip()]
        elif key == "cost":
            out["cost"] = int(val)
        elif key == "ram":
            out["ram_kb"] = int(val)
    return out


def analyze(source_text):
    src = _strip_comments(source_text)
    globals_ = _top_level_globals(src)
    tasks = []
    for m in TASK_RE.finditer(src):
        args, ret, name, params = m.groups()
        body_open = m.end() - 1
        body = src[body_open:_match_brace(src, body_open) + 1]
        used = {t for t in IDENT_RE.findall(body) if t not in KEYWORDS}
        tasks.append({
            "name": name,
            "return_type": ret.strip(),
            "params": params.strip(),
            "pointer_params": ("*" in params) or ("&" in params),
            "globals_touched": sorted(used & globals_),
            **_parse_args(args),
        })
    return {"tasks": tasks, "globals": sorted(globals_)}
