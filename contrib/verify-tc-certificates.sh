#!/usr/bin/env bash

# Read-only, generation-aware certificate diagnostics for TeddyCloud.
# This script deliberately writes only normalized OpenSSL input into a private
# temporary directory. It never changes certificate files or configuration.

set -u

BASE_PATH="${TEDDYCLOUD_BASE_PATH:-/teddycloud}"
USE_COLOR=1
VERBOSE=0
JSON_MODE=0
WARN_DAYS=30
WARN_SECONDS=$((WARN_DAYS * 24 * 60 * 60))

usage() {
    cat <<'EOF'
Usage: verify-tc-certificates.sh [--base-path DIR] [--verbose] [--json] [--no-color] [--help]

Read-only validation of TeddyCloud TB1/TB2 server, upstream client and local
fake-client certificates, including their effective overlay configuration.

Options:
  --base-path DIR  TeddyCloud base directory (default: /teddycloud)
  --verbose        Show every individual cryptographic check
  --json           Emit the complete machine-readable diagnosis as JSON
  --no-color       Disable ANSI colors (NO_COLOR is also honored)
  --help           Show this help

Exit codes:
  0  no warnings or errors
  1  warnings, but no errors
  2  at least one certificate or configuration error
  3  missing prerequisite or unreadable configuration
EOF
}

while (($#)); do
    case "$1" in
        --base-path)
            if (($# < 2)) || [[ -z "$2" ]]; then
                printf 'ERROR: --base-path requires a directory\n' >&2
                exit 3
            fi
            BASE_PATH="$2"
            shift 2
            ;;
        --no-color)
            USE_COLOR=0
            shift
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
        --json)
            JSON_MODE=1
            USE_COLOR=0
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            printf 'ERROR: unknown option: %s\n' "$1" >&2
            usage >&2
            exit 3
            ;;
    esac
done

if [[ -n "${NO_COLOR:-}" ]] || [[ ! -t 1 ]]; then
    USE_COLOR=0
fi

if ((BASH_VERSINFO[0] < 4)); then
    printf 'ERROR: Bash 4 or newer is required.\n' >&2
    exit 3
fi
if ! command -v openssl >/dev/null 2>&1; then
    printf 'ERROR: OpenSSL is required. Install it or use a current official TeddyCloud image.\n' >&2
    exit 3
fi
if ! command -v find >/dev/null 2>&1 || ! command -v cmp >/dev/null 2>&1; then
    printf 'ERROR: find and cmp are required by the certificate doctor.\n' >&2
    exit 3
fi

if [[ ! -d "$BASE_PATH" ]]; then
    printf 'ERROR: TeddyCloud base path is not a directory: %s\n' "$BASE_PATH" >&2
    exit 3
fi

CONFIG_FILE="$BASE_PATH/config/config.ini"
OVERLAY_FILE="$BASE_PATH/config/config.overlay.ini"
CERT_ROOT="$BASE_PATH/certs"

if [[ ! -r "$CONFIG_FILE" ]]; then
    printf 'ERROR: TeddyCloud configuration is not readable: %s\n' "$CONFIG_FILE" >&2
    exit 3
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tc-cert-doctor.XXXXXX")" || exit 3
chmod 700 "$TMP_DIR"
cleanup() {
    rm -rf -- "$TMP_DIR"
}
trap cleanup EXIT HUP INT TERM

if ((USE_COLOR)); then
    C_OK=$'\033[32m'
    C_WARN=$'\033[33m'
    C_ERROR=$'\033[31m'
    C_INFO=$'\033[36m'
    C_RESET=$'\033[0m'
else
    C_OK=''
    C_WARN=''
    C_ERROR=''
    C_INFO=''
    C_RESET=''
fi

OK_COUNT=0
WARN_COUNT=0
ERROR_COUNT=0
INFO_COUNT=0
ROLE_OK_COUNT=0
ROLE_WARN_COUNT=0
ROLE_ERROR_COUNT=0
ROLE_INFO_COUNT=0

declare -a COMPACT_FINDINGS=()
declare -A COMPACT_FINDING_SEEN=()
declare -a CHECK_LEVELS=()
declare -a CHECK_MESSAGES=()
declare -a ROLE_IDS=()
declare -a ROLE_LABELS=()
declare -a ROLE_ACTIVE=()
declare -a ROLE_STATUS=()
declare -a ROLE_GENERATION=()
declare -a ROLE_CN=()
declare -a ROLE_PATH=()
declare -a OVERLAY_KEYS=()
declare -a OVERLAY_BOX_IDS=()
declare -a OVERLAY_CONFIGURED_GENERATION=()
declare -a OVERLAY_SELECTED_GENERATION=()
declare -a OVERLAY_IDENTITY_SOURCE=()
declare -a OVERLAY_STATUS=()
declare -a OVERLAY_CN=()
declare -a OVERLAY_PATH=()

normalize_severity() {
    case "${1^^}" in
        OK) printf 'ok' ;;
        WARN|WARNING) printf 'warning' ;;
        ERROR) printf 'error' ;;
        *) printf 'info' ;;
    esac
}

print_status() {
    local level="$1"
    shift
    local color="$C_INFO"
    case "$level" in
        OK) color="$C_OK" ;;
        WARN) color="$C_WARN" ;;
        ERROR) color="$C_ERROR" ;;
    esac
    printf '  %b%-5s%b %s\n' "$color" "$level" "$C_RESET" "$*"
}

report() {
    local level="$1"
    shift
    local color="$C_INFO"
    case "$level" in
        OK) color="$C_OK"; OK_COUNT=$((OK_COUNT + 1)) ;;
        WARN) color="$C_WARN"; WARN_COUNT=$((WARN_COUNT + 1)) ;;
        ERROR) color="$C_ERROR"; ERROR_COUNT=$((ERROR_COUNT + 1)) ;;
        INFO) INFO_COUNT=$((INFO_COUNT + 1)) ;;
    esac
    if ((JSON_MODE)); then
        CHECK_LEVELS+=("$level")
        CHECK_MESSAGES+=("$*")
    fi
    if ((VERBOSE && !JSON_MODE)); then
        printf '  %b%-5s%b %s\n' "$color" "$level" "$C_RESET" "$*"
    elif [[ "$level" == WARN || "$level" == ERROR ]]; then
        local finding_key="$level|$*"
        if [[ -z "${COMPACT_FINDING_SEEN["$finding_key"]:-}" ]]; then
            COMPACT_FINDING_SEEN["$finding_key"]=1
            COMPACT_FINDINGS+=("$finding_key")
        fi
    fi
}

report_detail() {
    local level="$1"
    shift
    if ((JSON_MODE)); then
        CHECK_LEVELS+=("$level")
        CHECK_MESSAGES+=("$*")
    elif ((VERBOSE)); then
        report "$level" "$*"
    fi
}

issue() {
    local active="$1"
    shift
    if ((active)); then
        report ERROR "$*"
    else
        report WARN "$* (role disabled)"
    fi
}

section() {
    ((VERBOSE && !JSON_MODE)) || return 0
    printf '\n%s\n' "$1"
    printf '%*s\n' "${#1}" '' | tr ' ' '-'
}

compact_section() {
    ((VERBOSE || JSON_MODE)) && return 0
    printf '\n%s\n' "$1"
    printf '%*s\n' "${#1}" '' | tr ' ' '-'
}

compact_result() {
    local level="$1"
    shift
    case "$level" in
        OK) ROLE_OK_COUNT=$((ROLE_OK_COUNT + 1)) ;;
        WARN) ROLE_WARN_COUNT=$((ROLE_WARN_COUNT + 1)) ;;
        ERROR) ROLE_ERROR_COUNT=$((ROLE_ERROR_COUNT + 1)) ;;
        INFO) ROLE_INFO_COUNT=$((ROLE_INFO_COUNT + 1)) ;;
    esac
    ((VERBOSE || JSON_MODE)) || print_status "$level" "$*"
}

declare -A DEFAULTS=()
declare -A GLOBAL=()
declare -A GLOBAL_PRESENT=()
declare -A OVERLAY=()
declare -A OVERLAY_PRESENT=()
declare -A OVERLAY_IDS=()
declare -A CERT_IDENTITY_PATH=()
declare -A KEY_IDENTITY_PATH=()
declare -A SET_GENERATION=()
declare -A SET_CN=()
declare -A SET_COMPLETE=()
declare -A SET_WARN_START=()
declare -A SET_ERROR_START=()
declare -A SET_RESULT=()

update_set_result() {
    local set_id="$1"
    if ((ERROR_COUNT > ${SET_ERROR_START["$set_id"]:-0})); then
        SET_RESULT["$set_id"]=ERROR
    elif ((WARN_COUNT > ${SET_WARN_START["$set_id"]:-0})); then
        SET_RESULT["$set_id"]=WARN
    else
        SET_RESULT["$set_id"]=OK
    fi
}

