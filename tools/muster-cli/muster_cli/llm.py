"""Partitioning: decide which boards receive which task images, and who leads.

Two partitioners share one output contract:
  - llm_partition():       Qwen3.5-9B through Ollama, JSON-schema constrained
  - baseline_partition():  deterministic rules, no model at all

The baseline is not just a fallback. It is the yardstick: if the LLM cannot
beat it on validation-pass rate and plan quality, the LLM is not earning
its place in the pipeline.
"""
import json
import requests

DEFAULT_MODEL = "qwen3.5:9b"          # Ollama's default tag is Q4_K_M
OLLAMA_URL = "http://localhost:11434/api/chat"

PLAN_SCHEMA = {
    "type": "object",
    "properties": {
        "lead": {"type": "string"},
        "assignments": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "task": {"type": "string"},
                    "boards": {"type": "array", "items": {"type": "string"}},
                },
                "required": ["task", "boards"],
            },
        },
        "rationale": {"type": "string"},
    },
    "required": ["lead", "assignments", "rationale"],
}

SYSTEM_PROMPT = """You partition one embedded program across several microcontroller boards.
Rules you must follow:
- Every task must be resident on at least one board. Prefer two boards for resilience when they qualify.
- A board may only hold a task if it has every capability the task needs and enough RAM.
- The lead board coordinates everything; prefer the board with the most RAM and cores.
- Never place heavy-RAM tasks on boards with only a few KB of RAM.
- Use only the board IDs you are given. Never invent boards or tasks.
Return JSON matching the schema. Keep the rationale to two or three sentences."""


def _inventory_summary(inventory):
    return [
        {"id": b["id"], "type": b["type"], "cores": b["spec"]["cores"],
         "ram_kb": b["spec"]["ram_kb"], "clock_mhz": b["spec"]["clock_mhz"],
         "capabilities": b["spec"]["flags"]}
        for b in inventory
    ]


def _task_summary(analysis):
    return [
        {"task": t["name"], "needs": t["needs"], "ram_kb": t["ram_kb"], "cost": t["cost"]}
        for t in analysis["tasks"]
    ]


def llm_partition(analysis, inventory, model=DEFAULT_MODEL, feedback=None, timeout=180):
    user = {
        "boards": _inventory_summary(inventory),
        "tasks": _task_summary(analysis),
    }
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": json.dumps(user, indent=2)},
    ]
    if feedback:
        messages.append({"role": "user", "content":
            "Your previous plan failed validation:\n- " + "\n- ".join(feedback)
            + "\nReturn a corrected plan."})
    resp = requests.post(OLLAMA_URL, timeout=timeout, json={
        "model": model,
        "messages": messages,
        "format": PLAN_SCHEMA,              # constrained decoding
        "stream": False,
        "options": {"temperature": 0.1},
    })
    resp.raise_for_status()
    plan = json.loads(resp.json()["message"]["content"])
    plan["source"] = f"llm:{model}"
    return plan


def baseline_partition(analysis, inventory):
    lead = max(inventory, key=lambda b: (b["spec"]["ram_kb"], b["spec"]["cores"]))
    assignments = []
    for t in analysis["tasks"]:
        eligible = [b["id"] for b in inventory
                    if set(t["needs"]) <= set(b["spec"]["flags"])
                    and t["ram_kb"] <= b["spec"]["ram_kb"]]
        assignments.append({"task": t["name"], "boards": eligible})
    return {
        "lead": lead["id"],
        "assignments": assignments,
        "rationale": "Deterministic baseline: each task is resident on every board that "
                     "meets its needs; the board with the most RAM leads.",
        "source": "baseline",
    }


def ollama_available(model=DEFAULT_MODEL):
    try:
        tags = requests.get("http://localhost:11434/api/tags", timeout=2).json()
        return any(m.get("name", "").startswith(model.split(":")[0]) for m in tags.get("models", []))
    except Exception:
        return False
