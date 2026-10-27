#!/usr/bin/env bash
set -uo pipefail

SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD=""
PREFIX=""
PREFIX_GIVEN=no
LINK=no
PATH_CHOICE=ask
MODE=install

die() {
    printf 'install: error: %s\n' "$*" >&2
    exit 1
}

usage_error() {
    printf 'install: error: %s\n' "$*" >&2
    printf "Run 'tools/install.sh --help' for usage.\n" >&2
    exit 2
}

usage() {
    cat <<'USAGE'
Usage: tools/install.sh --build DIR [--prefix DIR] [--link]
                        [--add-to-path | --no-modify-path]
       tools/install.sh --uninstall [--prefix DIR]
USAGE
}

option_value() {
    [ "$#" -ge 2 ] && [ -n "$2" ] && case "$2" in -*) false ;; esac ||
        usage_error "$1 needs a directory"
    printf '%s' "$2"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build) BUILD="$(option_value "$@")" || exit 2; shift ;;
        --build=*) BUILD="${1#--build=}" ;;
        --prefix) PREFIX="$(option_value "$@")" || exit 2; PREFIX_GIVEN=yes; shift ;;
        --prefix=*) PREFIX="${1#--prefix=}"; PREFIX_GIVEN=yes ;;
        --link) LINK=yes ;;
        --add-to-path) PATH_CHOICE=add ;;
        --no-modify-path) PATH_CHOICE=skip ;;
        --uninstall) MODE=uninstall ;;
        -h|--help) usage; exit 0 ;;
        *) usage_error "unknown option '$1'" ;;
    esac
    shift
done

[ -n "$PREFIX" ] || PREFIX="$HOME/.local"
if [ "$MODE" = uninstall ]; then
    [ -d "$PREFIX" ] || die "no n00b toolchains are installed under $PREFIX: the directory does not exist"
else
    mkdir -p "$PREFIX" 2>/dev/null || die "cannot create the prefix directory $PREFIX"
fi
PREFIX="$(cd "$PREFIX" && pwd)"
STORE="$PREFIX/share/n00b"
BIN="$PREFIX/bin"