compact_set_result() {
    local set_id="$1"
    local role="$2"
    local active="$3"
    local path="$4"
    local result="${SET_RESULT["$set_id"]:-INFO}"
    local generation="${SET_GENERATION["$set_id"]:-UNKNOWN}"
    local cn="${SET_CN["$set_id"]:-}"
    local state=disabled
    ((active)) && state=active
    if [[ "${SET_COMPLETE["$set_id"]:-0}" == 0 && "$result" == OK ]]; then
        result=INFO
    fi
    local details="$state, generation=$generation"
    [[ -n "$cn" ]] && details="$details, CN=$cn"
    [[ -n "$path" ]] && details="$details, $path"
    compact_result "$result" "$role - $details"
    if [[ "$set_id" != overlay:* ]]; then
        ROLE_IDS+=("$set_id")
        ROLE_LABELS+=("$role")
        ROLE_ACTIVE+=("$active")
        ROLE_STATUS+=("$(normalize_severity "$result")")
        ROLE_GENERATION+=("$generation")
        ROLE_CN+=("$cn")
        ROLE_PATH+=("$path")
    fi
}

record_role() {
    local id="$1"
    local label="$2"
    local active="$3"
    local status="$4"
    local generation="$5"
    local cn="$6"
    local path="$7"
    ROLE_IDS+=("$id")
    ROLE_LABELS+=("$label")
    ROLE_ACTIVE+=("$active")
    ROLE_STATUS+=("$(normalize_severity "$status")")
    ROLE_GENERATION+=("$generation")
    ROLE_CN+=("$cn")
    ROLE_PATH+=("$path")
}

record_overlay() {
    OVERLAY_KEYS+=("$1")
    OVERLAY_BOX_IDS+=("$2")
    OVERLAY_CONFIGURED_GENERATION+=("$3")
    OVERLAY_SELECTED_GENERATION+=("$4")
    OVERLAY_IDENTITY_SOURCE+=("$5")
    OVERLAY_STATUS+=("$(normalize_severity "$6")")
    OVERLAY_CN+=("$7")
    OVERLAY_PATH+=("$8")
}

json_escape() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    value="${value//$'\n'/\\n}"
    value="${value//$'\r'/\\r}"
    value="${value//$'\t'/\\t}"
    printf '%s' "$value"
}

json_string() {
    printf '"'
    json_escape "$1"
    printf '"'
}

json_bool() {
    if (($1)); then
        printf true
    else
        printf false
    fi
}

