#!/bin/bash

# Exit immediately if a command exits with a non-zero status.
set -e

# Function to clone if directory doesn't exist
clone_if_missing() {
    local repo_url=$1
    local branch=$2
    local target_dir=$3
    
    if [ -z "$repo_url" ] || [ -z "$branch" ] || [ -z "$target_dir" ]; then
        echo "Usage: clone_if_missing <repo_url> <branch> <target_dir>"
        return 1
    fi

    if [ ! -d "$target_dir" ]; then
        echo "Cloning $target_dir..."
        git clone "$repo_url" -b "$branch" "$target_dir" -q || { echo "Error: Failed to clone $repo_url."; return 1; }
        echo "Done."
    else
        echo "Directory $target_dir already exists, skipping clone."
    fi
    return 0
}

# Function to perform a clean clone (removes existing directory first)
clean_clone() {
    local repo_url=$1
    local branch=$2
    local target_dir=$3

    # Validate inputs
    if [ -z "$repo_url" ] || [ -z "$branch" ] || [ -z "$target_dir" ]; then
        echo "Usage: clean_clone <repo_url> <branch> <target_dir>"
        return 1 # Indicate an error
    fi

    echo "Attempting to clean clone $repo_url (branch: $branch) into $target_dir..."

    # Step 1: Remove existing directory if it exists
    if [ -d "$target_dir" ]; then
        echo "Removing existing directory: $target_dir"
        rm -rf "$target_dir" || { echo "Error: Failed to remove existing directory $target_dir. Aborting." ; return 1; }
        echo "Existing directory removed."
    fi

    # Step 2: Perform the clone
    echo "Cloning $repo_url..."
    git clone "$repo_url" -b "$branch" "$target_dir" -q
    if [ $? -eq 0 ]; then
        echo "Clean clone complete for $target_dir."
        return 0 # Success
    else
        echo "Error: Failed to clone $repo_url (branch: $branch) into $target_dir."
        return 1 # Failure
    fi
}


# Git clones (using clone_if_missing by default, as it's generally safer for initial setup)
echo "Setting up repositories..."
clone_if_missing "https://github.com/glitch-wraith/android_device_xiaomi_sm8250-common" "15-qpr2" "device/xiaomi/sm8250-common"
clone_if_missing "https://github.com/glitch-wraith/android_kernel_xiaomi_sm8250" "axksu" "kernel/xiaomi/sm8250"
clone_if_missing "https://github.com/glitch-wraith/proprietary_vendor_xiaomi_sm8250-common" "15" "vendor/xiaomi/sm8250-common"
clone_if_missing "https://github.com/glitch-wraith/proprietary_vendor_xiaomi_pipa" "15" "vendor/xiaomi/pipa"

# Additional repos
clone_if_missing "https://github.com/LineageOS/android_hardware_xiaomi" "lineage-22.2" "hardware/xiaomi"
clone_if_missing "https://github.com/LineageOS/android_hardware_lineage_compat" "lineage-22.2" "hardware/lineage/compat"
clone_if_missing "https://github.com/LineageOS/android_hardware_lineage_interfaces" "lineage-22.2" "hardware/lineage/interfaces"
clone_if_missing "https://github.com/LineageOS/android_hardware_lineage_livedisplay" "lineage-22.2" "hardware/lineage/livedisplay"
clone_if_missing "https://github.com/Matrixx-Devices/hardware_dolby.git" "sony-1.3" "hardware/dolby"

