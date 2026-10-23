#!/usr/bin/env python3
"""tinySA MCP test double -- a stdio MCP server with the same tool names as
`~/Repos/tinySA_mcp/tinySA_Operator.py`, without a radio or a GUI.

It exists so `tools/cat_rf_test.py --mcp-selftest` and `--sim` can exercise the
real MCP client path (JSON-RPC over stdio) on a machine with no tinySA attached.
Only the tools the test uses are implemented: `connect`, `disconnect`,
`set_generator`, `get_generator_state`, `execute_command` (always `+OK`).

    python3 tools/tinysa_mcp_sim.py      # speaks MCP on stdin/stdout

The generator state is in memory; there is no RF. A real run swaps this for
`uv --directory ~/Repos/tinySA_mcp run tinySA_Operator.py` (the default
`--mcp-cmd`), which speaks the same protocol to a real instrument.
"""
import json
import sys

PROTOCOL_VERSION = "2024-11-05"

# Classic tinySA output stages: low up to ~350 MHz, high from ~240 MHz.
LOW_MAX_HZ = 350_000_000
HIGH_MIN_HZ = 240_000_000
LOW_RANGE = (-76.0, -6.0)
HIGH_RANGE = (-38.0, 13.0)

state = {
    "connected": False,
    "port": None,
    "output": False,
    "frequency_hz": None,
    "level_dbm": None,
    "modulation": None,
    "high_output": False,
}


def stage_for(freq_hz):
    return bool(freq_hz is not None and freq_hz >= HIGH_MIN_HZ)


def send(obj):
    sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def result(msg_id, value):
    send({"jsonrpc": "2.0", "id": msg_id, "result": value})


def error(msg_id, code, message):
    send({"jsonrpc": "2.0", "id": msg_id,
          "error": {"code": code, "message": message}})


TOOLS = [
    {
        "name": "connect",
        "description": "Open the serial connection to the tinySA.",
        "inputSchema": {"type": "object",
                        "properties": {"port": {"type": "string"}},
                        "required": ["port"]},
    },
    {
        "name": "disconnect",
        "description": "Close the serial connection.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "set_generator",
        "description": "Switch to generator mode and apply output settings.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "port": {"type": "string"},
                "output": {"type": "boolean"},
                "frequency_hz": {"type": "number"},
                "level_dbm": {"type": "number"},
                "modulation": {"type": "string"},
                "high_output": {"type": "boolean"},
            },
            "required": ["port", "output"],
        },
    },
    {
        "name": "get_generator_state",
        "description": "Report the generator state.",
        "inputSchema": {"type": "object",
                        "properties": {"port": {"type": "string"}},
                        "required": ["port"]},
    },
    {
        "name": "execute_command",
        "description": "Send a console command to the tinySA.",
        "inputSchema": {"type": "object",
                        "properties": {"port": {"type": "string"},
                                       "command": {"type": "string"}},
                        "required": ["port", "command"]},
    },
]


def text_result(value):
    return [{"type": "text", "text": json.dumps(value, separators=(",", ":"))}]


def call_tool(name, args):
    if name == "connect":
        state["port"] = args.get("port")
        state["connected"] = bool(state["port"])
        return {"status": "success" if state["connected"] else "error",
                "port": state["port"], "connected": state["connected"]}

    if name == "disconnect":
        state["connected"] = False
        return {"status": "success", "port": state["port"]}

    if name == "set_generator":
        if not args.get("port"):
            return {"status": "error", "message": "Port parameter is required."}
        state["connected"] = True
        state["port"] = args["port"]
        freq = args.get("frequency_hz")
        level = args.get("level_dbm")
        high = bool(args.get("high_output", False))
        if freq is None:
            freq = state["frequency_hz"]
        if level is None:
            level = state["level_dbm"]
        if freq is None:
            return {"status": "error", "message": "frequency_hz is required"}
        want_high = stage_for(float(freq))
        if high != want_high:
            return {"status": "error",
                    "message": "high_output does not match the frequency stage"}
        lo, hi = HIGH_RANGE if want_high else LOW_RANGE
        if level is None or not (lo <= float(level) <= hi):
            return {"status": "error",
                    "message": f"level_dbm {level} outside {lo}..{hi}"}
        state.update(output=bool(args.get("output", True)),
                     frequency_hz=float(freq),
                     level_dbm=float(level),
                     high_output=want_high,
                     modulation=args.get("modulation"))
        return {"status": "success", "port": state["port"],
                "output": state["output"],
                "frequency_hz": state["frequency_hz"],
                "level_dbm": state["level_dbm"],
                "modulation": state["modulation"],
                "high_output": state["high_output"]}

    if name == "get_generator_state":
        if not args.get("port"):
            return {"status": "error", "message": "Port parameter is required."}
        return {"status": "success", "port": args["port"],
                "connected": state["connected"],
                "output": state["output"],
                "frequency_hz": state["frequency_hz"],
                "level_dbm": state["level_dbm"],
                "modulation": state["modulation"],
                "high_output": state["high_output"],
                "level_range_dbm": list(HIGH_RANGE if state["high_output"]
                                        else LOW_RANGE)}

    if name == "execute_command":
        return {"status": "success", "response": "+OK"}

    return {"status": "error", "message": f"unknown tool {name}"}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

        msg_id = msg.get("id")
        method = msg.get("method", "")
        params = msg.get("params") or {}

        if msg_id is None:                  # a notification
            continue

        if method == "initialize":
            result(msg_id, {
                "protocolVersion": params.get("protocolVersion", PROTOCOL_VERSION),
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "tinysa-mcp-sim", "version": "1.0"},
            })
        elif method == "ping":
            result(msg_id, {})
        elif method == "tools/list":
            result(msg_id, {"tools": TOOLS})
        elif method == "tools/call":
            name = params.get("name", "")
            args = params.get("arguments") or {}
            payload = call_tool(name, args)
            result(msg_id, {
                "content": text_result(payload),
                "isError": payload.get("status") == "error",
            })
        else:
            error(msg_id, -32601, f"method not found: {method}")


if __name__ == "__main__":
    try:
        main()
    except (BrokenPipeError, KeyboardInterrupt):
        pass
