# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
"""Static analysis of the user's single main program, with libclang.

What it extracts is FACT, not judgement (the validator judges):
  - every function marked MUSTER_TASK(...), its declared needs/cost/ram
  - each parameter and the return value as a wire-type descriptor
    (wiretypes.py), or the reason it cannot cross a board boundary
  - which file-level variables each task touches (no shared memory across
    boards), and static locals (state that differs per board)

The sketch is parsed as C++11 without any board SDK: muster_cli/shim holds
stand-ins for the Arduino core and the C library, and any other header the
sketch includes is replaced by an empty stub so a missing library never stops
the parse. Only task signatures need real types, and those must be defined in
the sketch itself.

Output contract (consumed by validate, codegen, pio, the CLI and the MCP
server). The keys from the first, regex-based analyser are kept unchanged:
  tasks[]: name, return_type, params, pointer_params, globals_touched,
           needs, cost, ram_kb
  globals: [names]
and extended with:
  tasks[]: line, end_line, param_list [{name, spelling, type}], returns,
           args_bytes, ret_bytes, errors [], warnings []
  types:   {struct name: {"fields": [{name, type}], "size": bytes}}
  diagnostics: [compiler errors outside what the analyser reports itself]
  modules[]: name, line, end_line, needs, ram_kb, count, errors, warnings
             (MUSTER_MODULE(...) functions: int8_t name(uint8_t event))
  local_headers: project headers the sketch includes (paths relative to it),
             whose tasks and modules count as part of the program
  insert_line: last line of the last task, module or local #include in the
             sketch (generated code goes after it)
  has_gen_include: the sketch already #includes "muster_gen.h"
"""
import re
import tempfile
from pathlib import Path

import clang.cindex as ci

from . import wiretypes as wt

SHIM = Path(__file__).parent / "shim"
KNOWN_NEEDS = {"fpu", "wifi", "ble"}
CK, TK = ci.CursorKind, ci.TypeKind

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
BUILTIN_NAMES = {
    TK.INT: "int", TK.UINT: "unsigned int", TK.SHORT: "short", TK.USHORT: "unsigned short",
    TK.LONG: "long", TK.ULONG: "unsigned long", TK.LONGLONG: "long long",
    TK.ULONGLONG: "unsigned long long", TK.CHAR_S: "char", TK.CHAR_U: "char",
    TK.SCHAR: "signed char", TK.UCHAR: "unsigned char", TK.DOUBLE: "double",
    TK.LONGDOUBLE: "long double", TK.WCHAR: "wchar_t",
}


class AnalysisError(Exception):
    """The source could not be parsed at all."""


# ------------------------------------------------------------- parsing
def _stub_missing_headers(text, search_dirs, stub_dir):
    """Write an empty header for every include we cannot find, so clang does
    not stop at 'file not found'. Returns the names that were stubbed."""
    stubbed = []
    for name in INCLUDE_RE.findall(text):
        if any((Path(d) / name).is_file() for d in search_dirs):
            continue
        target = Path(stub_dir) / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("#pragma once\n")
        stubbed.append(name)
    return stubbed