# Apply recovery patch
apply_recovery_patch() {
    local root_dir=$(pwd)
    local target_dir="bootable/recovery"
    local patch_path="${root_dir}/device/xiaomi/pipa/source-patches/atomic-recovery.diff"
    local temp_patch="/tmp/atomic-recovery.patch" # Use a temp file for safety

    echo "Applying recovery patch..."
    
    if [ ! -f "$patch_path" ]; then
        echo "Error: Patch file not found: ${patch_path}"
        return 1
    fi
    
    # Change to target directory for patch application
    cd "$target_dir" || { echo "Error: Cannot CD into $target_dir. Aborting patch." ; return 1; }

    # --- Patch Check Logic ---
    local patch_marker_message="Applied recovery patch"
    local already_patched=false

    # Create a temporary patch file with corrected line endings for checks and application
    tr -d '\r' < "$patch_path" > "$temp_patch" || { echo "Error: Failed to process patch file line endings." ; cd "$root_dir" ; return 1; }
    
    # 1. Check if the specific commit message exists in the history
    if git log --grep="$patch_marker_message" -n 1 --pretty=format:"%s" &>/dev/null; then
        already_patched=true
        echo "Detected commit '$patch_marker_message' in $target_dir history."
    fi

    # 2. Check if the patch can be reversed (implying it's currently applied)
    # This is useful even if the commit message isn't found, e.g., if applied manually.
    if ! $already_patched && git apply --check --reverse --ignore-whitespace "$temp_patch" &>/dev/null; then
        already_patched=true
        echo "Patch file appears to be applied (reverse check successful)."
    fi

    if $already_patched; then
        read -p "Recovery patch already appears to be applied. Skip re-application? (y/N): " choice
        case "$choice" in
            [yY]|[yY][eE][sS])
                echo "Skipping recovery patch application."
                rm -f "$temp_patch" # Clean up temp patch
                cd "$root_dir"
                return 0
                ;;
            *)
                echo "Proceeding with patch application (will attempt to re-apply/update)."
                ;;
        esac
    fi
    # --- End Patch Check Logic ---

    echo "Attempting to apply patch..."
    # Try different patch methods (git am is usually best for well-formed patches)
    if git am --3way "$temp_patch"; then
        echo "Patch applied successfully with git am."
    elif git apply --check --ignore-whitespace "$temp_patch" && git apply --ignore-whitespace "$temp_patch"; then
        git add .
        git commit -m "$patch_marker_message" -q || true # Commit, ignore if no changes were actually made
        echo "Patch applied successfully with git apply and committed."
    else
        echo "Warning: git am and git apply failed. Attempting with standard patch command..."
        # Try standard patch with different strip levels
        for level in 1 0 2; do
            if patch -p${level} --ignore-whitespace --no-backup-if-mismatch < "$temp_patch"; then
                git add . || true
                git commit -m "$patch_marker_message (via patch -p$level)" -q || true
                echo "Patch applied successfully with patch -p$level."
                break
            fi
        done
        
        # If still failed, or partially applied
        if ! git diff --name-only | grep -q .; then # No changes in working tree means it completely failed or already applied
            echo "Failed to apply patch using any method, or no changes were necessary."
            git reset --hard HEAD >/dev/null 2>&1 || true # Clean up working dir in case of partial application
            rm -f "$temp_patch"
            cd "$root_dir"
            return 1
        else
            git add . || true
            git commit -m "$patch_marker_message (partial/manual)" -q || true
            echo "Patch partially applied or required manual intervention. Please review."
        fi
    fi
    
    rm -f "$temp_patch" # Clean up temp patch
    cd "$root_dir"
    return 0
}

# Download and extract firmware
setup_firmware() {
    local root_dir=$(pwd)
    local firmware_url="https://github.com/glitch-wraith/vendor_xiaomi_pipa/releases/download/fw-radio-OS2.0.3.0.UMZMIXM/vendor-pipa-fw-included.zip"
    local target_dir="${root_dir}/vendor/xiaomi"
    local temp_zip=$(mktemp /tmp/pipa_firmware_XXXXXX.zip) # Use mktemp for unique temp file
    local radio_dir="${target_dir}/pipa/radio"
    
    # Check if firmware already exists by checking key files
    if [ -d "$radio_dir" ] && [ -f "${radio_dir}/abl.img" ] && [ -f "${radio_dir}/xbl.img" ]; then
        echo "Firmware already present, skipping download."
        return 0
    fi
    
    echo "Downloading firmware from $firmware_url..."
    mkdir -p "$target_dir" || { echo "Error: Failed to create firmware target directory."; return 1; }
    
    curl -sL "$firmware_url" -o "$temp_zip"
    if [ ! -s "$temp_zip" ]; then # Check if file exists and is not empty
        echo "Error: Failed to download firmware from $firmware_url"
        rm -f "$temp_zip"
        return 1
    fi
    
    echo "Extracting firmware to $target_dir..."
    unzip -qo "$temp_zip" -d "$target_dir" || { echo "Error: Failed to extract firmware."; rm -f "$temp_zip"; return 1; }
    rm -f "$temp_zip"
    echo "Firmware setup complete."
    return 0
}

# Main script execution
ROOT_DIR=$(pwd)
DEVICE_PATH="${ROOT_DIR}/device/xiaomi/pipa"

# Check if device directory exists (after initial clones)
if [ ! -d "$DEVICE_PATH" ]; then
    echo "Error: Device directory 'device/xiaomi/pipa' not found. Please ensure repositories are properly cloned."
    exit 1
fi

# Create patches directory if it doesn't exist
mkdir -p "${DEVICE_PATH}/source-patches" || { echo "Error: Failed to create source-patches directory."; exit 1; }

# Apply patches
apply_recovery_patch || { echo "Recovery patch application failed."; exit 1; }

# Setup firmware
setup_firmware || { echo "Firmware setup failed."; exit 1; }

echo "-------------------------------------"
echo "           Setup complete!           "
echo "-------------------------------------"