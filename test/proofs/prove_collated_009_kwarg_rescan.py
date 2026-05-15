#!/usr/bin/env python3
"""Attempt to prove collated-009 with a deterministic nested-call scan model."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field


@dataclass
class Node:
    name: str
    children: list["Node"] = field(default_factory=list)
    token: bool = False


@dataclass
class Counts:
    total_depth: int
    calls: int = 0
    find_entries: int = 0
    call_args_visits: int = 0
    deepest_call_args_visits: int = 0


def build_nested_call(depth: int, current: int = 0) -> Node:
    if depth <= 0:
        return Node("int-lit", [Node("1", token=True)])

    nested_expr = build_nested_call(depth - 1, current + 1)
    args = Node(
        "call-args",
        [Node("expression", [nested_expr])],
    )
    args.depth_from_outermost = current  # type: ignore[attr-defined]

    return Node(
        "call",
        [
            Node("identifier", [Node("identity", token=True)]),
            args,
        ],
    )


def find_nt_deep(node: Node | None, nt_name: str, counts: Counts) -> Node | None:
    if node is None:
        return None

    counts.find_entries += 1

    if node.token:
        return None

    if node.name == "call-args":
        counts.call_args_visits += 1
        if not node.children:
            return None

        depth_from_outermost = getattr(node, "depth_from_outermost", None)
        if depth_from_outermost == counts.total_depth - 1:
            counts.deepest_call_args_visits += 1

    if node.name == nt_name:
        return node

    for child in node.children:
        found = find_nt_deep(child, nt_name, counts)
        if found is not None:
            return found

    return None


def codegen_call_walk(node: Node, counts: Counts) -> None:
    if node.name != "call":
        for child in node.children:
            if not child.token:
                codegen_call_walk(child, counts)
        return

    counts.calls += 1
    args_node = next(child for child in node.children if child.name == "call-args")

    found = find_nt_deep(args_node, "call-kw-arg", counts)
    if found is not None:
        raise AssertionError("test tree unexpectedly contains call-kw-arg")

    for child in args_node.children:
        codegen_call_walk(child, counts)


def nested_source(depth: int) -> str:
    expr = "1"
    for _ in range(depth):
        expr = f"identity({expr})"
    return (
        "func identity(x: int) -> int {\n"
        "    return x\n"
        "}\n\n"
        f"print({expr})\n"
    )


def run_depth(depth: int) -> Counts:
    counts = Counts(total_depth=depth)
    root = build_nested_call(depth)
    codegen_call_walk(root, counts)
    return counts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--depth", type=int, default=64)
    parser.add_argument(
        "--show-source",
        action="store_true",
        help="Print a representative nested positional-call source fixture.",
    )
    args = parser.parse_args()

    if args.depth < 1:
        parser.error("--depth must be positive")

    if args.show_source:
        print(nested_source(args.depth))

    print("collated-009 proof attempt")
    print("depth calls find_entries call_args_visits expected_triangular deepest_visits")

    proved = True
    for depth in sorted(set([1, 2, 4, 8, 16, 32, args.depth])):
        counts = run_depth(depth)
        expected_triangular = depth * (depth + 1) // 2
        line_ok = (
            counts.calls == depth
            and counts.call_args_visits == expected_triangular
            and counts.deepest_call_args_visits == depth
        )
        proved = proved and line_ok
        print(
            f"{depth:5d} {counts.calls:5d} {counts.find_entries:12d} "
            f"{counts.call_args_visits:16d} {expected_triangular:19d} "
            f"{counts.deepest_call_args_visits:14d}"
        )

    if proved:
        print("PROVED: positional nested calls cause triangular call-args rescans.")
        return 0

    print("NOT PROVED: scan counts did not match the expected repeated walk.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
