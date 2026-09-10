"""Compare the bounded before/visit/fatal snapshots from the DeS diagnostic build."""
import argparse
import json
import math
import re
from pathlib import Path


def parse_node(line):
    result = {}
    for key, value in re.findall(r"(\w+)=([^\s]+)", line):
        if key == "key":
            result[key] = float(value)
        elif key == "readable":
            result[key] = int(value)
        else:
            result[key] = int(value, 16)
    return result


def validate(nodes, sentinel):
    issues = []
    indices = {node["addr"]: i for i, node in enumerate(nodes)}
    for i, node in enumerate(nodes):
        if not node.get("readable", 1):
            issues.append(f"Unreadable node at index {i}")
            continue
        prev = sentinel if i == 0 else nodes[i - 1]["addr"]
        next_ = sentinel if i + 1 == len(nodes) else nodes[i + 1]["addr"]
        if node["prev"] != prev or node["next"] != next_:
            issues.append(f"Snapshot prev/next mismatch at index {i}")
        if not math.isfinite(node["key"]):
            issues.append(f"Non-finite sort key at index {i}")
        if i and node["key"] < nodes[i - 1]["key"]:
            issues.append(f"Descending sort key at index {i}")
        pair_index = indices.get(node["pair"])
        if pair_index is None or pair_index > i:
            issues.append(f"Absent or later paired start at index {i}")
    return issues


def analyze(path):
    lines = path.read_text(errors="replace").splitlines()
    begin, visited, final = [], [], []
    header = None
    target = None
    for line in lines:
        if line.startswith("GuestFault: rax="):
            target = parse_node(line)["rax"]
        if line.startswith("DesTouchTrace: thread="):
            header = dict(re.findall(r"(\w+)=([^\s]+)", line))
        if re.match(r"GuestTouch: \d+ addr=", line):
            final.append(parse_node(line))
        if re.match(r"DesTouchTrace: begin \d+ addr=", line):
            begin.append(parse_node(line))
        if re.match(r"DesTouchTrace: visit \d+ addr=", line):
            visited.append(parse_node(line))
    if header is None:
        raise ValueError("No completed DesTouchTrace crash capture in this log")
    sentinel = int(header["sentinel"], 16)
    seen = set()
    unmatched_ends = []
    for i, node in enumerate(visited):
        if node["pair"] == node["addr"]:
            seen.add(node["addr"])
        elif node["pair"] not in seen:
            unmatched_ends.append({"index": i, "end": hex(node["addr"]),
                                   "start": hex(node["pair"])})
    begin_addresses = {n["addr"] for n in begin}
    final_addresses = {n["addr"] for n in final}
    changes = []
    for i, (left, right) in enumerate(zip(visited, visited[1:])):
        if left["next"] != right["addr"]:
            changes.append({"after_visit": i, "from": hex(left["addr"]),
                            "snapshot_next": hex(left["next"]),
                            "next_visit": hex(right["addr"])})
    result = {
        "log": str(path), "trace_header": header,
        "target_start": hex(target) if target is not None else None,
        "target_indices": {
            phase: [i for i, n in enumerate(nodes) if n["addr"] == target]
            for phase, nodes in (("begin", begin), ("visit", visited), ("final", final))
        },
        "begin_issues": validate(begin, sentinel),
        "final_issues": validate(final, sentinel),
        "unmatched_visited_ends": unmatched_ends,
        "final_nodes_absent_at_begin": [hex(n["addr"]) for n in final
                                         if n["addr"] not in begin_addresses],
        "visited_nodes_absent_at_begin": [hex(n["addr"]) for n in visited
                                           if n["addr"] not in begin_addresses],
        "begin_nodes_absent_at_final": [hex(n["addr"]) for n in begin
                                         if n["addr"] not in final_addresses],
        "next_pointer_changes_between_visits": changes,
        "begin": begin, "visited": visited, "final": final,
        "limits": "Snapshots are bounded and not atomic across guest threads; probes alter timing."
    }
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    report = analyze(args.log)
    output = args.log.with_name("touch-trace-analysis.json")
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items()
                      if k not in ("begin", "visited", "final")}, indent=2))
    print(f"Saved {output}")