diagnostic_scope() {
    local message="$1"
    if [[ "$message" == *": "* ]]; then
        printf '%s' "${message%%: *}"
    elif [[ "$message" == Legacy\ */* ]]; then
        local legacy_path="${message#Legacy }"
        printf 'Legacy %s' "${legacy_path%%/*}"
    else
        printf 'General'
    fi
}

diagnostic_path() {
    local message="$1"
    local scope="$2"
    local candidate=''
    local token=''
    local index=0

    for token in $message; do
        token="${token#\(}"
        token="${token#\[}"
        token="${token#\"}"
        token="${token#CA=}"
        token="${token#cert=}"
        token="${token#key=}"
        case "$token" in
            /teddycloud/*|certs/*|config/*)
                candidate="$token"
                ;;
            client/*|client_tb1/*|client_tb2/*|server/*|server_tb1/*|server_tb2/*|.client.bak*|.server.bak*)
                candidate="certs/$token"
                ;;
        esac
        if [[ -n "$candidate" ]]; then
            while [[ -n "$candidate" ]]; do
                case "${candidate: -1}" in
                    ','|';'|':'|')'|']'|'"') candidate="${candidate%?}" ;;
                    *) break ;;
                esac
            done
            printf '%s' "$candidate"
            return 0
        fi
    done

    if [[ "$scope" == Inventory\ * ]]; then
        printf 'certs/%s' "${scope#Inventory }"
    elif [[ "$scope" == Legacy\ * ]]; then
        printf 'certs/%s' "${scope#Legacy }"
    elif [[ "$scope" == server_tb1 || "$scope" == server_tb2 || "$scope" == client_tb1 || "$scope" == client_tb2 ]]; then
        printf 'certs/%s' "$scope"
    else
        for ((index = 0; index < ${#ROLE_LABELS[@]}; index++)); do
            if [[ "${ROLE_LABELS[$index]}" == "$scope" && -n "${ROLE_PATH[$index]}" ]]; then
                printf '%s' "${ROLE_PATH[$index]}"
                return 0
            fi
        done
        for ((index = 0; index < ${#OVERLAY_KEYS[@]}; index++)); do
            if [[ "Overlay ${OVERLAY_KEYS[$index]}" == "$scope" && -n "${OVERLAY_PATH[$index]}" ]]; then
                printf '%s' "${OVERLAY_PATH[$index]}"
                return 0
            fi
        done
    fi
}

set_default() {
    DEFAULTS["$1"]="$2"
}

# Defaults mirror src/settings.c. Keep this list explicit so drift is visible
# in the contract test instead of being silently guessed at runtime.
set_default commonName default
set_default toniebox.boxGeneration 0
set_default cloud.enabled false
set_default cloud.tb2_enabled false
set_default cloud.tb2_v3_enabled true
set_default mqtt_server.enabled false
set_default mqtt_client_upstream.enabled false
set_default core.server_cert.file.ca certs/server_tb1/ca-root.pem
set_default core.server_cert.file.ca_der certs/server_tb1/ca.der
set_default core.server_cert.file.ca_key certs/server_tb1/ca-key.pem
set_default core.server_cert.file.crt certs/server_tb1/teddy-cert.pem
set_default core.server_cert.file.key certs/server_tb1/teddy-key.pem
set_default core.server_cert.data.ca ''
set_default core.server_cert.data.ca_key ''
set_default core.server_cert.data.crt ''
set_default core.server_cert.data.key ''
set_default core.server_cert_tb2.hostname tbs2.tonie.cloud
set_default core.server_cert_tb2.file.ca certs/server_tb2/ca-root.pem
set_default core.server_cert_tb2.file.ca_der certs/server_tb2/ca.der
set_default core.server_cert_tb2.file.ca_key certs/server_tb2/ca-key.pem
set_default core.server_cert_tb2.file.crt certs/server_tb2/teddy-cert.pem
set_default core.server_cert_tb2.file.key certs/server_tb2/teddy-key.pem
set_default core.server_cert_tb2.data.ca ''
set_default core.server_cert_tb2.data.ca_key ''
set_default core.server_cert_tb2.data.crt ''
set_default core.server_cert_tb2.data.key ''
set_default core.client_cert_tb1.file.ca certs/client_tb1/ca.der
set_default core.client_cert_tb1.file.crt certs/client_tb1/client.der
set_default core.client_cert_tb1.file.key certs/client_tb1/private.der
set_default core.client_cert_tb1.data.ca ''
set_default core.client_cert_tb1.data.crt ''
set_default core.client_cert_tb1.data.key ''
set_default core.client_cert_tb2.file.ca certs/client_tb2/ca.der
set_default core.client_cert_tb2.file.crt certs/client_tb2/client.der
set_default core.client_cert_tb2.file.key certs/client_tb2/private.der
set_default core.client_cert_tb2.data.ca ''
set_default core.client_cert_tb2.data.crt ''
set_default core.client_cert_tb2.data.key ''
set_default core.client_cert_fake.file.ca certs/client_tb1/ca.fake.der
set_default core.client_cert_fake.file.crt certs/client_tb1/client.fake.der
set_default core.client_cert_fake.file.key certs/client_tb1/private.fake.der
set_default core.client_cert_fake.data.ca ''
set_default core.client_cert_fake.data.crt ''
set_default core.client_cert_fake.data.key ''
set_default mqtt_server.hostname ici.tonie.cloud
set_default mqtt_server.cert.crt certs/server_tb2/ici.pem
set_default mqtt_server.cert.key certs/server_tb2/ici.key

trim_cr() {
    REPLY="${1%$'\r'}"
}

read_global_config() {
    local line key value
    while IFS= read -r line || [[ -n "$line" ]]; do
        trim_cr "$line"
        line="$REPLY"
        [[ -z "$line" || "${line:0:1}" == '#' || "$line" != *'='* ]] && continue
        key="${line%%=*}"
        value="${line#*=}"
        GLOBAL["$key"]="$value"
        GLOBAL_PRESENT["$key"]=1
    done < "$CONFIG_FILE"
}

read_overlay_config() {
    local line key value rest overlay_id option
    [[ -e "$OVERLAY_FILE" ]] || return 0
    if [[ ! -r "$OVERLAY_FILE" ]]; then
        printf 'ERROR: TeddyCloud overlay configuration is not readable: %s\n' "$OVERLAY_FILE" >&2
        exit 3
    fi
    while IFS= read -r line || [[ -n "$line" ]]; do
        trim_cr "$line"
        line="$REPLY"
        [[ -z "$line" || "${line:0:1}" == '#' || "$line" != *'='* ]] && continue
        key="${line%%=*}"
        value="${line#*=}"
        [[ "$key" == overlay.*.* ]] || continue
        rest="${key#overlay.}"
        overlay_id="${rest%%.*}"
        option="${rest#*.}"
        [[ -n "$overlay_id" && -n "$option" ]] || continue
        OVERLAY_IDS["$overlay_id"]=1
        OVERLAY["$overlay_id|$option"]="$value"
        OVERLAY_PRESENT["$overlay_id|$option"]=1
    done < "$OVERLAY_FILE"
}

read_global_config
read_overlay_config

effective() {
    local overlay_id="$1"
    local key="$2"
    if [[ -n "$overlay_id" && -n "${OVERLAY_PRESENT["$overlay_id|$key"]:-}" ]]; then
        REPLY="${OVERLAY["$overlay_id|$key"]}"
    elif [[ -n "${GLOBAL_PRESENT["$key"]:-}" ]]; then
        REPLY="${GLOBAL["$key"]}"
    else
        REPLY="${DEFAULTS["$key"]:-}"
    fi
}

is_true() {
    [[ "${1,,}" == true || "$1" == 1 ]]
}

resolve_path() {
    local value="$1"
    if [[ -z "$value" ]]; then
        REPLY=''
    elif [[ "$value" == /* ]]; then
        REPLY="$value"
    else
        REPLY="$BASE_PATH/$value"
    fi
}

MATERIAL_COUNTER=0
resolve_material() {
    local overlay_id="$1"
    local file_key="$2"
    local data_key="$3"
    local data_value file_value
    effective "$overlay_id" "$data_key"
    data_value="$REPLY"
    if [[ -n "$data_value" ]]; then
        MATERIAL_COUNTER=$((MATERIAL_COUNTER + 1))
        RESOLVED_PATH="$TMP_DIR/inline-$MATERIAL_COUNTER"
        data_value="${data_value//\\n/$'\n'}"
        printf '%s' "$data_value" > "$RESOLVED_PATH"
        chmod 600 "$RESOLVED_PATH"
        RESOLVED_DISPLAY="<inline:$data_key>"
        RESOLVED_SOURCE=inline
        return
    fi
    effective "$overlay_id" "$file_key"
    file_value="$REPLY"
    resolve_path "$file_value"
    RESOLVED_PATH="$REPLY"
    RESOLVED_DISPLAY="${file_value:-<unset>}"
    RESOLVED_SOURCE=file
}

resolve_file_only() {
    local overlay_id="$1"
    local file_key="$2"
    effective "$overlay_id" "$file_key"
    resolve_path "$REPLY"
    RESOLVED_PATH="$REPLY"
    effective "$overlay_id" "$file_key"
    RESOLVED_DISPLAY="${REPLY:-<unset>}"
    RESOLVED_SOURCE=file
}

file_state() {
    local path="$1"
    if [[ -z "$path" || ! -e "$path" ]]; then
        REPLY=missing
    elif [[ ! -f "$path" || ! -r "$path" || ! -s "$path" ]]; then
        REPLY=invalid
    else
        REPLY=present
    fi
}

NORMAL_COUNTER=0
normalize_cert() {
    local source="$1"
    NORMAL_COUNTER=$((NORMAL_COUNTER + 1))
    NORMALIZED_PATH="$TMP_DIR/cert-$NORMAL_COUNTER.pem"
    if openssl x509 -in "$source" -out "$NORMALIZED_PATH" >/dev/null 2>&1; then
        NORMALIZED_FORMAT=PEM
        return 0
    fi
    if openssl x509 -inform DER -in "$source" -out "$NORMALIZED_PATH" >/dev/null 2>&1; then
        NORMALIZED_FORMAT=DER
        return 0
    fi
    rm -f -- "$NORMALIZED_PATH"
    return 1
}

normalize_key() {
    local source="$1"
    NORMAL_COUNTER=$((NORMAL_COUNTER + 1))
    NORMALIZED_PATH="$TMP_DIR/key-$NORMAL_COUNTER.pem"
    if openssl pkey -in "$source" -out "$NORMALIZED_PATH" >/dev/null 2>&1; then
        NORMALIZED_FORMAT=PEM
        chmod 600 "$NORMALIZED_PATH"
        return 0
    fi
    if openssl pkey -inform DER -in "$source" -out "$NORMALIZED_PATH" >/dev/null 2>&1; then
        NORMALIZED_FORMAT=DER
        chmod 600 "$NORMALIZED_PATH"
        return 0
    fi
    rm -f -- "$NORMALIZED_PATH"
    return 1
}

cert_fingerprint() {
    REPLY="$(openssl x509 -in "$1" -noout -fingerprint -sha256 2>/dev/null | sed 's/^[^=]*=//; s/://g')"
}

short_fingerprint() {
    cert_fingerprint "$1"
    REPLY="${REPLY:0:16}"
}

public_key_fingerprint_from_cert() {
    REPLY="$(openssl x509 -in "$1" -pubkey -noout 2>/dev/null | openssl pkey -pubin -outform DER 2>/dev/null | openssl dgst -sha256 2>/dev/null | sed 's/^.*= //')"
}

public_key_fingerprint_from_key() {
    REPLY="$(openssl pkey -in "$1" -pubout -outform DER 2>/dev/null | openssl dgst -sha256 2>/dev/null | sed 's/^.*= //')"
}

cert_subject() {
    REPLY="$(openssl x509 -in "$1" -noout -subject -nameopt RFC2253 2>/dev/null | sed 's/^subject=//')"
}

cert_issuer() {
    REPLY="$(openssl x509 -in "$1" -noout -issuer -nameopt RFC2253 2>/dev/null | sed 's/^issuer=//')"
}

cert_cn() {
    cert_subject "$1"
    local subject="$REPLY"
    if [[ "$subject" =~ (^|,)CN=([^,]+) ]]; then
        REPLY="${BASH_REMATCH[2]}"
    else
        REPLY=''
    fi
}

canonical_box_id() {
    local candidate="$1"
    if [[ "$candidate" =~ ^[bB]\'([0-9A-Fa-f]{12})\'$ ]]; then
        REPLY="${BASH_REMATCH[1]^^}"
        return 0
    fi
    if [[ "$candidate" =~ ^[0-9A-Fa-f]{12}$ ]]; then
        REPLY="${candidate^^}"
        return 0
    fi
    REPLY=''
    return 1
}

CA_TB1_FP=''
CA_TB2_FP=''
CA_LOCAL_FP=''
classify_ca_textual() {
    local ca_pem="$1"
    local subject issuer combined
    cert_subject "$ca_pem"
    subject="$REPLY"
    cert_issuer "$ca_pem"
    issuer="$REPLY"
    combined="${subject,,} ${issuer,,}"
    if [[ "$combined" == *boxine* ]]; then
        REPLY=TB1
    elif [[ "$combined" == *tonies* || "$combined" == *'tonie cloud'* ]]; then
        REPLY=TB2
    elif [[ "$combined" == *teddycloud* || "$combined" == *'team revvox'* ]]; then
        REPLY=LOCAL
    else
        REPLY=UNKNOWN
    fi
}

classify_leaf_textual() {
    local cert_pem="$1"
    local subject issuer combined
    cert_subject "$cert_pem"
    subject="$REPLY"
    cert_issuer "$cert_pem"
    issuer="$REPLY"
    combined="${subject,,} ${issuer,,}"
    if [[ "$combined" == *boxine* ]]; then
        REPLY=TB1
    elif [[ "$combined" == *tonies* || "$combined" == *'tonie cloud'* ]]; then
        REPLY=TB2
    elif [[ "$combined" == *teddycloud* || "$combined" == *'team revvox'* ]]; then
        REPLY=LOCAL
    else
        REPLY=UNKNOWN
    fi
}

classify_ca() {
    local ca_pem="$1"
    local fp subject issuer combined
    cert_fingerprint "$ca_pem"
    fp="$REPLY"
    cert_subject "$ca_pem"
    subject="$REPLY"
    cert_issuer "$ca_pem"
    issuer="$REPLY"
    combined="${subject,,} ${issuer,,}"
    if [[ -n "$CA_TB1_FP" && "$fp" == "$CA_TB1_FP" ]]; then
        REPLY=TB1
    elif [[ -n "$CA_TB2_FP" && "$fp" == "$CA_TB2_FP" ]]; then
        REPLY=TB2
    elif [[ -n "$CA_LOCAL_FP" && "$fp" == "$CA_LOCAL_FP" ]]; then
        REPLY=LOCAL
    else
        classify_ca_textual "$ca_pem"
    fi
}

remember_known_ca() {
    local generation="$1"
    local path="$2"
    local normalized
    file_state "$path"
    [[ "$REPLY" == present ]] || return 0
    normalize_cert "$path" || return 0
    normalized="$NORMALIZED_PATH"
    classify_ca_textual "$normalized"
    [[ "$REPLY" == "$generation" ]] || return 0
    cert_fingerprint "$normalized"
    case "$generation" in
        TB1) CA_TB1_FP="$REPLY" ;;
        TB2) CA_TB2_FP="$REPLY" ;;
        LOCAL) CA_LOCAL_FP="$REPLY" ;;
    esac
}

expiry_check() {
    local role="$1"
    local active="$2"
    local pem="$3"
    local label="$4"
    local not_after
    not_after="$(openssl x509 -in "$pem" -noout -enddate 2>/dev/null | sed 's/^notAfter=//')"
    if ! openssl x509 -in "$pem" -noout -checkend 0 >/dev/null 2>&1; then
        issue "$active" "$role: $label expired or has an invalid validity period (notAfter=$not_after)"
    elif ! openssl x509 -in "$pem" -noout -checkend "$WARN_SECONDS" >/dev/null 2>&1; then
        report WARN "$role: $label expires within $WARN_DAYS days (notAfter=$not_after)"
    else
        report OK "$role: $label validity (notAfter=$not_after)"
    fi
}

register_identity() {
    local role="$1"
    local display="$2"
    local cert_pem="$3"
    local key_pem="$4"
    local cert_fp key_fp previous
    cert_fingerprint "$cert_pem"
    cert_fp="$REPLY"
    public_key_fingerprint_from_key "$key_pem"
    key_fp="$REPLY"
    previous="${CERT_IDENTITY_PATH["$cert_fp"]:-}"
    if [[ -n "$previous" && "$previous" != "$display" ]]; then
        report INFO "$role: repeated leaf identity at $display (also $previous)"
    else
        CERT_IDENTITY_PATH["$cert_fp"]="$display"
    fi
    previous="${KEY_IDENTITY_PATH["$key_fp"]:-}"
    if [[ -n "$previous" && "$previous" != "$display" ]]; then
        report INFO "$role: repeated private-key identity at $display (also $previous)"
    else
        KEY_IDENTITY_PATH["$key_fp"]="$display"
    fi
}

check_required_sans() {
    local role="$1"
    local active="$2"
    local cert_pem="$3"
    local names_csv="$4"
    local san_text host host_key
    local -A seen_names=()
    san_text="$(openssl x509 -in "$cert_pem" -noout -ext subjectAltName 2>/dev/null || true)"
    IFS=',' read -r -a required_names <<< "$names_csv"
    for host in "${required_names[@]}"; do
        [[ -n "$host" ]] || continue
        host_key="${host,,}"
        [[ -z "${seen_names["$host_key"]:-}" ]] || continue
        seen_names["$host_key"]=1
        if grep -Eiq "DNS:${host//./\\.}([,[:space:]]|$)" <<< "$san_text"; then
            report OK "$role: SAN contains $host"
        else
            issue "$active" "$role: SAN is missing $host"
        fi
    done
}

check_ca_key_pair() {
    local role="$1"
    local active="$2"
    local ca_pem="$3"
    local key_path="$4"
    local key_display="$5"
    local key_pem cert_pub key_pub format
    file_state "$key_path"
    if [[ "$REPLY" != present ]]; then
        issue "$active" "$role: CA private key missing, empty or unreadable: $key_display"
        return
    fi
    if ! normalize_key "$key_path"; then
        issue "$active" "$role: CA private key is not readable as PEM or DER: $key_display"
        return
    fi
    key_pem="$NORMALIZED_PATH"
    format="$NORMALIZED_FORMAT"
    public_key_fingerprint_from_cert "$ca_pem"
    cert_pub="$REPLY"
    public_key_fingerprint_from_key "$key_pem"
    key_pub="$REPLY"
    if [[ -n "$cert_pub" && "$cert_pub" == "$key_pub" ]]; then
        report OK "$role: CA certificate and $format private key match"
    else
        issue "$active" "$role: CA certificate and private key do not match ($key_display)"
    fi
}

check_cert_set() {
    local set_id="$1"
    local role="$2"
    local active="$3"
    local expected_generation="$4"
    local ca_path="$5"
    local ca_display="$6"
    local cert_path="$7"
    local cert_display="$8"
    local key_path="$9"
    local key_display="${10}"
    local kind="${11}"
    local sans="${12}"
    local expected_box_id="${13}"
    local directory_id="${14}"
    local ca_state cert_state key_state present_count=0
    local ca_pem='' cert_pem='' key_pem='' ca_format cert_format key_format
    local subject issuer fp cn canonical_cn generation=UNKNOWN leaf_generation=UNKNOWN
    local cert_pub key_pub ca_key_usage ca_subject verify_output

    SET_WARN_START["$set_id"]="$WARN_COUNT"
    SET_ERROR_START["$set_id"]="$ERROR_COUNT"

    file_state "$ca_path"; ca_state="$REPLY"; [[ "$ca_state" == present ]] && present_count=$((present_count + 1))
    file_state "$cert_path"; cert_state="$REPLY"; [[ "$cert_state" == present ]] && present_count=$((present_count + 1))
    file_state "$key_path"; key_state="$REPLY"; [[ "$key_state" == present ]] && present_count=$((present_count + 1))

    if ((present_count == 0)); then
        SET_COMPLETE["$set_id"]=0
        SET_GENERATION["$set_id"]=UNKNOWN
        if ((active)); then
            report ERROR "$role: required certificate set is absent (CA=$ca_display cert=$cert_display key=$key_display)"
        else
            report INFO "$role: certificate set is absent and the role is disabled"
        fi
        update_set_result "$set_id"
        return
    fi
    if ((present_count != 3)); then
        SET_COMPLETE["$set_id"]=0
        issue "$active" "$role: incomplete certificate set (CA=$ca_state cert=$cert_state key=$key_state)"
    fi

    if [[ "$ca_state" == present ]]; then
        if normalize_cert "$ca_path"; then
            ca_pem="$NORMALIZED_PATH"; ca_format="$NORMALIZED_FORMAT"
            short_fingerprint "$ca_pem"; fp="$REPLY"
            cert_subject "$ca_pem"; subject="$REPLY"
            report INFO "$role: CA $ca_format fingerprint=$fp subject=$subject path=$ca_display"
            if openssl x509 -in "$ca_pem" -noout -ext basicConstraints 2>/dev/null | grep -q 'CA:TRUE'; then
                report OK "$role: CA certificate has CA:TRUE"
            else
                issue "$active" "$role: configured CA lacks CA:TRUE"
            fi
            ca_key_usage="$(openssl x509 -in "$ca_pem" -noout -ext keyUsage 2>/dev/null || true)"
            if [[ -n "$ca_key_usage" ]]; then
                if grep -qi 'Certificate Sign' <<< "$ca_key_usage"; then
                    report OK "$role: CA key usage permits certificate signing"
                else
                    issue "$active" "$role: CA key usage does not permit certificate signing"
                fi
            fi
            expiry_check "$role" "$active" "$ca_pem" CA
            classify_ca "$ca_pem"; generation="$REPLY"
            SET_GENERATION["$set_id"]="$generation"
            if [[ "$expected_generation" != ANY && "$generation" != "$expected_generation" ]]; then
                issue "$active" "$role: detected certificate generation $generation, expected $expected_generation"
            else
                report OK "$role: detected generation $generation"
            fi
        else
            if [[ "$set_id" == server-tb1 ]]; then
                report WARN "$role: OpenSSL cannot parse the legacy TB1 CA at $ca_display; keep its bytes unchanged and inspect details with --verbose"
            else
                issue "$active" "$role: CA is not readable as PEM or DER: $ca_display"
            fi
        fi
    fi

    if [[ "$cert_state" == present ]]; then
        if normalize_cert "$cert_path"; then
            cert_pem="$NORMALIZED_PATH"; cert_format="$NORMALIZED_FORMAT"
            short_fingerprint "$cert_pem"; fp="$REPLY"
            cert_subject "$cert_pem"; subject="$REPLY"
            cert_issuer "$cert_pem"; issuer="$REPLY"
            cert_cn "$cert_pem"; cn="$REPLY"
            SET_CN["$set_id"]="$cn"
            classify_leaf_textual "$cert_pem"; leaf_generation="$REPLY"
            report INFO "$role: leaf $cert_format fingerprint=$fp subject=$subject issuer=$issuer path=$cert_display"
            if openssl x509 -in "$cert_pem" -noout -ext basicConstraints 2>/dev/null | grep -q 'CA:TRUE'; then
                issue "$active" "$role: leaf certificate is incorrectly marked as a CA"
            else
                report OK "$role: leaf certificate is not a CA"
            fi
            expiry_check "$role" "$active" "$cert_pem" leaf
            [[ -z "$sans" ]] || check_required_sans "$role" "$active" "$cert_pem" "$sans"
            if [[ "$kind" == client || "$kind" == fake ]]; then
                if canonical_box_id "$cn"; then
                    canonical_cn="$REPLY"
                    report OK "$role: valid box CN $canonical_cn"
                    if [[ -n "$expected_box_id" && "${canonical_cn^^}" != "${expected_box_id^^}" ]]; then
                        issue "$active" "$role: certificate CN $canonical_cn does not match overlay $expected_box_id"
                    fi
                    if [[ -n "$directory_id" && "${canonical_cn^^}" != "${directory_id^^}" ]]; then
                        issue "$active" "$role: certificate CN $canonical_cn does not match directory $directory_id"
                    fi
                elif [[ "$kind" == client ]]; then
                    issue "$active" "$role: client CN is not <12-hex> or b'<12-hex>' (CN=$cn)"
                else
                    report INFO "$role: local fake-client CN is not a box ID (CN=$cn)"
                fi
            fi
            if [[ "$kind" == client && "$expected_generation" != ANY && "$leaf_generation" != "$expected_generation" ]]; then
                issue "$active" "$role: detected leaf generation $leaf_generation, expected $expected_generation"
            fi
        else
            issue "$active" "$role: leaf certificate is not readable as PEM or DER: $cert_display"
        fi
    fi

    if [[ "$key_state" == present ]]; then
        if normalize_key "$key_path"; then
            key_pem="$NORMALIZED_PATH"; key_format="$NORMALIZED_FORMAT"
            report OK "$role: private key is readable as $key_format ($key_display)"
        else
            issue "$active" "$role: private key is not readable as PEM or DER: $key_display"
        fi
    fi

    if [[ -n "$ca_pem" && -n "$cert_pem" ]]; then
        if verify_output="$(openssl verify -CAfile "$ca_pem" "$cert_pem" 2>&1)"; then
            report OK "$role: leaf certificate verifies against the configured CA"
        else
            cert_subject "$ca_pem"; ca_subject="$REPLY"
            cert_issuer "$cert_pem"; issuer="$REPLY"
            if [[ "$kind" == client && "$issuer" != "$ca_subject" && "$generation" == "$expected_generation" && "$leaf_generation" == "$expected_generation" ]] \
                && grep -Eqi 'unable to get local issuer certificate|unable to verify the first certificate' <<< "$verify_output"; then
                report INFO "$role: leaf was issued by $issuer; its intermediate certificate is not included, so the full chain cannot be verified locally"
            else
                issue "$active" "$role: leaf certificate does not verify against the configured CA"
            fi
        fi
    fi
    if [[ -n "$cert_pem" && -n "$key_pem" ]]; then
        public_key_fingerprint_from_cert "$cert_pem"; cert_pub="$REPLY"
        public_key_fingerprint_from_key "$key_pem"; key_pub="$REPLY"
        if [[ -n "$cert_pub" && "$cert_pub" == "$key_pub" ]]; then
            report OK "$role: leaf certificate and private key match"
            register_identity "$role" "$cert_display" "$cert_pem" "$key_pem"
        else
            issue "$active" "$role: leaf certificate and private key do not match"
        fi
    fi

    if [[ -n "$ca_pem" && -n "$cert_pem" && -n "$key_pem" ]]; then
        SET_COMPLETE["$set_id"]=1
    else
        SET_COMPLETE["$set_id"]=0
    fi
    update_set_result "$set_id"
}

component_is_overridden() {
    local overlay_id="$1"
    local file_key="$2"
    local data_key="$3"
    [[ -n "${OVERLAY_PRESENT["$overlay_id|$file_key"]:-}" || -n "${OVERLAY_PRESENT["$overlay_id|$data_key"]:-}" ]]
}

configured_path_state() {
    local candidate="$1"
    local key overlay_id value
    REPLY=no
    for key in \
        core.server_cert.file.ca core.server_cert.file.ca_key core.server_cert.file.crt core.server_cert.file.key \
        core.server_cert.file.ca_der \
        core.server_cert_tb2.file.ca core.server_cert_tb2.file.ca_der core.server_cert_tb2.file.ca_key core.server_cert_tb2.file.crt core.server_cert_tb2.file.key \
        core.client_cert_tb1.file.ca core.client_cert_tb1.file.crt core.client_cert_tb1.file.key \
        core.client_cert_tb2.file.ca core.client_cert_tb2.file.crt core.client_cert_tb2.file.key \
        mqtt_server.cert.crt mqtt_server.cert.key; do
        effective '' "$key"; resolve_path "$REPLY"; value="$REPLY"
        [[ "$value" == "$candidate" ]] && { REPLY=yes; return; }
        for overlay_id in "${!OVERLAY_IDS[@]}"; do
            effective "$overlay_id" "$key"; resolve_path "$REPLY"; value="$REPLY"
            [[ "$value" == "$candidate" ]] && { REPLY=yes; return; }
        done
    done
}

check_legacy_tree() {
    local legacy_name="$1"
    local canonical_name="$2"
    local legacy="$CERT_ROOT/$legacy_name"
    local canonical="$CERT_ROOT/$canonical_name"
    local source rel target configured target_root target_name
    local missing_count=0 identical_count=0 conflict_count=0
    [[ -d "$legacy" ]] || return
    while IFS= read -r source; do
        [[ "${source##*/}" == .gitkeep ]] && continue
        rel="${source#"$legacy"/}"
        target_root="$canonical"
        target_name="$canonical_name"
        if [[ "$legacy_name" == server && ("$rel" == ici.pem || "$rel" == ici.key) ]]; then
            target_root="$CERT_ROOT/server_tb2"
            target_name=server_tb2
        fi
        target="$target_root/$rel"
        if [[ ! -e "$target" ]]; then
            missing_count=$((missing_count + 1))
            report_detail WARN "Legacy $legacy_name/$rel has no canonical $target_name counterpart; migration recommended"
        elif cmp -s -- "$source" "$target"; then
            identical_count=$((identical_count + 1))
            report_detail WARN "Identical legacy duplicate: $legacy_name/$rel and $target_name/$rel"
        else
            configured_path_state "$source"; configured="legacy-configured=$REPLY"
            configured_path_state "$target"; configured="$configured canonical-configured=$REPLY"
            conflict_count=$((conflict_count + 1))
            report_detail ERROR "Conflicting legacy/canonical files: $legacy_name/$rel != $target_name/$rel ($configured)"
        fi
    done < <(find "$legacy" -type f -print 2>/dev/null | sort)
    if ((!VERBOSE)); then
        ((missing_count == 0)) || report WARN "Legacy $legacy_name: $missing_count file(s) have no canonical counterpart; migration recommended (details: --verbose)"
        ((identical_count == 0)) || report WARN "Legacy $legacy_name: $identical_count identical duplicate file(s) (details: --verbose)"
        ((conflict_count == 0)) || report ERROR "Legacy $legacy_name: $conflict_count file(s) conflict with their canonical counterpart (details: --verbose)"
    fi
}

check_ca_representations() {
    local root_name="$1"
    local pem="$CERT_ROOT/$root_name/ca-root.pem"
    local der="$CERT_ROOT/$root_name/ca.der"
    local pem_normalized der_normalized pem_fp der_fp
    [[ -e "$pem" || -e "$der" ]] || return
    if [[ ! -s "$pem" || ! -s "$der" ]]; then
        report WARN "$root_name: CA PEM/DER representation is incomplete (ca-root.pem and ca.der)"
        return
    fi
    if ! normalize_cert "$pem"; then
        if [[ "$root_name" == server_tb1 ]]; then
            report INFO "$root_name: modern OpenSSL cannot parse the legacy ca-root.pem representation; bytes were left unchanged"
        else
            report ERROR "$root_name: ca-root.pem is not a readable certificate"
        fi
        return
    fi
    pem_normalized="$NORMALIZED_PATH"
    if ! normalize_cert "$der"; then
        report ERROR "$root_name: ca.der is not a readable certificate"
        return
    fi
    der_normalized="$NORMALIZED_PATH"
    cert_fingerprint "$pem_normalized"; pem_fp="$REPLY"
    cert_fingerprint "$der_normalized"; der_fp="$REPLY"
    if [[ "$pem_fp" == "$der_fp" ]]; then
        report OK "$root_name: ca-root.pem and ca.der contain the same CA"
    else
        report ERROR "$root_name: ca-root.pem and ca.der contain different CAs"
    fi
}

check_hidden_certificate_backups() {
    local backup count
    while IFS= read -r backup; do
        [[ -n "$backup" ]] || continue
        count="$(find "$backup" -type f 2>/dev/null | wc -l | tr -d ' ')"
        report INFO "Certificate backup ${backup#"$CERT_ROOT"/}: $count preserved file(s); not used at runtime"
    done < <(find "$CERT_ROOT" -mindepth 1 -maxdepth 1 -type d \
        \( -name '.server.bak' -o -name '.server.bak.*' \
           -o -name '.client.bak' -o -name '.client.bak.*' \) \
        -print 2>/dev/null | sort)
}

operational_legacy_path_state() {
    local key="$1"
    local rest overlay_id option option_generation configured_generation
    REPLY=active

    [[ "$key" == overlay.*.* ]] || return
    rest="${key#overlay.}"
    overlay_id="${rest%%.*}"
    option="${rest#*.}"
    case "$option" in
        core.client_cert_tb1.*) option_generation=1 ;;
        core.client_cert_tb2.*) option_generation=2 ;;
        *) return ;;
    esac

    effective "$overlay_id" toniebox.boxGeneration
    configured_generation="$REPLY"
    case "$configured_generation" in
        1|2)
            if [[ "$configured_generation" != "$option_generation" ]]; then
                REPLY=inactive
            else
                REPLY=active
            fi
            ;;
        *) REPLY=unknown ;;
    esac
}