pretty() {
    case "$1" in
        "$HOME"/*) printf '~/%s' "${1#"$HOME"/}" ;;
        *) printf '%s' "$1" ;;
    esac
}

on_path() {
    case ":$PATH:" in
        *":$1:"*|*":$1/:"*) return 0 ;;
    esac
    return 1
}

first_on_path() {
    local dir
    local IFS=:
    for dir in $PATH; do
        [ -n "$dir" ] && [ -x "$dir/n00b" ] && [ ! -d "$dir/n00b" ] && {
            printf '%s\n' "$dir/n00b"
            return 0
        }
    done
    return 1
}

resolved() {
    local path="$1"
    while [ -L "$path" ]; do
        local target
        target="$(readlink "$path")"
        case "$target" in
            /*) path="$target" ;;
            *) path="$(dirname "$path")/$target" ;;
        esac
    done
    (cd "$(dirname "$path")" 2>/dev/null && printf '%s/%s\n' "$(pwd -P)" "$(basename "$path")")
}

shell_rc() {
    case "$(basename "${SHELL:-sh}")" in
        zsh) printf '%s\n' "$HOME/.zshrc" ;;
        bash)
            if [ "$(uname -s)" = Darwin ]; then
                printf '%s\n' "$HOME/.bash_profile"
            else
                printf '%s\n' "$HOME/.bashrc"
            fi ;;
        fish) printf '%s\n' "$HOME/.config/fish/config.fish" ;;
        *) printf '%s\n' "$HOME/.profile" ;;
    esac
}

path_line() {
    local dir="$1"
    case "$dir" in
        "$HOME"/*) dir="\$HOME/${dir#"$HOME"/}" ;;
    esac
    if [ "$(basename "${SHELL:-sh}")" = fish ]; then
        printf 'fish_add_path %s # added by n00b install\n' "$dir"
    else
        printf 'export PATH="%s:$PATH" # added by n00b install\n' "$dir"
    fi
}

boxed() {
    local width=0 line
    for line in "$@"; do
        [ "${#line}" -gt "$width" ] && width="${#line}"
    done
    printf '\n  +%s+\n' "$(printf '%*s' $((width + 2)) '' | tr ' ' '-')"
    for line in "$@"; do
        printf '  | %-*s |\n' "$width" "$line"
    done
    printf '  +%s+\n\n' "$(printf '%*s' $((width + 2)) '' | tr ' ' '-')"
}

uninstall() {
    [ -d "$STORE" ] || die "no n00b toolchains are installed under $(pretty "$PREFIX")"
    local link
    if [ -f "$STORE/links" ]; then
        while IFS= read -r link; do
            [ -L "$link" ] && rm -f "$link" && printf 'Removed %s\n' "$(pretty "$link")"
        done < "$STORE/links"
    fi
    if [ -L "$BIN/n00b" ]; then
        rm -f "$BIN/n00b"
        printf 'Removed %s\n' "$(pretty "$BIN/n00b")"
    fi
    rm -rf "$STORE"
    printf 'Removed every toolchain in %s\n' "$(pretty "$STORE")"
    local rc
    for rc in "$HOME/.zshrc" "$HOME/.bashrc" "$HOME/.bash_profile" \
              "$HOME/.profile" "$HOME/.config/fish/config.fish"; do
        if [ -f "$rc" ] && grep -q '# added by n00b install' "$rc"; then
            printf '\n%s still has lines the installer added:\n' "$(pretty "$rc")"
            grep -n '# added by n00b install' "$rc" | sed 's/^/    /'
            printf 'Remove them if you no longer want them.\n'
        fi
    done
}

if [ "$MODE" = uninstall ]; then
    uninstall
    exit 0
fi

[ -n "$BUILD" ] || { usage >&2; die "--build is required"; }
COMPILER="$BUILD/n00b"
[ -x "$COMPILER" ] || die "no compiler at $COMPILER; build first"

MARKER="$("$COMPILER" version --marker)" || die "cannot read the compiler's version"
VERSION="$(printf '%s\n' "$MARKER" | sed -n 's/^version=//p')"
BUILD_NUMBER="$(printf '%s\n' "$MARKER" | sed -n 's/^build=//p')"
ID="$VERSION-$BUILD_NUMBER"
[ "$LINK" = yes ] && ID="$ID-link"
DEST="$STORE/toolchains/$ID"
STAGE="$STORE/toolchains/.$ID.staging"

printf 'Installing n00b %s\n' "$ID"

NOTES=()

advance() {
    printf '  %s\n' "$1"
}

stage_files() {
    rm -rf "$STAGE"
    mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share/grammar" \
             "$STAGE/share/writ" "$STAGE/include" || return 1
    cp "$COMPILER" "$STAGE/bin/n00b" || return 1
    cp "$BUILD/libn00b-abi-runtime.a" "$BUILD/libn00b-abi-runtime.bc" \
       "$STAGE/lib/" || return 1
    for cross in "$BUILD"/*/libn00b-abi-runtime.a; do
        [ -f "$cross" ] || continue
        profile="$(basename "$(dirname "$cross")")"
        mkdir -p "$STAGE/lib/$profile" &&
            cp "$cross" "${cross%.a}.bc" "$STAGE/lib/$profile/" || return 1
    done
    cp "$SOURCE/abi/n00b-abi-v1.h" "$SOURCE/runtime/builtin_os.h" \
       "$SOURCE/runtime/builtin_abi.h" "$STAGE/include/" || return 1
    if [ "$LINK" = yes ]; then
        ln -s "$SOURCE/lib" "$STAGE/stdlib" &&
        ln -s "$SOURCE/grammar/n00b.bnf" "$STAGE/share/grammar/n00b.bnf" &&
        ln -s "$SOURCE/grammar/n00b.lex" "$STAGE/share/grammar/n00b.lex" &&
        ln -s "$SOURCE/writ/profiles" "$STAGE/share/writ/profiles" &&
        ln -s "$SOURCE/writ/contracts" "$STAGE/share/writ/contracts" || return 1
    else
        cp -R "$SOURCE/lib" "$STAGE/stdlib" &&
        cp "$SOURCE/grammar/n00b.bnf" "$SOURCE/grammar/n00b.lex" \
           "$STAGE/share/grammar/" &&
        cp -R "$SOURCE/writ/profiles" "$STAGE/share/writ/profiles" &&
        cp -R "$SOURCE/writ/contracts" "$STAGE/share/writ/contracts" || return 1
    fi
    mkdir -p "$STAGE/share/completions" || return 1
    local shell
    for shell in bash zsh fish; do
        "$COMPILER" completions "$shell" > "$STAGE/share/completions/n00b.$shell" ||
            return 1
    done
    "$COMPILER" completions powershell > "$STAGE/share/completions/n00b.ps1" ||
        return 1
    printf 'id=%s\n%s\n' "$ID" "$MARKER" > "$STAGE/n00b-toolchain"
}

