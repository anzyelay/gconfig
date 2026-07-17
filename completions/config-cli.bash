# Bash completion for config-cli
# Source this file or install to /usr/share/bash-completion/completions/

_config_cli() {
    local cur prev words cword
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"
    cword=$COMP_CWORD
    words=("${COMP_WORDS[@]}")

    local socket_path="/tmp/configd.sock"

    local i
    for ((i = 1; i < cword; i++)); do
        if [[ "${words[i]}" == "-s" && $i -lt ${#words[@]} ]]; then
            socket_path="${words[i+1]}"
            break
        fi
    done

    case $prev in
        -s)
            compgen -f -- "$cur" | while read -r f; do
                COMPREPLY+=("$f")
            done
            return
            ;;
        -t)
            COMPREPLY=($(compgen -W "int double string boolean" -- "$cur"))
            return
            ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=($(compgen -W "-s -t -h --help" -- "$cur"))
        return
    fi

    local cmd=""
    local cmd_index=0
    for ((i = 1; i < cword; i++)); do
        case "${words[i]}" in
            -s) ((i++)) ;;
            -t) ((i++)) ;;
            -h|--help) ;;
            *)
                if [[ -z "$cmd" ]]; then
                    cmd="${words[i]}"
                    cmd_index=$i
                fi
                ;;
        esac
    done

    if [[ -z "$cmd" ]]; then
        COMPREPLY=($(compgen -W "get set reset reset-all list-keys list-schemas monitor help" -- "$cur"))
        return
    fi

    local arg_after_cmd=""
    for ((i = cmd_index + 1; i < cword; i++)); do
        case "${words[i]}" in
            -s) ((i++)) ;;
            -t) ((i++)) ;;
            *)
                if [[ -z "$arg_after_cmd" ]]; then
                    arg_after_cmd="${words[i]}"
                else
                    arg_after_cmd="done"
                fi
                ;;
        esac
    done

    case $cmd in
        get|reset)
            if [[ -z "$arg_after_cmd" ]]; then
                _config_cli_schemas "$socket_path"
            elif [[ "$arg_after_cmd" != "done" ]]; then
                _config_cli_keys "$socket_path" "$arg_after_cmd"
            fi
            ;;
        set)
            if [[ -z "$arg_after_cmd" ]]; then
                _config_cli_schemas "$socket_path"
            elif [[ "$arg_after_cmd" != "done" ]]; then
                _config_cli_keys "$socket_path" "$arg_after_cmd"
            fi
            ;;
        reset-all|list-keys|monitor)
            if [[ -z "$arg_after_cmd" ]]; then
                _config_cli_schemas "$socket_path"
            fi
            if [[ "$cmd" == "monitor" && "$arg_after_cmd" != "done" && -n "$arg_after_cmd" ]]; then
                _config_cli_keys "$socket_path" "$arg_after_cmd"
            fi
            ;;
    esac
}

_config_cli_schemas() {
    local socket="$1"
    local cli
    cli=$(type -p config-cli 2>/dev/null || echo "config-cli")
    local schemas
    schemas=$("$cli" -s "$socket" list-schemas 2>/dev/null)
    if [[ -n "$schemas" ]]; then
        COMPREPLY=($(compgen -W "$schemas" -- "$cur"))
    fi
}

_config_cli_keys() {
    local socket="$1" schema="$2"
    local cli
    cli=$(type -p config-cli 2>/dev/null || echo "config-cli")
    local keys
    keys=$("$cli" -s "$socket" list-keys "$schema" 2>/dev/null | awk '{print $1}')
    if [[ -n "$keys" ]]; then
        COMPREPLY=($(compgen -W "$keys" -- "$cur"))
    fi
}

complete -F _config_cli config-cli
