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

# Build default core mask (CSV format)
CORE_MASK="0"
i=1
while [ "$i" -lt "$WORKERS" ]; do
    CORE_MASK="$CORE_MASK,$i"
    i=$((i + 1))
done

# Check if the user already provided --core-mask or -c in the arguments
HAS_MASK=0
for arg in "$@"; do
    case "$arg" in
        --core-mask|-c)
            HAS_MASK=1
            break
            ;;
    esac
done

# If --core-mask wasn't explicitly overridden by the user, append our calculated default
if [ "$HAS_MASK" -eq 0 ]; then
    set -- "$@" --core-mask "$CORE_MASK"
    echo "[Docker] Automatically scaling to $WORKERS worker(s) using cores: $CORE_MASK"
fi

# Execute the actual MachAudio binary with the final arguments
# Using `exec` ensures machaudio takes PID 1 and receives OS signals (like SIGTERM) properly.
exec machaudio "$@"
