#!/usr/bin/env python3
"""Revalidate collated-003: feature state lives in process globals."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from inspector_revalidation_common import (
    base_result,
    exit_code_for_verdict,
    repo_root,
    run_command,
    write_result,
)


COLLATED_ID = "collated-003"
TITLE = "Feature state lives in process globals"


MODEL_SOURCE = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t confspec_section_total = 0;
static int64_t confspec_field_total = 0;

static void confspec_register(int64_t sections, int64_t fields) {
    confspec_section_total += sections;
    confspec_field_total += fields;
}

typedef struct once_slot_t {
    uint64_t key;
    uint64_t value;
    bool done;
    struct once_slot_t *next;
} once_slot_t;

static once_slot_t *once_slots;

static once_slot_t *once_slot(uint64_t key, bool create) {
    for (once_slot_t *slot = once_slots; slot; slot = slot->next) {
        if (slot->key == key) {
            return slot;
        }
    }
    if (!create) {
        return NULL;
    }
    once_slot_t *slot = calloc(1, sizeof(*slot));
    slot->key = key;
    slot->next = once_slots;
    once_slots = slot;
    return slot;
}

static int once_is_done(uint64_t key) {
    once_slot_t *slot = once_slot(key, false);
    return slot && slot->done ? 1 : 0;
}

static uint64_t once_get(uint64_t key) {
    once_slot_t *slot = once_slot(key, false);
    return slot ? slot->value : 0;
}

static void once_store(uint64_t key, uint64_t value) {
    once_slot_t *slot = once_slot(key, true);
    slot->value = value;
    slot->done = true;
}

int main(void) {
    const char *same_pointer_name = "init";
    char equal_name_a[] = "init";
    char equal_name_b[] = "init";
    uint64_t same_key = (uint64_t)(uintptr_t)same_pointer_name;
    uint64_t equal_key_a = (uint64_t)(uintptr_t)equal_name_a;
    uint64_t equal_key_b = (uint64_t)(uintptr_t)equal_name_b;

    printf("initial_confspec=%lld/%lld\n",
           (long long)confspec_section_total,
           (long long)confspec_field_total);
    confspec_register(2, 3);
    printf("after_first_session=%lld/%lld\n",
           (long long)confspec_section_total,
           (long long)confspec_field_total);
    confspec_register(1, 4);
    printf("after_second_session=%lld/%lld\n",
           (long long)confspec_section_total,
           (long long)confspec_field_total);

    once_store(same_key, 111);
    printf("same_pointer_done=%d value=%llu\n",
           once_is_done(same_key),
           (unsigned long long)once_get(same_key));
    printf("equal_names_strcmp=%d pointer_equal=%d\n",
           strcmp(equal_name_a, equal_name_b),
           equal_key_a == equal_key_b ? 1 : 0);
    once_store(equal_key_a, 222);
    printf("equal_name_b_done=%d value=%llu\n",
           once_is_done(equal_key_b),
           (unsigned long long)once_get(equal_key_b));

    return confspec_section_total == 3
        && confspec_field_total == 7
        && once_is_done(same_key)
        && once_get(same_key) == 111
        && strcmp(equal_name_a, equal_name_b) == 0
        && equal_key_a != equal_key_b
        && !once_is_done(equal_key_b)
        ? 0
        : 1;
}
"""


def source_patterns_hold() -> tuple[bool, list[str]]:
    root = repo_root()
    builtins = (root / "src/slay/codegen_builtins.c").read_text(encoding="utf-8")
    codegen = (root / "src/slay/codegen.c").read_text(encoding="utf-8")
    checks = [
        (
            "confspec section count is file-static",
            "static int64_t confspec_section_total" in builtins,
        ),
        (
            "confspec field count is file-static",
            "static int64_t confspec_field_total" in builtins,
        ),
        (
            "once slots live in a file-static linked list",
            "static n00b_once_slot_t *once_slots" in builtins,
        ),
        (
            "once key is emitted from current_once_key pointer",
            "(uintptr_t)s->current_once_key" in codegen,
        ),
        (
            "once lowering assigns current_once_key from func_name",
            "s->current_once_key" in codegen and "func_name" in codegen,
        ),
    ]
    observations = [f"{name}: {'yes' if ok else 'no'}" for name, ok in checks]
    return all(ok for _, ok in checks), observations


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--n00b", type=Path, default=Path("build_debug/n00b"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    source_holds, source_observations = source_patterns_hold()
    cc = shutil.which("cc")
    commands = []
    limitations = []
    model_holds = False

    if cc is None:
        limitations.append("No C compiler named cc was available for the standalone reproduction model.")
    else:
        with tempfile.TemporaryDirectory(prefix="n00b-revalidate-003.") as tmp:
            tmp_path = Path(tmp)
            model_c = tmp_path / "collated_003_state_leak_model.c"
            model_bin = tmp_path / "collated_003_state_leak_model"
            model_c.write_text(MODEL_SOURCE, encoding="utf-8")
            compile_cmd = run_command([cc, "-std=c99", "-Wall", "-Wextra", "-o", model_bin, model_c], timeout=args.timeout)
            commands.append(compile_cmd)
            if compile_cmd["exit_code"] == 0:
                run_cmd = run_command([model_bin], timeout=args.timeout)
                commands.append(run_cmd)
                model_holds = run_cmd["exit_code"] == 0
            else:
                limitations.append("The standalone C reproduction model did not compile.")

    if source_holds and model_holds:
        verdict = "holds"
        confidence = "high"
        summary = (
            "The current source still stores confspec counters and once slots in process-global static state, "
            "and the compiled reproduction model demonstrates the resulting same-process leakage."
        )
    elif not source_holds:
        verdict = "refuted"
        confidence = "medium"
        summary = "The source no longer matches the process-global confspec/once state pattern checked by this finding."
    else:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The source still matches the global-state pattern, but the standalone reproduction model could not run."

    observations = source_observations
    if commands:
        observations.append(f"C model final command exit={commands[-1]['exit_code']}")

    artifacts = [
        {
            "kind": "reproduction",
            "description": "Standalone C model of the static confspec counters and pointer-keyed once slot list.",
        },
        {
            "kind": "other",
            "description": "Repository source-pattern checks for process-global confspec counters and once slot state.",
        },
    ]
    result = base_result(
        COLLATED_ID,
        TITLE,
        verdict,
        confidence,
        summary,
        commands=commands,
        observations=observations,
        artifacts=artifacts,
        limitations=limitations,
    )
    write_result(args.output_dir, result)
    return exit_code_for_verdict(verdict)


if __name__ == "__main__":
    raise SystemExit(main())