check_operational_legacy_paths() {
    local config_file line key value state
    for config_file in "$CONFIG_FILE" "$OVERLAY_FILE"; do
        [[ -f "$config_file" ]] || continue
        while IFS= read -r line; do
            [[ "$line" == *=* ]] || continue
            key="${line%%=*}"
            value="${line#*=}"
            case "$value" in
                certs/server|certs/server/*|certs/client|certs/client/*)
                    operational_legacy_path_state "$key"
                    state="$REPLY"
                    case "$state" in
                        inactive)
                            # Preserve settings for a future manual generation switch,
                            # but do not diagnose them as operational for this box.
                            ;;
                        unknown)
                            report INFO "Stored legacy certificate path has no active overlay generation in ${config_file#"$BASE_PATH"/}: $key=$value"
                            ;;
                        *)
                            report ERROR "Operational legacy certificate path remains in ${config_file#"$BASE_PATH"/}: $key=$value"
                            ;;
                    esac
                    ;;
            esac
        done < "$config_file"
    done
}

directory_box_id() {
    local name="$1"
    if [[ "$name" =~ ^[0-9A-Fa-f]{12}$ ]]; then
        REPLY="${name^^}"
    else
        REPLY=''
    fi
}

inventory_client_tree() {
    local generation="$1"
    local root_name="$2"
    local root="$CERT_ROOT/$root_name"
    local source cert dir dir_name dir_id role ca key
    local -A seen_dirs=()
    [[ -d "$root" ]] || { report INFO "$root_name: directory absent"; return; }
    while IFS= read -r source; do
        dir="${source%/*}"
        [[ -z "${seen_dirs["$dir"]:-}" ]] || continue
        seen_dirs["$dir"]=1
        dir_name="${dir##*/}"
        directory_box_id "$dir_name"; dir_id="$REPLY"
        ca="$dir/ca.der"
        cert="$dir/client.der"
        key="$dir/private.der"
        role="$root_name/${dir#"$root"/}"
        [[ "$dir" == "$root" ]] && role="$root_name/global"
        check_cert_set "inventory:$generation:$dir" "Inventory $role" 0 "$generation" \
            "$ca" "${ca#"$BASE_PATH"/}" "$cert" "${cert#"$BASE_PATH"/}" \
            "$key" "${key#"$BASE_PATH"/}" client '' '' "$dir_id"
    done < <(find "$root" -mindepth 1 -maxdepth 2 -type f \
        \( -name ca.der -o -name client.der -o -name private.der \) -print 2>/dev/null | sort)
}

