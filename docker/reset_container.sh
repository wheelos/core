#!/bin/bash
set -euo pipefail

BLUE='\033[1;34m'
GREEN='\033[1;32m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${BLUE}🛠 Initializing development environment...${NC}"

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT_DIR="${DIR}/.."
cd "${ROOT_DIR}"

# user info
USER_NAME=$(id -un)
USER_UID=$(id -u)
USER_GID=$(id -g)

PASSWD_FILE="/tmp/wheelos-docker-passwd-${USER_NAME}"
GROUP_FILE="/tmp/wheelos-docker-group-${USER_NAME}"

cat <<EOF > .env
HOST_USER=${USER_NAME}
HOST_UID=${USER_UID}
HOST_GID=${USER_GID}
BAZEL_CACHE=${HOME}/.cache/bazel-apollo-docker
PASSWD_FILE=${PASSWD_FILE}
GROUP_FILE=${GROUP_FILE}
EOF
echo -e "✅ .env configuration file generated"

# 2. host directory setup for bazel cache and container home
mkdir -p "${HOME}/.cache/bazel-apollo-docker"
mkdir -p "${HOME}/.apollo_container_home"

# 3. dynamically generate passwd/group files
cat > "${PASSWD_FILE}" <<EOF
root:x:0:0:root:/root:/bin/bash
${USER_NAME}:x:${USER_UID}:${USER_GID}:${USER_NAME}:/home/${USER_NAME}:/bin/bash
EOF
cat > "${GROUP_FILE}" <<EOF
root:x:0:
${USER_NAME}:x:${USER_GID}:
EOF

echo -e "${BLUE}🔄 Cleaning and redeploying containers...${NC}"
docker compose --env-file .env -f docker/docker-compose.yml down --remove-orphans >/dev/null 2>&1
docker compose --env-file .env -f docker/docker-compose.yml up -d --build

if [ "$(docker compose --env-file .env -f docker/docker-compose.yml ps -q cyber-dev)" ]; then
    echo -e "------------------------------------------------"
    echo -e "${GREEN}🚀 Container updated and started successfully!${NC}"
    echo -e "👉 Run ${BLUE}bash docker/enter_container.sh${NC} to enter the environment"
    echo -e "------------------------------------------------"
else
    echo -e "${RED}❌ Container failed to start; check 'docker compose logs' for details.${NC}"
    exit 1
fi
