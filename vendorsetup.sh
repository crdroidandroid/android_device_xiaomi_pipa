#!/bin/bash

# ──────────────────────────────────────────────────────────────
#  ENVIRONMENT SETUP | DEVICE: PIPA
# ──────────────────────────────────────────────────────────────

# 🎨 Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
WHITE='\033[1;37m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# ⚙️ Configuration
KERNEL_REPO="https://github.com/gensis01/kernel_xiaomi_pipa"
KERNEL_BRANCH="16"
ROOT_DIR=$(pwd)

# 🛠️ Helper Functions
info()    { echo -e "${BLUE}${BOLD} [INFO] ${NC} $1"; }
success() { echo -e "${GREEN}${BOLD} [DONE] ${NC} $1"; }
warn()    { echo -e "${YELLOW}${BOLD} [WARN] ${NC} $1"; }
error()   { echo -e "${RED}${BOLD} [ERR]  ${NC} $1"; }

clone_repo() {
    local url=$1
    local branch=$2
    local target=$3
    local type=$4

    if [[ "$type" == "fresh" ]]; then
        if [ -d "$target" ]; then
            rm -rf "$target"
        fi
        info "Cloning ${BOLD}$target${NC} (Fresh)..."
    else
        if [ -d "$target" ]; then
            warn "${BOLD}$target${NC} already exists. Skipping."
            return 0
        fi
        info "Cloning ${BOLD}$target${NC}..."
    fi

    git clone --depth=2 "$url" -b "$branch" "$target" -q

    if [ $? -eq 0 ]; then
        success "Successfully cloned $target"
    else
        error "Failed to clone $target"
        return 1
    fi
}

apply_patch() {
    local name=$1
    local patch_path=$2
    local target_dir=$3
    local tmp_patch="/tmp/temp_patch.$$"

    info "Processing patch: ${BOLD}$name${NC}..."

    if [ ! -f "$patch_path" ]; then
        error "Patch file not found: $patch_path"
        return 1
    fi

    local current_dir=$(pwd)
    cd "$target_dir" || { error "Could not enter $target_dir"; return 1; }

    # Clean CRLF (Windows) line endings
    tr -d '\r' < "$patch_path" > "$tmp_patch"

    if git apply --check "$tmp_patch" >/dev/null 2>&1; then
        if git apply "$tmp_patch" >/dev/null 2>&1; then
            git add .
            git commit -m "Apply $name" -q || true
            success "$name applied and committed successfully."
        else
            warn "Failed to apply $name cleanly. Resetting..."
            git reset --hard HEAD >/dev/null 2>&1
            git clean -fd >/dev/null 2>&1
        fi
    else
        warn "$name seems to be already applied or has conflicts."
    fi

    rm -f "$tmp_patch"
    cd "$current_dir" || return
}

setup_firmware() {
    local target_dir="vendor/xiaomi/pipa"
    local fw_url="https://github.com/gensis01/vendor_xiaomi_pipa/releases/download/fw-radio-OS2.0.12.0.UMZCNXM-pipa/OS2.0.12.0.UMZCNXM-pipa.zip"
    local tmp_zip="/tmp/OS2.0.12.0.UMZCNXM-pipa.zip"
    local tmp_extract="/tmp/pipa_fw_extract"

    echo -e "${BOLD}──────────────────────────────────────────────────────────────${NC}"
    info "Setting up Firmware (Radio)..."

    # Ensure target dir exists (it should have been cloned by now)
    mkdir -p "$target_dir"
    
    # Clean old files
    if [ -d "$target_dir/radio" ]; then
        rm -rf "$target_dir/radio"
    fi
    rm -rf "$tmp_extract" "$tmp_zip"

    # Download
    if command -v curl >/dev/null; then
        curl -L -o "$tmp_zip" "$fw_url" --progress-bar || { error "Download failed"; return 1; }
    elif command -v wget >/dev/null; then
        wget -q --show-progress -O "$tmp_zip" "$fw_url" || { error "Download failed"; return 1; }
    else
        error "No download tool (curl/wget) found!"
        return 1
    fi

    # Extract
    mkdir -p "$tmp_extract"
    info "Extracting firmware..."
    if command -v unzip >/dev/null; then
        unzip -q -o "$tmp_zip" -d "$tmp_extract"
    elif command -v bsdtar >/dev/null; then
        bsdtar -xf "$tmp_zip" -C "$tmp_extract"
    else
        error "No unzip tool found!"
        rm -f "$tmp_zip"
        return 1
    fi

    # Move Radio Folder
    local radio_src
    radio_src=$(find "$tmp_extract" -type d -name radio -print -quit)

    if [ -n "$radio_src" ]; then
        mv "$radio_src" "$target_dir/"
        success "Firmware radio moved to $target_dir/radio"
    else
        error "Radio folder not found in the downloaded zip!"
    fi

    # Cleanup
    rm -f "$tmp_zip"
    rm -rf "$tmp_extract"
}