def _parse(text, filename, include_dirs):
    tmp = tempfile.TemporaryDirectory()
    stub_dir = Path(tmp.name)
    src_dir = Path(filename).resolve().parent
    search = [SHIM, src_dir, *include_dirs]
    stubbed = _stub_missing_headers(text, search, stub_dir)
    args = ["-x", "c++", "-std=c++11", "--target=x86_64-unknown-linux-gnu",
            "-nostdinc", "-nostdinc++", "-isystem", str(SHIM), "-isystem", str(stub_dir),
            "-I", str(src_dir), "-include", str(SHIM / "muster_analysis.h")]
    for d in include_dirs:
        args += ["-I", str(d)]
    path = str(Path(filename).resolve())
    try:
        tu = ci.Index.create().parse(path, args=args, unsaved_files=[(path, text)],
                                     options=ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
    except ci.TranslationUnitLoadError as e:
        tmp.cleanup()
        raise AnalysisError(f"libclang could not parse {filename}: {e}")
    return tu, tmp, stubbed, path


def _under(p, d):
    try:
        Path(p).resolve().relative_to(Path(d).resolve())
        return True
    except ValueError:
        return False


def _in_project(cursor, path, stub_dir):
    """In the sketch or in a header next to it (not a stub or a shim)."""
    f = cursor.location.file
    return (f is not None and _under(f.name, Path(path).parent)
            and not _under(f.name, stub_dir))


# ------------------------------------------------------------- types
def _is_const(t):
    if t.kind in (TK.CONSTANTARRAY, TK.INCOMPLETEARRAY):
        return _is_const(t.element_type)
    return t.is_const_qualified()


class _Resolver:
    """Turns clang types into wire descriptors, collecting struct definitions."""

    def __init__(self):
        self.types = {}

    def resolve(self, t, where, errors, allow_array=False):
        k = t.kind
        if k == TK.ELABORATED:
            return self.resolve(t.get_named_type(), where, errors, allow_array)
        if k in (TK.POINTER, TK.LVALUEREFERENCE, TK.RVALUEREFERENCE, TK.MEMBERPOINTER,
                 TK.INCOMPLETEARRAY, TK.VARIABLEARRAY, TK.DEPENDENTSIZEDARRAY):
            what = "a reference" if "REFERENCE" in k.name else "a pointer"
            errors.append(f"{where} is {what} ({t.spelling}); the other board cannot see "
                          f"this board's memory. Pass the value itself.")
            return None
        if k == TK.TYPEDEF:
            name = t.get_declaration().spelling
            if name in wt.SCALARS:
                return wt.scalar(name)
            if name in wt.REPLACEMENTS:
                errors.append(f"{where} is {name}; use {wt.REPLACEMENTS[name]}.")
                return None
            return self.resolve(t.get_declaration().underlying_typedef_type, where, errors,
                                allow_array)
        if k == TK.BOOL:
            return wt.scalar("bool")
        if k == TK.FLOAT:
            return wt.scalar("float")
        if k in BUILTIN_NAMES:
            name = BUILTIN_NAMES[k]
            errors.append(f"{where} is {name}; use {wt.REPLACEMENTS[name]}.")
            return None
        if k == TK.ENUM:
            errors.append(f"{where} is an enum ({t.spelling}); its size differs between "
                          f"compilers. Use uint8_t and named constants.")
            return None
        if k == TK.CONSTANTARRAY:
            if not allow_array:
                errors.append(f"{where} is an array ({t.spelling}); C passes arrays as "
                              f"pointers. Wrap it in a struct.")
                return None
            of = self.resolve(t.element_type, where + "[]", errors, allow_array=True)
            return {"kind": "array", "of": of, "count": t.element_count} if of else None
        if k == TK.RECORD:
            return self._record(t, where, errors)
        if k == TK.VOID:
            errors.append(f"{where} cannot be void")
            return None
        errors.append(f"{where} has type '{t.spelling}', which muster cannot resolve "
                      f"(define it in this file, or check its header is found).")
        return None

    def _record(self, t, where, errors):
        decl = t.get_declaration()
        name = t.get_canonical().spelling.replace("struct ", "").replace("class ", "")
        if decl.kind == CK.UNION_DECL:
            errors.append(f"{where} is a union ({name}); use a struct.")
            return None
        if not re.fullmatch(r"[A-Za-z_]\w*(::[A-Za-z_]\w*)*", name):
            errors.append(f"{where} is an unnamed struct; give it a name.")
            return None
        if name in self.types:
            return {"kind": "struct", "name": name}
        fields, ok = [], True
        for c in decl.get_children():
            if c.kind == CK.CXX_BASE_SPECIFIER:
                errors.append(f"struct {name} has a base class; muster sends plain structs only.")
                ok = False
            elif c.kind == CK.CXX_METHOD and c.is_virtual_method():
                errors.append(f"struct {name} has virtual methods; muster sends plain structs only.")
                ok = False
            elif c.kind == CK.FIELD_DECL:
                if c.is_bitfield():
                    errors.append(f"field {name}.{c.spelling} is a bit-field; use a whole uint8_t/uint16_t.")
                    ok = False
                    continue
                d = self.resolve(c.type, f"field {name}.{c.spelling}", errors, allow_array=True)
                if d is None:
                    ok = False
                else:
                    fields.append({"name": c.spelling, "type": d})
        if not ok:
            return None
        self.types[name] = {"fields": fields}
        self.types[name]["size"] = wt.encoded_size({"kind": "struct", "name": name}, self.types)
        return {"kind": "struct", "name": name}

    def resolve_return(self, t, where, errors):
        if t.kind == TK.VOID:
            return {"kind": "void"}
        return self.resolve(t, where, errors)


# ------------------------------------------------------------- tasks
def _parse_annotation(text, name, errors, kind="task"):
    out = {"needs": [], "ram_kb": 1}
    out.update({"cost": 100} if kind == "task" else {"count": 1})
    keys = ("needs", "cost", "ram") if kind == "task" else ("needs", "ram", "count")
    for part in [p.strip() for p in text.split(",") if p.strip()]:
        if "=" not in part:
            errors.append(f"{kind} '{name}': annotation item '{part}' is not key = value")
            continue
        key, val = [s.strip() for s in part.split("=", 1)]
        if key == "needs":
            out["needs"] = [v.strip().lower() for v in val.split("|") if v.strip()]
            unknown = set(out["needs"]) - KNOWN_NEEDS
            if unknown:
                errors.append(f"{kind} '{name}': unknown need(s) {', '.join(sorted(unknown))}; "
                              f"known: {', '.join(sorted(KNOWN_NEEDS))}")
        elif key in keys:
            try:
                v = int(val, 0)
            except ValueError:
                errors.append(f"{kind} '{name}': {key} = {val} is not an integer")
                continue
            if key == "count" and not 1 <= v <= 8:
                errors.append(f"{kind} '{name}': count must be 1..8 (boards that run it at once)")
            elif key == "cost" and not 1 <= v <= 65535:
                errors.append(f"{kind} '{name}': cost must be 1..65535 (it is a uint16_t)")
            elif key == "ram" and v < 0:
                errors.append(f"{kind} '{name}': ram must be >= 0")
            out[{"ram": "ram_kb"}.get(key, key)] = v
        else:
            errors.append(f"{kind} '{name}': unknown annotation key '{key}' "
                          f"(known: {', '.join(keys)})")
    return out


def _body_facts(fn):
    """File-level variables the body touches, and static locals it declares."""
    touched, statics = set(), set()

    def walk(c):
        if c.kind == CK.DECL_REF_EXPR:
            ref = c.referenced
            if (ref is not None and ref.kind == CK.VAR_DECL
                    and ref.semantic_parent is not None
                    and ref.semantic_parent.kind in (CK.TRANSLATION_UNIT, CK.NAMESPACE)
                    and not _is_const(ref.type)):
                touched.add(ref.spelling)
        elif c.kind == CK.VAR_DECL and c.storage_class == ci.StorageClass.STATIC:
            statics.add(c.spelling)
        for ch in c.get_children():
            walk(ch)

    for ch in fn.get_children():
        if ch.kind == CK.COMPOUND_STMT:
            walk(ch)
    return sorted(touched), sorted(statics)


def _annotation(fn):
    for c in fn.get_children():
        if c.kind == CK.ANNOTATE_ATTR and c.spelling.startswith("muster:"):
            return c.spelling[len("muster:"):]
    return None


def _module_annotation(fn):
    for c in fn.get_children():
        if c.kind == CK.ANNOTATE_ATTR and c.spelling.startswith("muster-module:"):
            return c.spelling[len("muster-module:"):]
    return None


def _walk_decls(cursor, path, stub_dir):
    for c in cursor.get_children():
        if not _in_project(c, path, stub_dir):
            continue
        if c.kind == CK.NAMESPACE:
            yield from _walk_decls(c, path, stub_dir)
        else:
            yield c


def analyze(source_text, filename="main.cpp", include_dirs=()):
    tu, tmp, stubbed, path = _parse(source_text, filename, include_dirs)
    try:
        return _analyze_tu(tu, path, stubbed, tmp.name)
    finally:
        tmp.cleanup()


def analyze_file(path, include_dirs=()):
    p = Path(path)
    return analyze(p.read_text(encoding="utf-8"), str(p), include_dirs)


def _module(c, ann, names):
    name = c.spelling
    errors, warnings = [], []
    if c.kind == CK.CXX_METHOD:
        errors.append(f"module '{name}' is a member function; modules must be free functions")
    if name in names:
        errors.append(f"module '{name}' clashes with another task or module of that name")
    names[name] = True
    args = list(c.get_arguments())
    if len(args) != 1 or args[0].type.spelling != "uint8_t" or c.result_type.spelling != "int8_t":
        errors.append(f"module '{name}' must be declared int8_t {name}(uint8_t event): it is "
                      f"called with MUSTER_MOD_START (return < 0 to decline), MUSTER_MOD_RUN "
                      f"(often) and MUSTER_MOD_STOP")
    return {"name": name, **_parse_annotation(ann, name, errors, "module"),
            "line": c.extent.start.line, "end_line": c.extent.end.line,
            "errors": errors, "warnings": warnings}


def _analyze_tu(tu, path, stubbed, stub_dir):
    res = _Resolver()
    tasks, modules, globals_, names = [], [], set(), {}
    insert_line, has_gen = 0, False
    local_headers, module_names = [], set()

    for inc in tu.get_includes():
        if inc.depth == 1 and Path(inc.include.name).name == "muster_gen.h":
            has_gen = True
        f = inc.include.name
        if Path(f).resolve() != Path(path).resolve() and _under(f, Path(path).parent) \
                and not _under(f, stub_dir):
            rel = Path(f).resolve().relative_to(Path(path).parent.resolve()).as_posix()
            if rel not in local_headers:
                local_headers.append(rel)
            if inc.depth == 1:
                insert_line = max(insert_line, inc.location.line)

    for c in _walk_decls(tu.cursor, path, stub_dir):
        if c.kind == CK.VAR_DECL and not _is_const(c.type):
            globals_.add(c.spelling)
        if c.kind not in (CK.FUNCTION_DECL, CK.CXX_METHOD):
            continue
        ann = _annotation(c) if c.is_definition() else None
        if ann is None:
            # A module may be declared early and defined after the generated
            # glue, where it can call the muster_<task>() stubs; count it once.
            mann = _module_annotation(c)
            if mann is not None and c.spelling not in module_names:
                module_names.add(c.spelling)
                modules.append(_module(c, mann, names))
                if _under(c.location.file.name, Path(path).parent) and \
                        Path(c.location.file.name).resolve() == Path(path).resolve():
                    insert_line = max(insert_line, c.extent.end.line)
            continue
        name = c.spelling
        errors, warnings = [], []
        if c.kind == CK.CXX_METHOD:
            errors.append(f"task '{name}' is a member function; tasks must be free functions")
        if name in names:
            errors.append(f"task '{name}' is defined twice (overloads cannot be tasks)")
        names[name] = True
        meta = _parse_annotation(ann, name, errors)

        params = []
        for i, a in enumerate(c.get_arguments()):
            pname = a.spelling or f"arg{i}"
            d = res.resolve(a.type, f"task '{name}' parameter '{pname}'", errors)
            params.append({"name": pname, "spelling": a.type.spelling, "type": d})
        ret = res.resolve_return(c.result_type, f"task '{name}' return value", errors)
        touched, statics = _body_facts(c)
        if statics:
            warnings.append(f"task '{name}' keeps static local(s) {', '.join(statics)}; each "
                            f"board has its own copy, so results depend on where it ran")
        typed = ret is not None and all(p["type"] for p in params)
        tasks.append({
            "name": name,
            "return_type": c.result_type.spelling,
            "params": ", ".join(f"{p['spelling']} {p['name']}" for p in params),
            "pointer_params": any(a.type.kind in (TK.POINTER, TK.LVALUEREFERENCE,
                                                  TK.RVALUEREFERENCE) for a in c.get_arguments()),
            "globals_touched": touched,
            **meta,
            "line": c.extent.start.line,
            "end_line": c.extent.end.line,
            "param_list": params,
            "returns": ret,
            "args_bytes": sum(wt.encoded_size(p["type"], res.types) for p in params) if typed else None,
            "ret_bytes": wt.encoded_size(ret, res.types) if typed else None,
            "errors": errors,
            "warnings": warnings,
        })
        if Path(c.location.file.name).resolve() == Path(path).resolve():
            insert_line = max(insert_line, c.extent.end.line)

    diagnostics = []
    for d in tu.diagnostics:
        if d.severity < ci.Diagnostic.Error:
            continue
        if re.search(r"undeclared identifier '(muster_|MUSTER_)", d.spelling):
            continue                        # generated call stubs and runtime constants, see codegen.py
        line = d.location.line if d.location.file else 0
        diagnostics.append(f"line {line}: {d.spelling}")

    return {
        "tasks": tasks,
        "modules": modules,
        "local_headers": local_headers,
        "globals": sorted(globals_),
        "types": res.types,
        "diagnostics": diagnostics,
        "stubbed_headers": stubbed,
        "insert_line": insert_line,
        "has_gen_include": has_gen,
    }
