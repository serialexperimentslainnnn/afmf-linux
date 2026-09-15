#!/bin/bash
# (Direct interpreter path on purpose: the IDE's security guard flags `env bash` shebangs.)
#
# Creates the afmf-linux release signing key in the maintainer's keyring, has the maintainer's
# own keys certify it, and exports what the repository and CI need:
#   packaging/afmf-linux-release-key.asc   public key, with the certifications (committed)
#   <out>/afmf-linux-release-key.secret.asc private key, passphrase-protected (GitHub secret)
#
# The passphrase is asked by gpg's pinentry, never taken from the command line or a file.
# Certifying with a key that lives on a smartcard asks for that card's PIN; a signer that is
# not available (an offline root) is reported and skipped, run again with --certify later.
set -Eeuo pipefail
shopt -s inherit_errexit
IFS=$'\n\t'
SCRIPT_NAME="${0##*/}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly SCRIPT_NAME SCRIPT_DIR

: "${UID_NAME:=afmf-linux release signing}"
: "${UID_EMAIL:=lain@digitalexperiments.dev}"
: "${EXPIRE:=2y}"
: "${OUT_DIR:=${SCRIPT_DIR}/out}"
: "${REPO:=serialexperimentslainnnn/afmf-linux}"
: "${GH_ENV:=release}"
UPLOAD=0
# The maintainer's keys that certify the release key, in this order: root trust, intermediate
# trust, the YubiKey key that signs the commits. Public fingerprints.
readonly -a DEFAULT_SIGNERS=(
  E70A886589AB9AB9DC2D2CA3B746AD2C841D5CE3
  318BBEFF6E5DD5A03A8280518DAB773C3796B834
  B12DB7CFBAC52556672E9B24E2E4041CCF039102
)
PUBLIC_FILE="${SCRIPT_DIR}/afmf-linux-release-key.asc"
readonly PUBLIC_FILE

log() { printf '%(%FT%TZ)T [%s] %s: %s\n' -1 "$1" "$SCRIPT_NAME" "${*:2}" >&2; }
die() { log error "$*"; exit 1; }

usage() {
  cat <<'EOF'
Usage: packaging/release-key.sh
       packaging/release-key.sh [--upload] create [SIGNER_FPR ...]
       packaging/release-key.sh [--upload] certify KEY_FPR SIGNER_FPR [SIGNER_FPR ...]
       packaging/release-key.sh [--upload] export KEY_FPR
       packaging/release-key.sh upload KEY_FPR

With no arguments it does everything: create, certify with the maintainer's root, intermediate
and YubiKey keys, export, and upload to GitHub.

create    Generates an Ed25519 sign+certify key (pinentry asks for its passphrase), certifies it
          with every SIGNER_FPR given, then exports (see export).
certify   Certifies KEY_FPR with each SIGNER_FPR (gpg --quick-sign-key); skips signers that fail.
export    Writes the public key to packaging/afmf-linux-release-key.asc and the protected private
          key to $OUT_DIR (default packaging/out, git-ignored) for the CI secret.

upload    With gh (authenticated as the repository owner): creates the GitHub environment, stores
          the private key as RELEASE_GPG_KEY and the passphrase (asked here, hidden) as
          RELEASE_GPG_PASSPHRASE in it, adds the public key to your GitHub account so tags
          signed with it verify, and shreds the exported private key.
--upload  Runs upload at the end of create / certify / export.

Environment: UID_NAME, UID_EMAIL, EXPIRE (default 2y), OUT_DIR, REPO (owner/name), GH_ENV.
Exit codes: 0 done, 1 a step failed, 2 usage error.
EOF
}

is_fpr() { [[ $1 =~ ^[0-9A-Fa-f]{40}$ ]]; }

create_key() {
  command -v gpg >/dev/null || die "gpg not found"
  log info "generating: ${UID_NAME} <${UID_EMAIL}>, Ed25519, expires in ${EXPIRE}; pinentry will ask for the passphrase"
  local status fpr
  status="$(gpg --batch --status-fd 1 --gen-key <<EOF
%ask-passphrase
Key-Type: eddsa
Key-Curve: ed25519
Key-Usage: sign,cert
Name-Real: ${UID_NAME}
Name-Email: ${UID_EMAIL}
Name-Comment: package and tarball signatures only, not the maintainer's key
Expire-Date: ${EXPIRE}
%commit
EOF
)" || die "key generation failed"
  fpr="$(printf '%s\n' "$status" | sed -n 's/^\[GNUPG:\] KEY_CREATED [PBS] \([0-9A-F]\{40\}\).*/\1/p' | head -n1)"
  is_fpr "$fpr" || die "could not read the new key's fingerprint from gpg's status output"
  log info "created ${fpr}"
  printf '%s\n' "$fpr"
}

