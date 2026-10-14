#!/bin/bash
# packaging/maven-central.sh: the Android library to Maven Central, through
# the API of the Central Portal (central.sonatype.com).
#
#   maven-central.sh bundle <repository> <bundle.zip>
#       the signed Maven repository that
#       `./gradlew :libbdf:publishReleasePublicationToBuildRepository` writes
#       (android/libbdf/build/repo, signed when the Gradle properties
#       signingKey and signingPassword are given), checked and zipped as the
#       Portal takes a bundle
#   maven-central.sh upload <bundle.zip>
#       uploads it, and waits until the Portal has validated it.
#       $CENTRAL_USERNAME and $CENTRAL_PASSWORD are a user token of the
#       Portal. $CENTRAL_PUBLISHING is USER_MANAGED (the default: the
#       deployment waits for its Publish button) or AUTOMATIC.
set -euo pipefail

api=https://central.sonatype.com/api/v1/publisher

bundle() {
  local repo=$1 out=$2 bad=0
  repo=$(cd "$repo" && pwd)
  # every file of the component has its signature and its checksums
  while IFS= read -r f; do
    for ext in asc md5 sha1; do
      if [ ! -s "$f.$ext" ]; then
        echo "maven-central: ${f#"$repo"/} has no .$ext" >&2
        bad=1
      fi
    done
  done < <(find "$repo" -type f \( -name '*.aar' -o -name '*.jar' -o -name '*.pom' -o -name '*.module' \) | sort)
  if ! find "$repo" -name '*.pom' | grep -q .; then
    echo "maven-central: no POM in $repo" >&2
    bad=1
  fi
  [ $bad = 0 ] || exit 1
  rm -f "$out"
  # (the Portal makes the maven-metadata.xml of a release itself)
  (cd "$repo" && find . -type f ! -name 'maven-metadata.xml*' | sed 's|^\./||' | sort | zip -q "$out" -@)
  unzip -Z1 "$out"
}

upload() {
  local file=$1 token id state status
  : "${CENTRAL_USERNAME:?a user token of the Central Portal: its user name}"
  : "${CENTRAL_PASSWORD:?a user token of the Central Portal: its password}"
  token=$(printf '%s:%s' "$CENTRAL_USERNAME" "$CENTRAL_PASSWORD" | base64 | tr -d '\n')
  id=$(curl --fail-with-body -sS -X POST -H "Authorization: Bearer $token" \
    -F "bundle=@$file" "$api/upload?name=$(basename "$file" .zip)&publishingType=${CENTRAL_PUBLISHING:-USER_MANAGED}")
  echo "deployment $id"
  for _ in $(seq 1 90); do
    status=$(curl --fail-with-body -sS -X POST -H "Authorization: Bearer $token" "$api/status?id=$id")
    state=$(printf '%s' "$status" | python3 -c 'import json, sys; print(json.load(sys.stdin)["deploymentState"])')
    echo "$state"
    case "$state" in
    VALIDATED | PUBLISHING | PUBLISHED) return 0 ;;
    FAILED)
      printf '%s\n' "$status" >&2
      return 1
      ;;
    esac
    sleep 10
  done
  echo "maven-central: still $state after fifteen minutes" >&2
  return 1
}

case "${1:-}" in
bundle) bundle "${2:?the repository}" "${3:?the bundle to write}" ;;
upload) upload "${2:?the bundle}" ;;
*)
  echo "usage: maven-central.sh bundle <repository> <bundle.zip> | upload <bundle.zip>" >&2
  exit 2
  ;;
esac