inventory_fake_tree() {
    local root_name="$1"
    local root="$CERT_ROOT/$root_name"
    local source cert dir dir_name dir_id ca key role
    local -A seen_dirs=()
    [[ -d "$root" ]] || return
    while IFS= read -r source; do
        dir="${source%/*}"
        [[ -z "${seen_dirs["$dir"]:-}" ]] || continue
        seen_dirs["$dir"]=1
        dir_name="${dir##*/}"
        directory_box_id "$dir_name"; dir_id="$REPLY"
        ca="$dir/ca.fake.der"
        cert="$dir/client.fake.der"
        key="$dir/private.fake.der"
        role="$root_name/${dir#"$root"/}"
        [[ "$dir" == "$root" ]] && role="$root_name/global"
        check_cert_set "fake:$dir" "Local fake client $role" 0 LOCAL \
            "$ca" "${ca#"$BASE_PATH"/}" "$cert" "${cert#"$BASE_PATH"/}" \
            "$key" "${key#"$BASE_PATH"/}" fake '' '' "$dir_id"
    done < <(find "$root" -mindepth 1 -maxdepth 2 -type f \
        \( -name ca.fake.der -o -name client.fake.der -o -name private.fake.der \) -print 2>/dev/null | sort)
}

# Establish known CA fingerprints only after effective global paths are known.
resolve_material '' core.client_cert_tb1.file.ca core.client_cert_tb1.data.ca
remember_known_ca TB1 "$RESOLVED_PATH"
resolve_material '' core.client_cert_tb2.file.ca core.client_cert_tb2.data.ca
remember_known_ca TB2 "$RESOLVED_PATH"
resolve_material '' core.server_cert_tb2.file.ca core.server_cert_tb2.data.ca
remember_known_ca LOCAL "$RESOLVED_PATH"

