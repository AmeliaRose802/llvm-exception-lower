#!/usr/bin/env bash
# tests/run.sh - Bash mirror of run.ps1.
#
# For each tests/*.cpp fixture this script:
#   1. Compiles the source to LLVM bitcode (Itanium EH for fixtures whose
#      name starts with `itanium_`, MSVC EH otherwise).
#   2. Runs exception-lower on the bitcode.
#   3. Disassembles the lowered bitcode to text IR.
#   4. Runs `opt -passes=verify` on the lowered bitcode.
#   5. Verifies the lowered text IR satisfies the // CHECK directives embedded
#      in the fixture source. Supported directives are CHECK / CHECK-LABEL /
#      CHECK-NOT / CHECK-DAG, with literal-substring semantics only.
#
# Environment overrides:
#   CLANGXX, OPT, LLVM_DIS - individual tool paths (defaults autodetected).
#   EXCLOW_BIN             - path to the built exception-lower binary.
#                            Defaults to <repo-root>/build/exception-lower.
#
# Usage:
#   tests/run.sh                  # run every fixture
#   tests/run.sh add_one_multi*   # filter fixtures by basename glob

set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
out_dir="$script_dir/out"
mkdir -p "$out_dir"

filter="${1:-*}"

# -- tool discovery --------------------------------------------------------

find_tool() {
    # $1 = preferred binary name (e.g. clang++)
    # $2 = override env var name
    local pref="$1"; local var="$2"
    local cand
    if [[ -n "${!var:-}" ]]; then printf '%s' "${!var}"; return; fi
    if command -v "$pref" >/dev/null 2>&1; then command -v "$pref"; return; fi
    # Try common versioned suffixes (newest first).
    for v in 20 19 18 17 16 15 14; do
        cand="${pref}-${v}"
        if command -v "$cand" >/dev/null 2>&1; then command -v "$cand"; return; fi
    done
    echo "ERROR: cannot find $pref (try setting \$$var)" >&2
    exit 1
}

CLANGXX="$(find_tool clang++ CLANGXX)"
OPT="$(find_tool opt OPT)"
LLVM_DIS="$(find_tool llvm-dis LLVM_DIS)"
EXCLOW_BIN="${EXCLOW_BIN:-$repo_root/build/exception-lower}"

if [[ ! -x "$EXCLOW_BIN" ]]; then
    echo "ERROR: exception-lower binary not found at $EXCLOW_BIN" >&2
    echo "       Build the project first, or set \$EXCLOW_BIN." >&2
    exit 1
fi

echo "clang++:         $CLANGXX"
echo "opt:             $OPT"
echo "llvm-dis:        $LLVM_DIS"
echo "exception-lower: $EXCLOW_BIN"
echo

# -- CHECK directive verifier ----------------------------------------------
#
# Parses // CHECK[-NOT|-DAG|-LABEL]: <pattern> directives from the fixture
# source and asserts presence/absence of literal substrings in the lowered IR.

