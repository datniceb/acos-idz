#!/bin/bash
# Использование: ./run_shm.sh [поваров] [официантов]   (по умолчанию 4 и 3, как в сценарии 1)

cd "$(dirname "$0")" || exit 1
COOKS=${1:-4}
WAITERS=${2:-3}

for i in $(seq "$COOKS"); do ./shm_cook & done
for i in $(seq "$WAITERS"); do ./shm_waiter & done
./shm_door &
./shm_kitchen
wait
