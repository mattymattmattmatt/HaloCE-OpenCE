#!/usr/bin/env bash
# Uploads a title built by build.sh to the console over FTP (ftpsrv, port
# 2121), into /data/homebrew/<TITLE_ID>/, where ShadowMountPlus finds it:
#
#   port/ps5/probe/title/deploy.sh <console address> <TITLE_ID> [port]
#
# eboot.bin and param.json go last, so a title is never started half
# uploaded. Close the title on the console first.

set -euo pipefail

repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)
console=${1:?usage: deploy.sh <console address> <TITLE_ID> [port]}
title_id=${2:?usage: deploy.sh <console address> <TITLE_ID> [port]}
port=${3:-2121}
app="$repository/build/ps5/probe/$title_id"
[[ -f $app/eboot.bin ]] || { echo "$app is not built" >&2; exit 2; }

upload() {
    curl --silent --show-error --ftp-create-dirs -T "$app/$1" "ftp://$console:$port/data/homebrew/$title_id/$1"
    echo "uploaded $1"
}
while IFS= read -r -d '' file; do
    file=${file#"$app/"}
    [[ $file == eboot.bin || $file == sce_sys/param.json ]] || upload "$file"
done < <(find "$app" -type f -print0 | sort -z)
upload eboot.bin
upload sce_sys/param.json
echo "deployed $title_id to /data/homebrew/$title_id"