TB1_SERVER_ACTIVE=0
TB2_SERVER_ACTIVE=0
for overlay_id in "${!OVERLAY_IDS[@]}"; do
    effective "$overlay_id" toniebox.boxGeneration
    [[ "$REPLY" == 1 ]] && TB1_SERVER_ACTIVE=1
    if [[ "$REPLY" == 2 ]]; then
        effective "$overlay_id" cloud.tb2_enabled; proxy_value="$REPLY"
        effective "$overlay_id" cloud.tb2_v3_enabled; v3_value="$REPLY"
        if is_true "$proxy_value" || is_true "$v3_value"; then
            TB2_SERVER_ACTIVE=1
        fi
    fi
done
effective '' cloud.enabled; is_true "$REPLY" && TB1_UPSTREAM_ACTIVE=1 || TB1_UPSTREAM_ACTIVE=0
effective '' cloud.tb2_enabled; proxy_value="$REPLY"
effective '' cloud.tb2_v3_enabled; v3_value="$REPLY"
if is_true "$proxy_value" || is_true "$v3_value"; then
    TB2_SERVER_ACTIVE=1
fi
effective '' mqtt_client_upstream.enabled; mqtt_upstream_value="$REPLY"
if is_true "$proxy_value" || is_true "$v3_value" || is_true "$mqtt_upstream_value"; then
    TB2_UPSTREAM_ACTIVE=1
else
    TB2_UPSTREAM_ACTIVE=0
fi
effective '' mqtt_server.enabled; is_true "$REPLY" && MQTT_SERVER_ACTIVE=1 || MQTT_SERVER_ACTIVE=0

compact_section 'Certificate roles'
section 'Server certificate roles'
resolve_material '' core.server_cert.file.ca core.server_cert.data.ca; tb1_server_ca="$RESOLVED_PATH"; tb1_server_ca_display="$RESOLVED_DISPLAY"
resolve_material '' core.server_cert.file.crt core.server_cert.data.crt; tb1_server_crt="$RESOLVED_PATH"; tb1_server_crt_display="$RESOLVED_DISPLAY"
resolve_material '' core.server_cert.file.key core.server_cert.data.key; tb1_server_key="$RESOLVED_PATH"; tb1_server_key_display="$RESOLVED_DISPLAY"
check_cert_set server-tb1 'TB1 HTTPS server' "$TB1_SERVER_ACTIVE" LOCAL \
    "$tb1_server_ca" "$tb1_server_ca_display" "$tb1_server_crt" "$tb1_server_crt_display" \
    "$tb1_server_key" "$tb1_server_key_display" server '' '' ''
if [[ "${SET_COMPLETE[server-tb1]:-0}" == 1 ]]; then
    resolve_material '' core.server_cert.file.ca_key core.server_cert.data.ca_key
    normalize_cert "$tb1_server_ca" && check_ca_key_pair 'TB1 HTTPS server' "$TB1_SERVER_ACTIVE" "$NORMALIZED_PATH" "$RESOLVED_PATH" "$RESOLVED_DISPLAY"
fi
update_set_result server-tb1
compact_set_result server-tb1 'TB1 HTTPS server' "$TB1_SERVER_ACTIVE" "$tb1_server_crt_display"

effective '' core.server_cert_tb2.hostname; https_hostname="$REPLY"
resolve_material '' core.server_cert_tb2.file.ca core.server_cert_tb2.data.ca; tb2_server_ca="$RESOLVED_PATH"; tb2_server_ca_display="$RESOLVED_DISPLAY"
resolve_material '' core.server_cert_tb2.file.crt core.server_cert_tb2.data.crt; tb2_server_crt="$RESOLVED_PATH"; tb2_server_crt_display="$RESOLVED_DISPLAY"
resolve_material '' core.server_cert_tb2.file.key core.server_cert_tb2.data.key; tb2_server_key="$RESOLVED_PATH"; tb2_server_key_display="$RESOLVED_DISPLAY"
check_cert_set server-tb2-https 'TB2 HTTPS server' "$TB2_SERVER_ACTIVE" LOCAL \
    "$tb2_server_ca" "$tb2_server_ca_display" "$tb2_server_crt" "$tb2_server_crt_display" \
    "$tb2_server_key" "$tb2_server_key_display" server "$https_hostname,tbs2.tonie.cloud" '' ''
if [[ "${SET_COMPLETE[server-tb2-https]:-0}" == 1 ]]; then
    resolve_material '' core.server_cert_tb2.file.ca_key core.server_cert_tb2.data.ca_key
    normalize_cert "$tb2_server_ca" && check_ca_key_pair 'TB2 HTTPS server' "$TB2_SERVER_ACTIVE" "$NORMALIZED_PATH" "$RESOLVED_PATH" "$RESOLVED_DISPLAY"
fi
update_set_result server-tb2-https
compact_set_result server-tb2-https 'TB2 HTTPS server' "$TB2_SERVER_ACTIVE" "$tb2_server_crt_display"

