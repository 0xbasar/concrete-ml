#!/bin/bash

# Run benchmarks while logging the intermediate results
# Publish findings in the progress tracker

set -e

# shellcheck disable=SC1091
if [ -f .env ]
then
    # shellcheck disable=SC1091
    set -a; source .env; set +a
fi

DEV_VENV_PATH="/home/dev_user/dev_venv"

# shellcheck disable=SC1090,SC1091
if ! source "${DEV_VENV_PATH}/bin/activate"; then
    python3 -m venv "${DEV_VENV_PATH}"
    # shellcheck disable=SC1090,SC1091
    source "${DEV_VENV_PATH}/bin/activate"
fi

cd /src/ && make setup_env

mkdir -p /tmp/keycache
mkdir -p logs

initial_ml_log=logs/$(date -u --iso-8601=seconds).ml.log
make -s benchmark 2>&1 | tee -a "$initial_ml_log"

final_ml_log=logs/$(date -u --iso-8601=seconds).ml.log
cat -s "$initial_ml_log" | sed '1d; $d' > "$final_ml_log"

# sed above removes the first and the last lines of the log
# which are empty to provide a nice console output
# but empty lines are useless for logs so we get rid of them

rm "$initial_ml_log"
cp "$final_ml_log" logs/latest.ml.log

curl \
     -H 'Authorization: Bearer '"$ML_PROGRESS_TRACKER_TOKEN"'' \
     -H 'Content-Type: application/json' \
     -d @progress.json \
     -X POST "$ML_PROGRESS_TRACKER_URL"/measurement