PREVIOUS=""
[ -L "$STORE/current" ] && PREVIOUS="$(basename "$(readlink "$STORE/current")")"

stage_files || die "could not copy the toolchain into $(pretty "$STAGE")"
advance "copied files"

rm -rf "$DEST" && mv "$STAGE" "$DEST" || die "could not place the toolchain at $(pretty "$DEST")"
advance "placed toolchain"

ln -sfn "toolchains/$ID" "$STORE/current" ||
    die "could not make $ID the active toolchain"
advance "activated toolchain"

mkdir -p "$BIN" && ln -sfn "../share/n00b/current/bin/n00b" "$BIN/n00b" ||
    die "could not link $(pretty "$BIN/n00b")"
: > "$STORE/links"
EXTRA_LINK=""
if [ "$PREFIX_GIVEN" = no ] && ! on_path "$BIN"; then
    for candidate in "$HOME/.local/bin" "$HOME/bin"; do
        if [ "$candidate" != "$BIN" ] && [ -d "$candidate" ] \
           && [ -w "$candidate" ] && on_path "$candidate"; then
            ln -sfn "$STORE/current/bin/n00b" "$candidate/n00b" &&
                EXTRA_LINK="$candidate/n00b" &&
                printf '%s\n' "$EXTRA_LINK" >> "$STORE/links"
            break
        fi
    done
fi
advance "linked n00b"

WARM="$(mktemp -d "${TMPDIR:-/tmp}/n00b-install.XXXXXX")"
printf 'var ready = 1\n' > "$WARM/warm.n"
START=$(date +%s)
if ! (cd "$WARM" && "$DEST/bin/n00b" build --output "$WARM/warm" warm.n \
        > "$WARM/log" 2>&1); then
    cat "$WARM/log" >&2
    rm -rf "$WARM"
    die "the installed compiler could not build a test program"
fi
rm -rf "$WARM"
advance "prepared the standard library ($(( $(date +%s) - START ))s)"

"$DEST/bin/n00b" --version > /dev/null 2>&1 || die "the installed compiler does not run"
advance "verified"

printf '\nn00b %s is installed in %s\n' "$ID" "$(pretty "$DEST")"
if [ -n "$PREVIOUS" ] && [ "$PREVIOUS" != "$ID" ]; then
    printf 'It is now the active toolchain (it was %s; switch back with: n00b toolchain use %s).\n' \
        "$PREVIOUS" "$PREVIOUS"
else
    printf 'It is now the active toolchain.\n'
fi

REACHABLE=no
if on_path "$BIN" || [ -n "$EXTRA_LINK" ]; then
    REACHABLE=yes
fi