effective '' mqtt_server.hostname; mqtt_hostname="$REPLY"
resolve_file_only '' mqtt_server.cert.crt; mqtt_server_crt="$RESOLVED_PATH"; mqtt_server_crt_display="$RESOLVED_DISPLAY"
resolve_file_only '' mqtt_server.cert.key; mqtt_server_key="$RESOLVED_PATH"; mqtt_server_key_display="$RESOLVED_DISPLAY"
check_cert_set server-tb2-mqtt 'TB2 ICI-MQTT server' "$MQTT_SERVER_ACTIVE" LOCAL \
    "$tb2_server_ca" "$tb2_server_ca_display" "$mqtt_server_crt" "$mqtt_server_crt_display" \
    "$mqtt_server_key" "$mqtt_server_key_display" server \
    "$mqtt_hostname,ici.tonie.cloud,ici.dev.tonie.cloud,ici.stage.tonie.cloud" '' ''
compact_set_result server-tb2-mqtt 'TB2 ICI-MQTT server' "$MQTT_SERVER_ACTIVE" "$mqtt_server_crt_display"

section 'Global upstream client identities'
resolve_material '' core.client_cert_tb1.file.ca core.client_cert_tb1.data.ca; tb1_client_ca="$RESOLVED_PATH"; tb1_client_ca_display="$RESOLVED_DISPLAY"
resolve_material '' core.client_cert_tb1.file.crt core.client_cert_tb1.data.crt; tb1_client_crt="$RESOLVED_PATH"; tb1_client_crt_display="$RESOLVED_DISPLAY"
resolve_material '' core.client_cert_tb1.file.key core.client_cert_tb1.data.key; tb1_client_key="$RESOLVED_PATH"; tb1_client_key_display="$RESOLVED_DISPLAY"
check_cert_set client-tb1-global 'TB1 Boxine upstream client (global)' "$TB1_UPSTREAM_ACTIVE" TB1 \
    "$tb1_client_ca" "$tb1_client_ca_display" "$tb1_client_crt" "$tb1_client_crt_display" \
    "$tb1_client_key" "$tb1_client_key_display" client '' '' ''
compact_set_result client-tb1-global 'TB1 Boxine upstream client' "$TB1_UPSTREAM_ACTIVE" "$tb1_client_crt_display"

resolve_material '' core.client_cert_tb2.file.ca core.client_cert_tb2.data.ca; tb2_client_ca="$RESOLVED_PATH"; tb2_client_ca_display="$RESOLVED_DISPLAY"
resolve_material '' core.client_cert_tb2.file.crt core.client_cert_tb2.data.crt; tb2_client_crt="$RESOLVED_PATH"; tb2_client_crt_display="$RESOLVED_DISPLAY"
resolve_material '' core.client_cert_tb2.file.key core.client_cert_tb2.data.key; tb2_client_key="$RESOLVED_PATH"; tb2_client_key_display="$RESOLVED_DISPLAY"
check_cert_set client-tb2-global 'TB2 TONIES upstream client (global)' "$TB2_UPSTREAM_ACTIVE" TB2 \
    "$tb2_client_ca" "$tb2_client_ca_display" "$tb2_client_crt" "$tb2_client_crt_display" \
    "$tb2_client_key" "$tb2_client_key_display" client '' '' ''
compact_set_result client-tb2-global 'TB2 TONIES upstream client' "$TB2_UPSTREAM_ACTIVE" "$tb2_client_crt_display"
if is_true "$mqtt_upstream_value"; then
    report INFO 'TB2 ICI-MQTT client: active; shares the effective TB2 TONIES client identity above'
    compact_result INFO 'TB2 ICI-MQTT client - active, shares the TB2 TONIES identity'
    record_role client-tb2-mqtt 'TB2 ICI-MQTT client' 1 "${SET_RESULT[client-tb2-global]:-info}" \
        "${SET_GENERATION[client-tb2-global]:-UNKNOWN}" "${SET_CN[client-tb2-global]:-}" "$tb2_client_crt_display"
else
    report INFO 'TB2 ICI-MQTT client: disabled; shares the effective TB2 TONIES client identity when enabled'
    compact_result INFO 'TB2 ICI-MQTT client - disabled, shares the TB2 TONIES identity when enabled'
    record_role client-tb2-mqtt 'TB2 ICI-MQTT client' 0 info \
        "${SET_GENERATION[client-tb2-global]:-UNKNOWN}" "${SET_CN[client-tb2-global]:-}" "$tb2_client_crt_display"
fi

section 'Configured local fake-client identity'
resolve_material '' core.client_cert_fake.file.ca core.client_cert_fake.data.ca; fake_client_ca="$RESOLVED_PATH"; fake_client_ca_display="$RESOLVED_DISPLAY"
resolve_material '' core.client_cert_fake.file.crt core.client_cert_fake.data.crt; fake_client_crt="$RESOLVED_PATH"; fake_client_crt_display="$RESOLVED_DISPLAY"
resolve_material '' core.client_cert_fake.file.key core.client_cert_fake.data.key; fake_client_key="$RESOLVED_PATH"; fake_client_key_display="$RESOLVED_DISPLAY"
check_cert_set fake-global 'Local fake client (configured global)' 0 LOCAL \
    "$fake_client_ca" "$fake_client_ca_display" "$fake_client_crt" "$fake_client_crt_display" \
    "$fake_client_key" "$fake_client_key_display" fake '' '' ''
compact_set_result fake-global 'Local fake client' 0 "$fake_client_crt_display"

compact_section 'Box overlays'
section 'Effective box overlays'
if ((${#OVERLAY_IDS[@]} == 0)); then
    report INFO 'No box overlays are configured'
    compact_result INFO 'No box overlays are configured'
fi
while IFS= read -r overlay_id; do
    [[ -n "$overlay_id" ]] || continue
    effective "$overlay_id" commonName; common_name="$REPLY"
    canonical_box_id "$common_name" && box_id="$REPLY" || box_id="${overlay_id^^}"
    effective "$overlay_id" toniebox.boxGeneration; generation_value="$REPLY"
    case "$generation_value" in
        1) box_generation=TB1; prefix=core.client_cert_tb1 ;;
        2) box_generation=TB2; prefix=core.client_cert_tb2 ;;
        *) box_generation=UNKNOWN; prefix='' ;;
    esac
    report INFO "Overlay $overlay_id: box=$box_id configured-generation=$box_generation"

    selected_generation="$box_generation"
    overlay_generation_note=''
    if [[ "$box_generation" == UNKNOWN ]]; then
        tb1_overrides=0; tb2_overrides=0
        for component in ca crt key; do
            component_is_overridden "$overlay_id" "core.client_cert_tb1.file.$component" "core.client_cert_tb1.data.$component" && tb1_overrides=$((tb1_overrides + 1))
            component_is_overridden "$overlay_id" "core.client_cert_tb2.file.$component" "core.client_cert_tb2.data.$component" && tb2_overrides=$((tb2_overrides + 1))
        done
        if ((tb1_overrides > 0 && tb2_overrides == 0)); then
            selected_generation=TB1; prefix=core.client_cert_tb1
            overlay_generation_note='; recommendation boxGeneration=TB1'
            report INFO "Overlay $overlay_id: recommendation boxGeneration=TB1 based on its explicit certificate settings"
        elif ((tb2_overrides > 0 && tb1_overrides == 0)); then
            selected_generation=TB2; prefix=core.client_cert_tb2
            overlay_generation_note='; recommendation boxGeneration=TB2'
            report INFO "Overlay $overlay_id: recommendation boxGeneration=TB2 based on its explicit certificate settings"
        else
            report WARN "Overlay $overlay_id: boxGeneration is unknown; no unique certificate-based recommendation is possible"
            compact_result WARN "Overlay $overlay_id - boxGeneration unknown, no unique certificate recommendation"
            record_overlay "$overlay_id" "$box_id" UNKNOWN UNKNOWN unknown warning '' ''
            continue
        fi
    fi

    override_count=0
    for component in ca crt key; do
        component_is_overridden "$overlay_id" "$prefix.file.$component" "$prefix.data.$component" && override_count=$((override_count + 1))
    done
    if ((override_count == 0)); then
        report OK "Overlay $overlay_id: uses the shared global $selected_generation client identity"
        if [[ "$selected_generation" == TB1 ]]; then
            global_set_id=client-tb1-global
            global_crt_display="$tb1_client_crt_display"
        else
            global_set_id=client-tb2-global
            global_crt_display="$tb2_client_crt_display"
        fi
        global_status="${SET_RESULT["$global_set_id"]:-OK}"
        compact_result "$global_status" "Overlay $overlay_id - $selected_generation, shared global identity"
        record_overlay "$overlay_id" "$box_id" "$box_generation" "$selected_generation" shared \
            "${global_status,,}" "${SET_CN["$global_set_id"]:-}" "$global_crt_display"
        continue
    fi
    if ((override_count != 3)); then
        report ERROR "Overlay $overlay_id: partial $selected_generation client override ($override_count/3 components); CA, cert and key must be overridden together"
    else
        report OK "Overlay $overlay_id: complete explicit $selected_generation client override"
    fi

    resolve_material "$overlay_id" "$prefix.file.ca" "$prefix.data.ca"; overlay_ca="$RESOLVED_PATH"; overlay_ca_display="$RESOLVED_DISPLAY"
    resolve_material "$overlay_id" "$prefix.file.crt" "$prefix.data.crt"; overlay_crt="$RESOLVED_PATH"; overlay_crt_display="$RESOLVED_DISPLAY"
    resolve_material "$overlay_id" "$prefix.file.key" "$prefix.data.key"; overlay_key="$RESOLVED_PATH"; overlay_key_display="$RESOLVED_DISPLAY"
    directory_box_id "$(basename "$(dirname "$overlay_crt")")"; path_box_id="$REPLY"
    overlay_active=0
    if [[ "$selected_generation" == TB1 ]]; then
        effective "$overlay_id" cloud.enabled; is_true "$REPLY" && overlay_active=1
    else
        effective "$overlay_id" cloud.tb2_enabled; overlay_proxy="$REPLY"
        effective "$overlay_id" cloud.tb2_v3_enabled; overlay_v3="$REPLY"
        if is_true "$overlay_proxy" || is_true "$overlay_v3" || is_true "$mqtt_upstream_value"; then overlay_active=1; fi
    fi
    check_cert_set "overlay:$overlay_id" "Overlay $overlay_id $selected_generation upstream client" "$overlay_active" "$selected_generation" \
        "$overlay_ca" "$overlay_ca_display" "$overlay_crt" "$overlay_crt_display" \
        "$overlay_key" "$overlay_key_display" client '' "$box_id" "$path_box_id"
    detected="${SET_GENERATION["overlay:$overlay_id"]:-UNKNOWN}"
    if ((override_count != 3)); then
        SET_RESULT["overlay:$overlay_id"]=ERROR
    fi
    if [[ "$box_generation" != UNKNOWN && "$detected" != UNKNOWN && "$detected" != "$box_generation" ]]; then
        report ERROR "Overlay $overlay_id: boxGeneration=$box_generation conflicts with certificate generation=$detected"
        SET_RESULT["overlay:$overlay_id"]=ERROR
    fi
    compact_set_result "overlay:$overlay_id" "Overlay $overlay_id $selected_generation$overlay_generation_note" "$overlay_active" "$overlay_crt_display"
    overlay_result="${SET_RESULT["overlay:$overlay_id"]:-INFO}"
    record_overlay "$overlay_id" "$box_id" "$box_generation" "$selected_generation" override \
        "${overlay_result,,}" "${SET_CN["overlay:$overlay_id"]:-}" "$overlay_crt_display"
