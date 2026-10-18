"""Generates muster_gen.h: the typed glue between the user's functions and the
runtime. Fully deterministic; nothing here comes from a model.

The header is included into the user's sketch right after the last task (so
it sees the user's structs and functions) and provides:

  mg_put_T / mg_get_T     encoders for each struct a task exchanges
  muster_tr_<task>        trampolines: bytes in -> typed call -> bytes out
  muster_setup()          bring up the CAN link and the runtime, register the
                          tasks resident on this board (call from setup())
  muster_loop()           muster_poll() (call from loop())
  muster_<task>(...)      run <task> wherever the lead places it (blocking)
  muster_<task>_async / muster_<task>_result   the non-blocking pair

Per-board facts (node ID, lead flag, resident mask, link pins, capabilities)
arrive as -D flags from platformio.ini, so one header serves every board.
The link is TWAI or MCP2515 (from the board DB), or MUSTER_GEN_LINK_EXTERNAL:
the application defines `muster_link *muster_gen_external_link(void)`.
"""
from . import wiretypes as wt

# Names the generated stubs would clash with (muster_<task>).
RESERVED = {
    "begin", "add_link", "register_task", "set_cost", "on_event", "poll", "call",
    "call_async", "result", "cancel", "alive_count", "node_by_id", "crc16", "w_init",
    "r_init", "setup", "loop", "rt", "can", "link",
} | {f"{d}_{s}" for d in ("put", "get") for s in ("u8", "u16", "u32", "i8", "i16", "i32", "f32")}


def task_id_macro(name):
    return f"MUSTER_TASK_ID_{name.upper()}"


def _params_decl(task):
    return ", ".join(f"{wt.spell(p['type'])} {p['name']}" for p in task["param_list"])


def _put_args(task, w):
    lines = []
    for p in task["param_list"]:
        lines += wt.put_lines(p["type"], p["name"], w)
    return lines


def _indent(lines, n=1):
    return ["    " * n + line for line in lines]


def _trampoline(t):
    name, ret = t["name"], t["returns"]
    out = [f"static inline int16_t muster_tr_{name}(const uint8_t *mg_a, uint16_t mg_len, "
           f"uint8_t *mg_ret, uint16_t mg_cap) {{",
           "    muster_rbuf mg_r; muster_wbuf mg_w;",
           "    muster_r_init(&mg_r, mg_a, mg_len);"]
    for p in t["param_list"]:
        out.append(f"    {wt.spell(p['type'])} {p['name']}{{}};")
        out += _indent(wt.get_lines(p["type"], p["name"], "&mg_r"))
    out.append("    if (mg_r.err || mg_r.n != mg_len) return -1;   /* wrong size: not our caller */")
    call = f"{name}({', '.join(p['name'] for p in t['param_list'])})"
    out.append("    muster_w_init(&mg_w, mg_ret, mg_cap);")
    if ret["kind"] == "void":
        out.append(f"    {call};")
    else:
        out.append(f"    {wt.spell(ret)} mg_out = {call};")
        out += _indent(wt.put_lines(ret, "mg_out", "&mg_w"))
    out += ["    return mg_w.err ? -1 : (int16_t)mg_w.n;", "}"]
    return out


def _stubs(t):
    name, ret = t["name"], t["returns"]
    tid = task_id_macro(name)
    abuf, rbuf = max(1, t["args_bytes"]), max(1, t["ret_bytes"])
    has_ret = ret["kind"] != "void"
    params = _params_decl(t)
    sep = ", " if params else ""
    out_param = f"{sep}{wt.spell(ret)} *mg_out" if has_ret else ""
    decode = ([f"    {wt.spell(ret)} mg_v{{}};",
               "    muster_rbuf mg_r; muster_r_init(&mg_r, mg_rb, mg_n);"]
              + _indent(wt.get_lines(ret, "mg_v", "&mg_r"))
              + ["    if (mg_r.err) return MUSTER_ERR;", "    if (mg_out) *mg_out = mg_v;"]) if has_ret else []

    blocking = [f"/* {name}({', '.join(p['name'] for p in t['param_list'])}) on whichever "
                f"board the lead picks. Blocks up to timeout_ms;",
                "   never call it from inside a task (use the _async pair there). */",
                f"static inline int8_t muster_{name}({params}{out_param}, "
                f"uint32_t mg_timeout_ms = 1000) {{" if (params or has_ret) else
                f"static inline int8_t muster_{name}(uint32_t mg_timeout_ms = 1000) {{",
                f"    uint8_t mg_ab[{abuf}], mg_rb[{rbuf}]; uint16_t mg_n = 0; int8_t mg_st;",
                "    muster_wbuf mg_w; muster_w_init(&mg_w, mg_ab, sizeof mg_ab);"]
    blocking += _indent(_put_args(t, "&mg_w"))
    blocking += [f"    mg_st = muster_call(&muster_rt, {tid}, mg_ab, mg_w.n, mg_rb, sizeof mg_rb, "
                 f"&mg_n, mg_timeout_ms);",
                 "    if (mg_st != MUSTER_OK) return mg_st;"]
    blocking += decode + ["    return MUSTER_OK;", "}"]

    async_ = [f"/* Start {name}; returns a handle >= 0 or a negative MUSTER_E* code. */",
              f"static inline int8_t muster_{name}_async({params}) {{",
              f"    uint8_t mg_ab[{abuf}]; muster_wbuf mg_w; muster_w_init(&mg_w, mg_ab, sizeof mg_ab);"]
    async_ += _indent(_put_args(t, "&mg_w"))
    async_ += [f"    return muster_call_async(&muster_rt, {tid}, mg_ab, mg_w.n);", "}"]

    res_params = f"int8_t mg_h, {wt.spell(ret)} *mg_out" if has_ret else "int8_t mg_h"
    result = [f"/* MUSTER_PENDING until {name} finishes, then MUSTER_OK or an error. */",
              f"static inline int8_t muster_{name}_result({res_params}) {{",
              f"    uint8_t mg_rb[{rbuf}]; uint16_t mg_n = 0;",
              "    int8_t mg_st = muster_result(&muster_rt, mg_h, mg_rb, sizeof mg_rb, &mg_n);",
              "    if (mg_st != MUSTER_OK) return mg_st;"]
    result += decode + ["    return MUSTER_OK;", "}"]
    return blocking + [""] + async_ + [""] + result