SHELL_NAME="$(basename "${SHELL:-sh}")"
RC="$(shell_rc)"
if [ "$SHELL_NAME" = fish ] || [ -d "$HOME/.config/fish" ]; then
    mkdir -p "$HOME/.config/fish/completions" &&
        ln -sfn "$STORE/current/share/completions/n00b.fish" \
            "$HOME/.config/fish/completions/n00b.fish" &&
        printf '%s\n' "$HOME/.config/fish/completions/n00b.fish" >> "$STORE/links"
fi

completion_line() {
    local file="$STORE/current/share/completions/n00b.$SHELL_NAME"
    case "$file" in
        "$HOME"/*) file="\$HOME/${file#"$HOME"/}" ;;
    esac
    printf '[ -f "%s" ] && . "%s" # added by n00b install\n' "$file" "$file"
}

has_line() {
    [ -f "$RC" ] && grep -qF "$1" "$RC"
}

WANTED=()
PURPOSES=()
if [ "$REACHABLE" = no ]; then
    if has_line "$(path_line "$BIN")"; then
        REACHABLE=pending
    else
        WANTED+=("$(path_line "$BIN")")
        PURPOSES+=("put $(pretty "$BIN") on your PATH")
    fi
fi
case "$SHELL_NAME" in
    bash|zsh)
        if ! has_line "$(completion_line)"; then
            WANTED+=("$(completion_line)")
            PURPOSES+=("turn on tab completion for n00b")
        fi ;;
esac

if [ "${#WANTED[@]}" -gt 0 ] && { [ "$PATH_CHOICE" = add ] ||
        { [ "$PATH_CHOICE" = ask ] && [ -t 0 ]; }; }; then
    ANSWER=y
    if [ "$PATH_CHOICE" = ask ]; then
        printf '\nTo %s,\n' "$(IFS=';'; printf '%s' "${PURPOSES[*]}" | sed 's/;/ and /g')"
        printf 'these lines can be appended to %s:\n\n' "$(pretty "$RC")"
        for line in "${WANTED[@]}"; do
            printf '    %s\n' "$line"
        done
        printf '\n'
        read -r -p "Add them? [Y/n] " ANSWER < /dev/tty || ANSWER=n
    fi
    case "$ANSWER" in
        ""|y|Y|yes|YES)
            mkdir -p "$(dirname "$RC")"
            printf '\n' >> "$RC"
            for line in "${WANTED[@]}"; do
                printf '%s\n' "$line" >> "$RC"
            done
            [ "$REACHABLE" = no ] && REACHABLE=pending
            printf 'Added them to %s.\n' "$(pretty "$RC")" ;;
    esac
fi

case "$REACHABLE" in
    yes)
        FIRST="$(first_on_path || true)"
        EXPECTED="$(resolved "$DEST/bin/n00b")"
        if [ -n "$FIRST" ] && [ "$(resolved "$FIRST")" != "$EXPECTED" ]; then
            boxed "Another n00b comes first on your PATH: $FIRST" \
                  "Running 'n00b' will use that one, not the one just installed." \
                  "Remove it, or put $(pretty "$BIN") earlier in your PATH."
        else
            printf 'n00b on your PATH: %s\n' "$(pretty "${EXTRA_LINK:-$BIN/n00b}")"
        fi ;;
    pending)
        printf 'Open a new terminal, or run this in the current one, to pick it up:\n\n'
        printf '    %s\n' "$(path_line "$BIN" | sed 's/ # added by n00b install//')" ;;
    no)
        boxed "$(pretty "$BIN") is not on your PATH, so 'n00b' will not run by name." \
              "Add this line to $(pretty "$(shell_rc)"):" \
              "" \
              "    $(path_line "$BIN" | sed 's/ # added by n00b install//')" \
              "" \
              "or rerun the install with --add-to-path." ;;
esac

printf '\nTry it:\n    n00b run hello.n\n    n00b env\n'
