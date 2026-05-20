#!/bin/sh
set -e

# If the first argument does not start with a '-' and is not 'machaudio',
# assume the user wants to run an arbitrary command (e.g., /bin/sh, ls, env).
# The ${1#-} syntax is POSIX compliant string manipulation that removes a leading dash.
if [ "${1#-}" = "$1" ] && [ "$1" != "machaudio" ]; then
    exec "$@"
fi

# If the user explicitly called 'machaudio' in their `docker run` command,
# remove it from the argument list so we don't run `machaudio machaudio...`
if [ "$1" = "machaudio" ]; then
    shift
fi

# Calculate default workers: max(1, nproc - 1)
CORES=$(nproc)
if [ "$CORES" -gt 1 ]; then
    WORKERS=$((CORES - 1))
else
    WORKERS=1
fi

# Check if the user already provided --workers or -w in the arguments
HAS_WORKERS=0
for arg in "$@"; do
    case "$arg" in
        --workers|-w)
            HAS_WORKERS=1
            break
            ;;
    esac
done

# If --workers wasn't explicitly overridden by the user, append our calculated default
if [ "$HAS_WORKERS" -eq 0 ]; then
    set -- "$@" --workers "$WORKERS"
    echo "[Docker] Automatically scaling to $WORKERS worker(s) based on $CORES available core(s)."
fi

# Execute the actual MachAudio binary with the final arguments
# Using `exec` ensures machaudio takes PID 1 and receives OS signals (like SIGTERM) properly.
exec machaudio "$@"