certify() {
  local key=$1 signer failed=0
  shift
  for signer in "$@"; do
    is_fpr "$signer" || die "not a fingerprint: ${signer}"
    log info "certifying ${key} with ${signer}"
    if gpg --default-key "$signer" --quick-sign-key "$key"; then
      log info "certified by ${signer}"
    else
      log warn "signer ${signer} could not certify (offline key, wrong PIN, cancelled); skipped"
      failed=1
    fi
  done
  return "$failed"
}

export_key() {
  local key=$1
  is_fpr "$key" || die "not a fingerprint: ${key}"
  mkdir -p -- "$OUT_DIR"
  chmod 700 -- "$OUT_DIR"
  gpg --armor --export "$key" >"$PUBLIC_FILE" || die "public export failed"
  local secret="${OUT_DIR}/afmf-linux-release-key.secret.asc"
  (umask 077 && gpg --armor --export-secret-keys "$key" >"$secret") || die "secret export failed"
  log info "public key: ${PUBLIC_FILE} (commit it)"
  log info "private key, passphrase-protected: ${secret} (do NOT commit; upload as the GitHub secret)"
  printf '\nFingerprint: %s\n' "$key"
  if [[ $UPLOAD -eq 1 ]]; then
    upload_key "$key"
  else
    cat <<EOF
To store it in GitHub (secrets in the '${GH_ENV}' environment of ${REPO}, public key on your account):
  ${SCRIPT_NAME} upload ${key}
EOF
  fi
}

upload_key() {
  local key=$1 secret="${OUT_DIR}/afmf-linux-release-key.secret.asc" passphrase
  command -v gh >/dev/null || die "gh not found"
  gh auth status >/dev/null 2>&1 || die "gh is not authenticated (gh auth login)"
  [[ -s $secret ]] || die "no exported private key at ${secret}; run export first"

  log info "creating environment '${GH_ENV}' in ${REPO} (no-op if it exists)"
  gh api --method PUT "repos/${REPO}/environments/${GH_ENV}" >/dev/null || die "could not create the environment"

  log info "storing RELEASE_GPG_KEY"
  gh secret set RELEASE_GPG_KEY --repo "$REPO" --env "$GH_ENV" <"$secret" || die "secret upload failed"

  read -r -s -p "Passphrase of ${key} (stored as RELEASE_GPG_PASSPHRASE, not echoed): " passphrase
  printf '\n'
  [[ -n $passphrase ]] || die "empty passphrase"
  printf '%s' "$passphrase" | gh secret set RELEASE_GPG_PASSPHRASE --repo "$REPO" --env "$GH_ENV" || die "passphrase upload failed"
  unset passphrase

  log info "adding the public key to your GitHub account (signatures made with it show as verified)"
  if ! gh gpg-key add "$PUBLIC_FILE" 2>/dev/null; then
    log warn "gh gpg-key add failed or the key is already there; add ${PUBLIC_FILE} at github.com/settings/keys if needed"
  fi

  shred -u -- "$secret" && log info "exported private key shredded; it now lives only in your keyring and the GitHub secret"
}

main() {
  if [[ $# -eq 0 ]]; then
    UPLOAD=1
    set -- create "${DEFAULT_SIGNERS[@]}"
  fi
  if [[ ${1:-} == --upload ]]; then
    UPLOAD=1
    shift
  fi
  [[ $# -ge 1 ]] || { usage >&2; exit 2; }
  case "$1" in
    -h|--help) usage; exit 0 ;;
    upload)
      [[ $# -eq 2 ]] || { usage >&2; exit 2; }
      is_fpr "$2" || die "not a fingerprint: $2"
      upload_key "$2"
      ;;
    create)
      shift
      local fpr
      fpr="$(create_key)"
      if [[ $# -gt 0 ]]; then
        certify "$fpr" "$@" || log warn "some certifications are missing; rerun: ${SCRIPT_NAME} certify ${fpr} <SIGNER_FPR ...>"
      fi
      export_key "$fpr"
      ;;
    certify)
      [[ $# -ge 3 ]] || { usage >&2; exit 2; }
      is_fpr "$2" || die "not a fingerprint: $2"
      certify "${@:2}" || exit 1
      export_key "$2"
      ;;
    export)
      [[ $# -eq 2 ]] || { usage >&2; exit 2; }
      export_key "$2"
      ;;
    *) usage >&2; exit 2 ;;
  esac
}
main "$@"