done < <(printf '%s\n' "${!OVERLAY_IDS[@]}" | sort)

section 'Canonical and legacy directory inventory'
for dir_name in server_tb1 server_tb2 client_tb1 client_tb2; do
    if [[ -d "$CERT_ROOT/$dir_name" ]]; then
        count="$(find "$CERT_ROOT/$dir_name" -type f ! -name .gitkeep 2>/dev/null | wc -l | tr -d ' ')"
        report INFO "$dir_name: $count file(s)"
    else
        report WARN "$dir_name: canonical directory missing"
    fi
done
check_ca_representations server_tb1
check_ca_representations server_tb2
check_legacy_tree server server_tb1
check_legacy_tree client client_tb1
check_hidden_certificate_backups
check_operational_legacy_paths

section 'Client certificate inventory'
inventory_client_tree TB1 client_tb1
inventory_client_tree TB2 client_tb2

section 'Local fake-client inventory'
inventory_fake_tree client_tb1
inventory_fake_tree client_tb2

emit_json() {
    local result=ok clean_roles=0 index finding level message comma status severity scope path
    if ((ERROR_COUNT > 0)); then
        result=error
    elif ((WARN_COUNT > 0)); then
        result=warning
    fi
    for status in "${ROLE_STATUS[@]}"; do
        status="$(normalize_severity "$status")"
        [[ "$status" == ok || "$status" == info ]] && clean_roles=$((clean_roles + 1))
    done

    printf '{"schemaVersion":1,"result":'
    json_string "$result"
    printf ',"summary":{"roles":%d,"ok":%d,"warnings":%d,"errors":%d}' \
        "${#ROLE_IDS[@]}" "$clean_roles" "$WARN_COUNT" "$ERROR_COUNT"

    printf ',"roles":['
    comma=''
    for ((index = 0; index < ${#ROLE_IDS[@]}; index++)); do
        printf '%s{"id":' "$comma"; json_string "${ROLE_IDS[$index]}"
        printf ',"label":'; json_string "${ROLE_LABELS[$index]}"
        printf ',"active":'; json_bool "${ROLE_ACTIVE[$index]}"
        printf ',"status":'; json_string "$(normalize_severity "${ROLE_STATUS[$index]}")"
        printf ',"generation":'; json_string "${ROLE_GENERATION[$index]}"
        printf ',"cn":'; json_string "${ROLE_CN[$index]}"
        printf ',"path":'; json_string "${ROLE_PATH[$index]}"
        printf '}'
        comma=','
    done
    printf ']'

    printf ',"overlays":['
    comma=''
    for ((index = 0; index < ${#OVERLAY_KEYS[@]}; index++)); do
        printf '%s{"id":' "$comma"; json_string "${OVERLAY_KEYS[$index]}"
        printf ',"boxId":'; json_string "${OVERLAY_BOX_IDS[$index]}"
        printf ',"configuredGeneration":'; json_string "${OVERLAY_CONFIGURED_GENERATION[$index]}"
        printf ',"selectedGeneration":'; json_string "${OVERLAY_SELECTED_GENERATION[$index]}"
        printf ',"identitySource":'; json_string "${OVERLAY_IDENTITY_SOURCE[$index]}"
        printf ',"status":'; json_string "$(normalize_severity "${OVERLAY_STATUS[$index]}")"
        printf ',"cn":'; json_string "${OVERLAY_CN[$index]}"
        printf ',"path":'; json_string "${OVERLAY_PATH[$index]}"
        printf '}'
        comma=','
    done
    printf ']'

    printf ',"findings":['
    comma=''
    for ((index = 0; index < ${#COMPACT_FINDINGS[@]}; index++)); do
        finding="${COMPACT_FINDINGS[$index]}"
        level="${finding%%|*}"
        message="${finding#*|}"
        severity="$(normalize_severity "$level")"
        scope="$(diagnostic_scope "$message")"
        path="$(diagnostic_path "$message" "$scope")"
        printf '%s{"severity":' "$comma"; json_string "$severity"
        printf ',"code":'; json_string "doctor.$severity.$(printf '%03d' $((index + 1)))"
        printf ',"scope":'; json_string "$scope"
        printf ',"path":'; json_string "$path"
        printf ',"message":'; json_string "$message"
        printf '}'
        comma=','
    done
    printf ']'

    printf ',"checks":['
    comma=''
    for ((index = 0; index < ${#CHECK_LEVELS[@]}; index++)); do
        message="${CHECK_MESSAGES[$index]}"
        severity="$(normalize_severity "${CHECK_LEVELS[$index]}")"
        scope="$(diagnostic_scope "$message")"
        path="$(diagnostic_path "$message" "$scope")"
        printf '%s{"id":%d,"severity":' "$comma" "$((index + 1))"
        json_string "$severity"
        printf ',"scope":'; json_string "$scope"
        printf ',"path":'; json_string "$path"
        printf ',"message":'; json_string "$message"
        printf '}'
        comma=','
    done
    printf ']}\n'
}

if ((JSON_MODE)); then
    emit_json
elif ((VERBOSE)); then
    section 'Summary'
    printf '  OK=%d WARN=%d ERROR=%d INFO=%d\n' "$OK_COUNT" "$WARN_COUNT" "$ERROR_COUNT" "$INFO_COUNT"
else
    compact_section 'Findings'
    if ((${#COMPACT_FINDINGS[@]} == 0)); then
        print_status OK 'No certificate or configuration problems found.'
    else
        for finding in "${COMPACT_FINDINGS[@]}"; do
            level="${finding%%|*}"
            message="${finding#*|}"
            print_status "$level" "$message"
        done
    fi
    compact_section 'Summary'
    printf '  Roles/overlays: OK=%d WARN=%d ERROR=%d INFO=%d\n' \
        "$ROLE_OK_COUNT" "$ROLE_WARN_COUNT" "$ROLE_ERROR_COUNT" "$ROLE_INFO_COUNT"
    printf '  Findings: WARN=%d ERROR=%d\n' "$WARN_COUNT" "$ERROR_COUNT"
    printf '  Run again with --verbose for all individual checks.\n'
fi
if ((!JSON_MODE)); then
    printf '  Base path: %s\n' "$BASE_PATH"
    printf '  This doctor made no changes.\n'
fi

if ((ERROR_COUNT > 0)); then
    exit 2
fi
if ((WARN_COUNT > 0)); then
    exit 1
fi
exit 0
