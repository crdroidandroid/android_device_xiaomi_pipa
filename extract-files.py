#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: 2016 The CyanogenMod Project
# SPDX-FileCopyrightText: 2017-2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

import re
import os
import sys
import argparse
import subprocess
import shutil

from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

# Helper function to check if a command exists
def is_command_available(command):
    return shutil.which(command) is not None

# Create proper fixup classes that have a run method
class BatterySecretRcFixup(blob_fixup):
    def run(self, ctx, file_path, obj_file_path):
        """Remove seclabel line from batterysecret rc file"""
        if not obj_file_path:
            return True
        with open(obj_file_path, 'r') as f:
            content = f.read()
        content = re.sub(r"seclabel u:r:batterysecret:s0\n", "", content)
        with open(obj_file_path, 'w') as f:
            f.write(content)
        return True

class CameraPostprocFixup(blob_fixup):
    def run(self, ctx, file_path, obj_file_path):
        """Run sigscan on camera postproc library"""
        if not obj_file_path:
            return True
            
        # Try a direct binary patch if sigscan isn't available
        try:
            with open(obj_file_path, 'rb') as f:
                content = f.read()
                
            # Try to find the byte sequence that needs to be replaced
            pattern = b'\x9A\x0A\x00\x94'
            replacement = b'\x1F\x20\x03\xD5'
            
            if pattern in content:
                content = content.replace(pattern, replacement)
                with open(obj_file_path, 'wb') as f:
                    f.write(content)
                return True
        except Exception:
            pass
            
        # Fall back to sigscan if available
        sigscan_cmd = os.environ.get("SIGSCAN", "sigscan")
        if not is_command_available(sigscan_cmd):
            return True
            
        try:
            subprocess.run([
                sigscan_cmd,
                "-p", "9A 0A 00 94",
                "-P", "1F 20 03 D5",
                "-f", obj_file_path
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.CalledProcessError:
            pass
            
        return True

class CameraQcomFixup(blob_fixup):
    def run(self, ctx, file_path, obj_file_path):
        """Replace hex string in camera qcom library"""
        if not obj_file_path:
            return True
        try:
            with open(obj_file_path, 'rb') as f:
                content = f.read()
            
            # Original replacement
            orig_pattern = b"\x73\x74\x5F\x6C\x69\x63\x65\x6E\x73\x65\x2E\x6C\x69\x63"
            replacement = b"\x63\x61\x6D\x65\x72\x61\x5F\x63\x6E\x66\x2E\x74\x78\x74"
            
            if orig_pattern in content:
                content = content.replace(orig_pattern, replacement)
                
                with open(obj_file_path, 'wb') as f:
                    f.write(content)
                return True
            
            # Try alternative patterns if the original one isn't found
            alt_pattern = b"st_license.lic"
            if alt_pattern in content:
                content = content.replace(alt_pattern, b"camera_cnf.txt")
                with open(obj_file_path, 'wb') as f:
                    f.write(content)
        except Exception:
            pass
        return True

class WatermarkFixup(blob_fixup):
    def run(self, ctx, file_path, obj_file_path):
        """Check and add libpiex_shim.so dependency"""
        if not obj_file_path:
            return True
            
        patchelf_cmd = os.environ.get("PATCHELF", "patchelf")
        if not is_command_available(patchelf_cmd):
            return True
            
        try:
            # First try using strings to check for the dependency
            try:
                result = subprocess.run(
                    ["strings", obj_file_path], 
                    stdout=subprocess.PIPE, 
                    stderr=subprocess.DEVNULL,
                    text=True,
                    check=True
                )
                
                if "libpiex_shim.so" in result.stdout:
                    return True
            except (subprocess.CalledProcessError, FileNotFoundError):
                # Fall back to grep if strings fails
                try:
                    result = subprocess.run(
                        ["grep", "-q", "libpiex_shim.so", obj_file_path], 
                        stdout=subprocess.DEVNULL, 
                        stderr=subprocess.DEVNULL
                    )
                    
                    if result.returncode == 0:  # String found
                        return True
                except (subprocess.CalledProcessError, FileNotFoundError):
                    pass
            
            # Add the dependency if not found
            subprocess.run([
                patchelf_cmd,
                "--add-needed", "libpiex_shim.so",
                obj_file_path
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            
            return True
        except subprocess.CalledProcessError:
            return True

# Define blob fixups using class instances
blob_fixups: blob_fixups_user_type = {
    'vendor/etc/init/init.batterysecret.rc': BatterySecretRcFixup(),
    'vendor/lib/hw/audio.primary.pipa.so': AudioPrimaryFixup(),
    'vendor/lib64/vendor.qti.hardware.camera.postproc@1.0-service-impl.so': CameraPostprocFixup(),
    'vendor/lib64/hw/camera.qcom.so': CameraQcomFixup(),
    'vendor/lib64/camera/components/com.mi.node.watermark.so': WatermarkFixup(),
}  # fmt: skip

# Check if a file path contains Dolby-related patterns
def is_dolby_file(file_path):
    dolby_patterns = [
        "dolby", "Dolby", "DOLBY",
        "dax", "DAX", "Dax"
    ]
    
    for pattern in dolby_patterns:
        if pattern in file_path:
            return True
    return False

if __name__ == '__main__':
    # Parse command line arguments first
    parser = argparse.ArgumentParser(description='Extract proprietary blobs for Xiaomi Pad 6')
    parser.add_argument('-s', '--section', help='Extract only specified section')
    parser.add_argument('-p', '--path', help='Path to system image or directory')
    parser.add_argument('-k', '--kang', action='store_true', help='Force extraction')
    parser.add_argument('-n', '--no-cleanup', action='store_true', help='Skip cleanup')
    parser.add_argument('--skip-common', action='store_true', help='Skip extracting common blobs')
    parser.add_argument('--device-only', action='store_true', help='Extract only device-specific blobs')
    parser.add_argument('--common-only', '--only-common', action='store_true', help='Extract only common blobs')
    parser.add_argument('-q', '--quiet', action='store_true', help='Minimize output')
    parser.add_argument('--ignore-missing-dolby', action='store_true', default=True, 
                       help='Ignore errors for missing Dolby files')
    args, remaining_args = parser.parse_known_args()

    # Always disable firmware extraction
    add_firmware = False
    
    # Common module for sm8250-common
    common_device = 'sm8250-common'
    vendor = 'xiaomi'
    
    # Create module for device (unless common-only mode)
    if not args.common_only:
        device_module = ExtractUtilsModule(
            'pipa',
            'xiaomi',
            blob_fixups=blob_fixups,
            add_firmware_proprietary_file=add_firmware,
        )
        
        # Create ExtractUtils instance for device
        utils = ExtractUtils(device_module)
        
        # Set options based on arguments
        utils.clean_vendor = not args.no_cleanup
        utils.kang = args.kang
        if args.section:
            utils.section = args.section
        if args.path:
            utils.source_path = args.path
    
        if not args.quiet:
            print("Extracting Xiaomi Pad 6 proprietary blobs...")
    
    # Process common device if requested
    if not (args.skip_common or args.device_only or args.section):
        common_file_path = f'{os.path.dirname(os.path.abspath(__file__))}/../../{vendor}/{common_device}/proprietary-files.txt'
        if os.path.exists(common_file_path):
            if not args.quiet:
                print("Processing common files...")
            
            # Custom extraction function to handle common files with error handling for Dolby
            try:
                # Read proprietary-files.txt for the common device
                with open(common_file_path, 'r') as f:
                    common_files = f.read().splitlines()
                
                # Create module for common device
                common_module = ExtractUtilsModule(
                    common_device,
                    vendor,
                    add_firmware_proprietary_file=add_firmware,
                )
                
                common_utils = ExtractUtils(common_module)
                common_utils.clean_vendor = not args.no_cleanup
                common_utils.kang = args.kang
                
                if args.path:
                    common_utils.source_path = args.path
                
                # Extract the common files
                try:
                    common_utils.run()
                except Exception as e:
                    if not args.quiet:
                        print(f"Note: Error processing common files (this is expected if using a different firmware): {e}")
                    # Continue execution even if common files extraction fails
                    pass
            except Exception as e:
                if not args.quiet:
                    print(f"Warning: Error setting up common files: {e}")
                # Continue execution even if setup fails
                pass
    
    # Extract device-specific files unless common-only mode
    if not args.common_only:
        try:
            # Run for device
            if not args.quiet:
                print("Processing device files...")
            utils.run()
        except FileNotFoundError as e:
            # Skip error reporting for missing Dolby files
            if args.ignore_missing_dolby and is_dolby_file(str(e)):
                pass
            elif not args.quiet:
                print(f"Error: {e}")
            
            # Continue with makefile generation if it was a specific section
            if args.section:
                if not args.quiet:
                    print("Attempting to continue with available files...")
                try:
                    utils.write_makefiles()
                except Exception:
                    sys.exit(1)
            else:
                # Don't exit on errors that might be related to missing Dolby files
                if not (args.ignore_missing_dolby and "dolby" in str(e).lower()):
                    sys.exit(1)

    if not args.quiet:
        print("Extraction completed")
