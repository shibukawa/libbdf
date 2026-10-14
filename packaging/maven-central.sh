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
#   maven-central.sh upload [--validate-only] <bundle.zip>
#       uploads it and publishes it once the Portal has validated it;
#       --validate-only stops after the Portal's checks (namespace, POM,
#       signatures) and drops the deployment. $CENTRAL_USERNAME and
#       $CENTRAL_PASSWORD are a user token of the Portal.
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
  local validate_only=false
  if [ "$1" = --validate-only ]; then
    validate_only=true
    shift
  fi
  local file=${1:?the bundle} token auth id state status type=AUTOMATIC
  : "${CENTRAL_USERNAME:?a user token of the Central Portal: its user name}"
  : "${CENTRAL_PASSWORD:?a user token of the Central Portal: its password}"
  token=$(printf '%s:%s' "$CENTRAL_USERNAME" "$CENTRAL_PASSWORD" | base64 | tr -d '\n')
  auth="Authorization: Bearer $token"
  if $validate_only; then type=USER_MANAGED; fi
  id=$(curl --fail-with-body -sS -X POST -H "$auth" \
    -F "bundle=@$file" "$api/upload?name=$(basename "$file" .zip)&publishingType=$type")
  echo "deployment $id ($type)"
  for _ in $(seq 1 90); do
    status=$(curl --fail-with-body -sS -X POST -H "$auth" "$api/status?id=$id")
    state=$(printf '%s' "$status" | python3 -c 'import json, sys; print(json.load(sys.stdin)["deploymentState"])')
    echo "$state"
    case "$state" in
    VALIDATED)
      if $validate_only; then
        curl --fail-with-body -sS -X DELETE -H "$auth" "$api/deployment/$id"
        echo "validated; the deployment is dropped"
        return 0
      fi
      ;;
    PUBLISHED) return 0 ;;
    FAILED)
      printf '%s\n' "$status" | python3 -m json.tool >&2
      if $validate_only; then
        curl --fail-with-body -sS -X DELETE -H "$auth" "$api/deployment/$id" || true
      fi
      return 1
      ;;
    esac
    sleep 10
  done
  echo "maven-central: deployment $id is still $state after fifteen minutes" >&2
  return 1
}

case "${1:-}" in
bundle) bundle "${2:?the repository}" "${3:?the bundle to write}" ;;
upload)
  shift
  upload "$@"
  ;;
*)
  echo "usage: maven-central.sh bundle <repository> <bundle.zip> | upload [--validate-only] <bundle.zip>" >&2
  exit 2
  ;;
esac
