#!/usr/bin/env bash
# Builds romfs/web/editor.js.gz, the code editor 3ds.local loads, from
# resources/editor: CodeMirror at the versions in its package-lock.json,
# bundled by esbuild and gzipped. Needs Node.js with npm.

set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="${ROOT}/resources/editor"
OUT="${ROOT}/romfs/web/editor.js.gz"

command -v npm >/dev/null 2>&1 || { echo "Node.js with npm is required." >&2; exit 1; }

cd "${SOURCE}"
npm ci --no-audit --no-fund --ignore-scripts
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# Every bundled package is Marijn Haverbeke's, under this MIT license.
LICENSE="$(cat node_modules/@codemirror/view/LICENSE)"
node_modules/.bin/esbuild editor.js --bundle --minify --format=iife \
    --global-name=CodeEditor --target=es2020 --log-level=warning \
    "--banner:js=/*! CodeMirror (codemirror.net), Lezer, style-mod, w3c-keyname, crelt, find-cluster-break
${LICENSE} */" \
    --outfile="${WORK}/editor.js"

mkdir -p "$(dirname "${OUT}")"
gzip -9 -n -c "${WORK}/editor.js" > "${OUT}"
printf 'Built %s: %s bytes, %s gzipped\n' "${OUT#"${ROOT}/"}" \
    "$(wc -c < "${WORK}/editor.js" | tr -d ' ')" "$(wc -c < "${OUT}" | tr -d ' ')"