# ──────────────────────────────────────────────────────────────
# 🚀 MAIN EXECUTION FLOW
# ──────────────────────────────────────────────────────────────

# STEP 1: CLONE ALL REPOSITORIES FIRST
echo -e "${BOLD}>>> 1. CLONING ALL REPOSITORIES${NC}"

# Kernel
clone_repo "$KERNEL_REPO" "$KERNEL_BRANCH" "kernel/xiaomi/sm8250"

# Dependencies
clone_repo "https://github.com/gensis01/android_device_xiaomi_sm8250-common" "16" "device/xiaomi/sm8250-common"
clone_repo "https://github.com/gensis01/vendor_xiaomi_sm8250-common" "16" "vendor/xiaomi/sm8250-common"
clone_repo "https://github.com/gensis01/vendor_xiaomi_pipa" "16" "vendor/xiaomi/pipa"
clone_repo "https://github.com/PocoF3Releases/vendor_qcom_wfd.git" "bka" "vendor/qcom/wfd"
clone_repo "https://github.com/PocoF3Releases/device_qcom_wfd.git" "bka" "device/qcom/wfd"
clone_repo "https://github.com/gensis01/hardware_xiaomi.git" "aosp-16" "hardware/xiaomi" "fresh"
clone_repo "https://github.com/PocoF3Releases/packages_resources_devicesettings.git" "aosp-16" "packages/resources/devicesettings" "fresh"

echo -e "${GREEN}${BOLD}✔ All repositories cloned successfully.${NC}"


# STEP 2: APPLY PATCHES (Only runs after cloning is done)
echo -e "\n${BOLD}>>> 2. APPLYING PATCHES${NC}"
DEVICE_PATH="device/xiaomi/pipa"
mkdir -p "$DEVICE_PATH/patches"

apply_patch "Tablet FWB Patch" "$ROOT_DIR/$DEVICE_PATH/patches/tablet-fwb.patch" "frameworks/base"


# STEP 3: SETUP FIRMWARE (Only runs after cloning and patching)
echo -e "\n${BOLD}>>> 3. SETUP FIRMWARE${NC}"
setup_firmware


# FINAL: BUILD TIME BANNER
echo -e ""
echo -e "${WHITE}  ____        _ _     _   _______ _                 ${NC}"
echo -e "${WHITE} |  _ \      (_) |   | | |__   __(_)                ${NC}"
echo -e "${WHITE} | |_) |_   _ _| | __| |    | |   _ _ __ ___   ___  ${NC}"
echo -e "${WHITE} |  _ <| | | | | |/ _\` |    | |  | | '_ \` _ \ / _ \ ${NC}"
echo -e "${WHITE} | |_) | |_| | | | (_| |    | |  | | | | | | |  __/ ${NC}"
echo -e "${WHITE} |____/ \__,_|_|_|\__,_|    |_|  |_|_| |_| |_|\___| ${NC}"
echo -e "${WHITE}                                       .            ${NC}"
echo -e ""