verify_checks() {
    # $1 = source .cpp file (carries CHECK directives)
    # $2 = lowered .ll file (target of the checks)
    #
    # Writes per-check failure messages to stderr (visible to the user) and
    # writes only the summary "<total> <failed>" line to stdout (so the
    # caller can `read` the counts back without picking up failure noise).
    local src="$1"; local ir="$2"
    local total=0 failed=0
    local ir_body; ir_body="$(cat "$ir")"
    local line kind pattern
    while IFS= read -r line; do
        # Trim leading whitespace.
        line="${line#"${line%%[![:space:]]*}"}"
        # Match `// CHECK[-NOT|-DAG|-LABEL]: <pattern>` or the `;`-prefixed
        # LLVM IR comment form used by .ll fixtures.
        if [[ "$line" =~ ^(//|\;)[[:space:]]*(CHECK(-NOT|-DAG|-LABEL)?):[[:space:]]+(.*)$ ]]; then
            kind="${BASH_REMATCH[2]}"
            pattern="${BASH_REMATCH[4]}"
            # Trim trailing whitespace from pattern.
            pattern="${pattern%"${pattern##*[![:space:]]}"}"
            total=$((total+1))
            local present=0
            if [[ "$ir_body" == *"$pattern"* ]]; then present=1; fi
            case "$kind" in
                CHECK|CHECK-LABEL|CHECK-DAG)
                    if [[ $present -eq 0 ]]; then
                        printf '    missing %s: %s\n' "$kind" "$pattern" >&2
                        failed=$((failed+1))
                    fi
                    ;;
                CHECK-NOT)
                    if [[ $present -eq 1 ]]; then
                        printf '    forbidden %s present: %s\n' "$kind" "$pattern" >&2
                        failed=$((failed+1))
                    fi
                    ;;
            esac
        fi
    done <"$src"
    printf '%d %d\n' "$total" "$failed"
}

# Read an optional `// EXCLOW-ARGS: <args>` / `; EXCLOW-ARGS: <args>` directive
# from the fixture and echo the extra arguments to forward to exception-lower
# (e.g. `--mode=cleanup-only`). Echoes nothing when absent.
get_exclow_args() {
    local src="$1" line
    while IFS= read -r line; do
        line="${line#"${line%%[![:space:]]*}"}"
        if [[ "$line" =~ ^(//|\;)[[:space:]]*EXCLOW-ARGS:[[:space:]]+(.*)$ ]]; then
            local args="${BASH_REMATCH[2]}"
            args="${args%"${args##*[![:space:]]}"}"
            printf '%s' "$args"
            return
        fi
    done <"$src"
}

# -- fixture loop ----------------------------------------------------------

shopt -s nullglob
fixtures=()
for f in "$script_dir"/*.cpp "$script_dir"/*.ll; do
    base="$(basename "$f")"; base="${base%.*}"
    # shellcheck disable=SC2053
    [[ $base == $filter ]] && fixtures+=("$f")
done

if [[ ${#fixtures[@]} -eq 0 ]]; then
    echo "ERROR: no fixtures matched '$filter'" >&2
    exit 1
fi

total_pass=0
total_fail=0
failed_names=()

for f in "${fixtures[@]}"; do
    base="$(basename "$f")"; name="${base%.*}"
    ext="${f##*.}"
    echo "=== $name ==="

    bc="$out_dir/$name.bc"
    low_bc="$out_dir/${name}_lowered.bc"
    low_ll="$out_dir/${name}_lowered.ll"

    # Extra args (e.g. --mode=cleanup-only) declared by the fixture itself.
    exc_extra="$(get_exclow_args "$f")"
    read -r -a exc_extra_arr <<<"$exc_extra"

    if [[ "$ext" == "ll" ]]; then
        # Pre-built LLVM IR fixture: feed straight to exception-lower.
        lower_input="$f"
    else
        triple="x86_64-pc-windows-msvc"
        [[ "$name" == itanium_* ]] && triple="x86_64-pc-linux-gnu"
        if ! "$CLANGXX" -target "$triple" -O0 -emit-llvm -c "$f" -o "$bc" 2>&1; then
            echo "  FAIL (clang++)"
            total_fail=$((total_fail+1)); failed_names+=("$name (compile)"); continue
        fi
        lower_input="$bc"
    fi

    if ! "$EXCLOW_BIN" "$lower_input" "${exc_extra_arr[@]}" -o "$low_bc" 2>&1; then
        echo "  FAIL (exception-lower)"
        total_fail=$((total_fail+1)); failed_names+=("$name (lower)"); continue
    fi
    if ! "$LLVM_DIS" "$low_bc" -o "$low_ll" 2>&1; then
        echo "  FAIL (llvm-dis)"
        total_fail=$((total_fail+1)); failed_names+=("$name (disas)"); continue
    fi
    if ! "$OPT" -passes=verify "$low_bc" -disable-output 2>&1; then
        echo "  FAIL (opt -passes=verify)"
        total_fail=$((total_fail+1)); failed_names+=("$name (verify)"); continue
    fi

    read -r checks_total checks_failed <<<"$(verify_checks "$f" "$low_ll")"
    if [[ "$checks_total" -eq 0 ]]; then
        echo "  PASS (no CHECK directives; verifier-only)"
        total_pass=$((total_pass+1))
    elif [[ "$checks_failed" -eq 0 ]]; then
        printf '  PASS (%s CHECK directives)\n' "$checks_total"
        total_pass=$((total_pass+1))
    else
        printf '  FAIL (%s of %s CHECK directives)\n' "$checks_failed" "$checks_total"
        total_fail=$((total_fail+1)); failed_names+=("$name")
    fi
    echo
done

echo
if [[ $total_fail -eq 0 ]]; then
    echo "Summary: $total_pass passed, $total_fail failed"
    exit 0
else
    echo "Summary: $total_pass passed, $total_fail failed"
    printf 'Failed: %s\n' "$(IFS=', '; echo "${failed_names[*]}")"
    exit 1
fi
