"""Fail a pull request that changes src/ without touching the [Unreleased] section of CHANGELOG.md.

Compares the [Unreleased] section (ignoring HTML comments and blank lines) at the merge base and
at the PR head; any difference counts, including a release PR that moves its entries into a new
version section. A PR that adds a new "## [x.y.z]" section also passes. Used by .github/workflows/changelog.yml; add the "skip-changelog" label to a PR
whose src/ change is not user-facing.

Usage: python -I tools/check_pr_changelog.py <base-sha> <head-sha>
"""
import re
import subprocess
import sys


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True,
                          encoding="utf-8").stdout


def unreleased(rev):
    try:
        text = git("show", f"{rev}:CHANGELOG.md")
    except subprocess.CalledProcessError:
        return None  # no CHANGELOG.md at this revision
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    section, inside = [], False
    for line in text.splitlines():
        if line.startswith("## "):
            if inside:
                break
            inside = line.strip().lower().startswith("## [unreleased]")
            continue
        if inside and re.match(r"^\[[^\]]+\]:\s", line):
            break  # link references at the end of the file
        if inside and line.strip():
            section.append(line.rstrip())
    return section


def versions(rev):
    """Version headings ("## [x.y.z]") in CHANGELOG.md at rev."""
    try:
        text = git("show", f"{rev}:CHANGELOG.md")
    except subprocess.CalledProcessError:
        return set()
    return set(re.findall(r"^## \[(\d[^\]]*)\]", text, flags=re.M))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    base = git("merge-base", sys.argv[1], sys.argv[2]).strip()
    head = sys.argv[2]
    changed = git("diff", "--name-only", f"{base}..{head}").splitlines()
    src = [f for f in changed if f.startswith("src/")]
    if not src:
        print("No changes under src/ - no changelog entry needed.")
        return
    print(f"{len(src)} file(s) changed under src/, e.g. {', '.join(src[:3])}")

    after = unreleased(head)
    if after is None:
        sys.exit("CHANGELOG.md is missing at the PR head.")
    added = versions(head) - versions(base)
    if added:
        print(f"CHANGELOG.md gained a release section: {', '.join(sorted(added))}.")
        return
    if unreleased(base) == after:
        sys.exit("This PR changes src/ but doesn't update the [Unreleased] section of CHANGELOG.md.\n"
                 "Add a line describing the user-facing change there, or, if nothing changes for "
                 "players, add the 'skip-changelog' label to the PR.")
    print("CHANGELOG.md [Unreleased] was updated.")


if __name__ == "__main__":
    main()