def _setup(analysis):
    out = [
        "static muster_t    muster_rt;",
        "static muster_link muster_can;",
        "#if defined(MUSTER_GEN_LINK_MCP2515)",
        "static MCP2515        muster_mcp(MUSTER_GEN_MCP_CS);",
        "static muster_mcp_ctx muster_mcp_state;",
        "#endif",
        "#if defined(MUSTER_GEN_LINK_EXTERNAL)",
        "/* You provide the link (SPI only, a custom transport, a test bus). */",
        "muster_link *muster_gen_external_link(void);",
        "#endif",
        "static uint32_t muster_millis_(void) { return millis(); }",
        "",
        "/* Call from setup(). Returns 0, or -1 if the link did not start. */",
        "static inline int muster_setup(void) {",
        "    int rc;",
        "    muster_link *mg_link = &muster_can;",
        "#if defined(MUSTER_GEN_LINK_TWAI)",
        "    rc = muster_twai_begin(&muster_can, MUSTER_NODE_ID, MUSTER_GEN_CAN_TX, MUSTER_GEN_CAN_RX);",
        "#elif defined(MUSTER_GEN_LINK_MCP2515)",
        "    rc = muster_mcp2515_begin(&muster_can, &muster_mcp_state, &muster_mcp, MUSTER_NODE_ID);",
        "#elif defined(MUSTER_GEN_LINK_EXTERNAL)",
        "    mg_link = muster_gen_external_link();",
        "    rc = mg_link ? 0 : -1;",
        "#else",
        '#error "muster_gen.h: build flags select no link (MUSTER_GEN_LINK_*)"',
        "#endif",
        "    muster_begin(&muster_rt, MUSTER_NODE_ID, MUSTER_LEAD, MUSTER_GEN_CORES,",
        "                 MUSTER_GEN_RAM_KB, MUSTER_GEN_CLOCK_MHZ, MUSTER_GEN_FLAGS,",
        "                 mg_link, muster_millis_);",
        "    /* every board knows every task's cost: the lead needs it to place",
        "       tasks it does not hold itself */",
    ]
    for t in analysis["tasks"]:
        out.append(f"    muster_set_cost(&muster_rt, {task_id_macro(t['name'])}, {t['cost']}u);")
    for t in analysis["tasks"]:
        tid = task_id_macro(t["name"])
        out += [f"#if (MUSTER_RESIDENT_MASK & (1ul << {tid}))",
                f"    muster_register_task(&muster_rt, {tid}, muster_tr_{t['name']}, {t['cost']}u);",
                "#endif"]
    out += ["    return rc;", "}", "",
            "/* Call from loop(), as often as possible. */",
            "static inline void muster_loop(void) { muster_poll(&muster_rt); }"]
    return out


def generate_header(analysis):
    types = analysis["types"]
    structs = []
    for t in analysis["tasks"]:
        for d in [p["type"] for p in t["param_list"]] + [t["returns"]]:
            for s in wt.structs_used(d, types):
                if s not in structs:
                    structs.append(s)

    out = ["/* GENERATED by muster - do not edit. Re-run `muster plan`. */",
           "#pragma once",
           "#include <Arduino.h>",
           '#include "muster.h"',
           '#include "muster_plan.h"',
           "#if defined(MUSTER_GEN_LINK_TWAI)",
           '#include "muster_link_twai.h"',
           "#elif defined(MUSTER_GEN_LINK_MCP2515)",
           '#include "muster_link_mcp2515.h"',
           "#endif",
           "#ifndef MUSTER_LEAD",
           "#define MUSTER_LEAD 0",
           "#endif",
           "",
           "/* ---- wire encoding of the structs tasks exchange (little-endian, fixed width) */"]
    for s in structs:
        out += wt.struct_codec(s, types) + [""]
    out.append("/* ---- trampolines: bytes in -> your function -> bytes out */")
    for t in analysis["tasks"]:
        out += _trampoline(t) + [""]
    out.append("/* ---- the runtime on this board */")
    out += _setup(analysis) + [""]
    out.append("/* ---- call any task wherever the lead places it */")
    for t in analysis["tasks"]:
        out += _stubs(t) + [""]
    return "\n".join(out) + "\n"


def generate_plan_header(analysis):
    lines = ["#pragma once", "/* GENERATED by muster - do not edit. */", ""]
    for i, t in enumerate(analysis["tasks"]):
        lines.append(f"#define {task_id_macro(t['name'])} {i}u")
    lines.append(f"#define MUSTER_TASK_COUNT {len(analysis['tasks'])}u")
    return "\n".join(lines) + "\n"
