# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

# Report the current directory only in Pretty/Shitty sessions.
[[ -n "${SHITTY_VERSION:-}${PRETTY_VERSION:-}" ]] || return 0

function _shitty_directory {
    local LC_ALL=C character encoded encodedDirectory=''
    for character in ${(s::)PWD}; do
        case "$character" in
            [a-zA-Z0-9/._~-]) encodedDirectory+="$character" ;;
            *) printf -v encoded '%%%02X' "'$character"; encodedDirectory+="$encoded" ;;
        esac
    done
    printf '\e]7;file://localhost%s\e\\' "$encodedDirectory"
}

autoload -Uz add-zsh-hook
add-zsh-hook chpwd _shitty_directory
add-zsh-hook precmd _shitty_directory
