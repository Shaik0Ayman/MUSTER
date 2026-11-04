# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
"""Types that may cross a board boundary, and how they are encoded.

A type descriptor is a small dict, produced by the analyser and consumed by
the validator and the code generator:

  {"kind": "scalar", "name": "float"}
  {"kind": "array",  "of": <descriptor>, "count": 4}
  {"kind": "struct", "name": "Vec3"}      # fields in analysis["types"]["Vec3"]
  {"kind": "void"}                        # return type only

Every scalar maps onto one muster_put_* / muster_get_* pair, so the bytes on
the wire are fixed-width little-endian on every board (firmware hard rule 3).
"""

# C name -> (muster.h codec suffix, encoded bytes)
SCALARS = {
    "uint8_t": ("u8", 1),
    "int8_t": ("i8", 1),
    "uint16_t": ("u16", 2),
    "int16_t": ("i16", 2),
    "uint32_t": ("u32", 4),
    "int32_t": ("i32", 4),
    "float": ("f32", 4),
    "bool": ("u8", 1),
}

# Types the user may write that must NOT cross boards, and what to use instead.
REPLACEMENTS = {
    "int": "int16_t or int32_t (int is 16-bit on AVR, 32-bit on ESP32)",
    "unsigned int": "uint16_t or uint32_t (unsigned is 16-bit on AVR, 32-bit on ESP32)",
    "short": "int16_t",
    "unsigned short": "uint16_t",
    "long": "int32_t (long is 32-bit on AVR, 64-bit on some hosts)",
    "unsigned long": "uint32_t",
    "long long": "int32_t, or split the value: 64-bit integers are not supported on the wire",
    "unsigned long long": "uint32_t, or split the value: 64-bit integers are not supported on the wire",
    "char": "uint8_t or int8_t (char signedness differs between compilers)",
    "signed char": "int8_t",
    "unsigned char": "uint8_t",
    "double": "float (double is 32-bit on AVR, 64-bit on ESP32)",
    "long double": "float",
    "size_t": "uint16_t or uint32_t (size_t is 16-bit on AVR, 32-bit on ESP32)",
    "wchar_t": "uint16_t",
}


def ident(name):
    """A C identifier for a (possibly namespace-qualified) struct name."""
    return name.replace("::", "_")


def scalar(name):
    return {"kind": "scalar", "name": name}


def encoded_size(desc, types):
    """Bytes `desc` occupies on the wire."""
    k = desc["kind"]
    if k == "void":
        return 0
    if k == "scalar":
        return SCALARS[desc["name"]][1]
    if k == "array":
        return desc["count"] * encoded_size(desc["of"], types)
    if k == "struct":
        return sum(encoded_size(f["type"], types) for f in types[desc["name"]]["fields"])
    raise ValueError(f"unknown descriptor kind {k!r}")


def spell(desc):
    """C++ spelling of a descriptor (arrays only appear as struct fields)."""
    k = desc["kind"]
    if k == "void":
        return "void"
    if k in ("scalar", "struct"):
        return desc["name"]
    return f"{spell(desc['of'])}[{desc['count']}]"


def structs_used(desc, types, out=None):
    """Struct names reachable from `desc`, dependencies first."""
    out = [] if out is None else out
    k = desc["kind"]
    if k == "array":
        structs_used(desc["of"], types, out)
    elif k == "struct" and desc["name"] not in out:
        for f in types[desc["name"]]["fields"]:
            structs_used(f["type"], types, out)
        out.append(desc["name"])
    return out


# ------------------------------------------------------------- codegen
def _put(desc, expr, w, depth):
    k = desc["kind"]
    if k == "scalar":
        codec = SCALARS[desc["name"]][0]
        if desc["name"] == "bool":
            return [f"muster_put_u8({w}, (uint8_t)({expr} ? 1 : 0));"]
        return [f"muster_put_{codec}({w}, {expr});"]
    if k == "struct":
        return [f"mg_put_{ident(desc['name'])}({w}, {expr});"]
    i = f"mg_i{depth}"
    body = _put(desc["of"], f"{expr}[{i}]", w, depth + 1)
    return [f"for (uint16_t {i} = 0; {i} < {desc['count']}u; {i}++) {{"] + \
           ["    " + line for line in body] + ["}"]


def _get(desc, lvalue, r, depth):
    k = desc["kind"]
    if k == "scalar":
        codec = SCALARS[desc["name"]][0]
        if desc["name"] == "bool":
            return [f"{lvalue} = muster_get_u8({r}) != 0;"]
        return [f"{lvalue} = muster_get_{codec}({r});"]
    if k == "struct":
        return [f"mg_get_{ident(desc['name'])}({r}, {lvalue});"]
    i = f"mg_i{depth}"
    body = _get(desc["of"], f"{lvalue}[{i}]", r, depth + 1)
    return [f"for (uint16_t {i} = 0; {i} < {desc['count']}u; {i}++) {{"] + \
           ["    " + line for line in body] + ["}"]


def put_lines(desc, expr, w="w"):
    """C++ statements that encode `expr` into the muster_wbuf pointer `w`."""
    return _put(desc, expr, w, 0)


def get_lines(desc, lvalue, r="r"):
    """C++ statements that decode into `lvalue` from the muster_rbuf pointer `r`."""
    return _get(desc, lvalue, r, 0)


def struct_codec(name, types):
    """mg_put_<T> / mg_get_<T> helpers for one struct (its fields' structs first)."""
    put = [f"static inline void mg_put_{ident(name)}(muster_wbuf *w, const {name} &v) {{"]
    get = [f"static inline void mg_get_{ident(name)}(muster_rbuf *r, {name} &v) {{"]
    for f in types[name]["fields"]:
        put += ["    " + line for line in put_lines(f["type"], f"v.{f['name']}")]
        get += ["    " + line for line in get_lines(f["type"], f"v.{f['name']}")]
    return put + ["}"] + get + ["}"]
