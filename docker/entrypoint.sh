#!/bin/bash
set -euo pipefail

# At this point the process is running as a non-root user
echo "⚙️ Container started | Mode: Native Non-Root"
echo "👤 UID: $(id -u) | GID: $(id -g)"
echo "🏠 HOME: ${HOME}"

# Execute the Dockerfile CMD process
exec "$@"
