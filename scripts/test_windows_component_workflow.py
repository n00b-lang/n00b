from pathlib import Path


workflow = (
    Path(__file__).resolve().parents[1]
    / ".github"
    / "workflows"
    / "windows-component.yml"
)
assert workflow.is_file(), "missing n00b-owned Windows validation workflow"
text = workflow.read_text(encoding="utf-8")

for contract in (
    "workflow_dispatch:",
    "ncc_run_id:",
    "ncc_sha:",
    "repository: crashappsec/ncc",
    # The run id is now RESOLVED rather than hand-entered (n00b#239): a
    # workflow_dispatch input cannot drive a pull_request gate. What must not
    # change is that the id is still what selects the artifact.
    "run-id: ${{ steps.nccrun.outputs.run_id }}",
    "github-token: ${{ github.token }}",
    ".\\build.ps1",
    "meson test",
    "condition",
    "actions/upload-artifact@",
    # Per-PR trigger: the point of #239.
    "pull_request:",
    # The resolved artifact must match the pin every other platform builds
    # with, or Windows silently gates on a different compiler.
    "NCC_REV_DEFAULT",
):
    assert contract in text, f"missing Windows workflow contract: {contract}"

# Unchanged policy: consume NCC's published Windows artifact, do not rebuild
# ncc here. #239 considered source-building it for consistency with ci.yml and
# rejected that in favour of pinning WHICH artifact is consumed.
for forbidden in ("Build NCC", "Build ncc"):
    assert forbidden not in text, (
        "n00b must consume NCC's artifact instead of rebuilding it"
    )
print("n00b Windows workflow contract: pass")
