#!/usr/bin/env bash
# Proves a change touched only comments/whitespace: strips comments from each
# file (as of REV and as in the working tree) with the compiler's own lexer,
# collapses whitespace, and diffs the token streams.
#
#   scripts/check_comment_only.sh [--rev REV] file...   (REV defaults to HEAD)
#
# Exit 0 = every file is code-identical to REV; 1 = at least one differs.
set -euo pipefail

rev=HEAD
if [[ "${1:-}" == "--rev" ]]; then rev=$2; shift 2; fi
[[ $# -gt 0 ]] || { echo "usage: $0 [--rev REV] file..." >&2; exit 2; }

strip() {
  # -fpreprocessed: lex only, drop comments, don't expand #include/macros.
  g++ -x c++ -fpreprocessed -dD -E -P - 2>/dev/null | tr -s ' \t\r\n' ' '
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
status=0
for f in "$@"; do
  git show "$rev:$f" 2>/dev/null | strip > "$tmp/old" || true
  strip < "$f" > "$tmp/new"
  if cmp -s "$tmp/old" "$tmp/new"; then
    echo "ok        $f"
  else
    echo "CODE DIFF $f"
    # One token per line so diff points at the changed token, not a whole file.
    tr ' ' '\n' < "$tmp/old" > "$tmp/old.f"
    tr ' ' '\n' < "$tmp/new" > "$tmp/new.f"
    diff "$tmp/old.f" "$tmp/new.f" | head -20 || true
    status=1
  fi
done
exit $status
