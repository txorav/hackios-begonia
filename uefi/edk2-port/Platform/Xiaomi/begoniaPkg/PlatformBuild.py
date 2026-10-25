#!/usr/bin/env python3
import datetime
import logging
import os
import uuid
import subprocess
import gzip
import shutil
import logging

from io import StringIO
from pathlib import Path

from edk2toolext import edk2_logging
from edk2toolext.environment import shell_environment
from edk2toolext.environment.uefi_build import UefiBuilder
from edk2toolext.invocables.edk2_platform_build import BuildSettingsManager
from edk2toolext.invocables.edk2_pr_eval import PrEvalSettingsManager
from edk2toolext.invocables.edk2_setup import (RequiredSubmodule, SetupSettingsManager)
from edk2toolext.invocables.edk2_update import UpdateSettingsManager
from edk2toolext.invocables.edk2_parse import ParseSettingsManager
from edk2toollib.utility_functions import RunCmd

# ####################################################################################### #
#                                Common Configuration                                     #
# ####################################################################################### #
class CommonPlatform ():
    PackagesSupported = ("begoniaPkg")
    ArchSupported = ("AARCH64")
    TargetsSupported = ("DEBUG", "RELEASE")
    Scopes = ('t1100', 'gcc_aarch64_linux', 'edk2-build')
    WorkspaceRoot = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    PackagesPath = (
        "Platform/Xiaomi",
        "Common/edk2",
        "Common/edk2-platforms",
        "Silicon/MediaTek"
    )

# ####################################################################################### #
#                         Configuration for Update & Setup                                #
# ####################################################################################### #
class SettingsManager (UpdateSettingsManager, SetupSettingsManager, PrEvalSettingsManager, ParseSettingsManager):

    def GetPackagesSupported (self):
        return CommonPlatform.PackagesSupported

    def GetArchitecturesSupported (self):
        return CommonPlatform.ArchSupported

    def GetTargetsSupported (self):
        return CommonPlatform.TargetsSupported

    def GetRequiredSubmodules (self):
        return [
            RequiredSubmodule ("Common/edk2", True),
        ]

    def SetArchitectures (self, list_of_requested_architectures):
        unsupported = set(list_of_requested_architectures) - set(self.GetArchitecturesSupported())

        if (len(unsupported) > 0):
            errorString = ("Unsupported Architecture Requested: " + " ".join(unsupported))
            logging.critical (errorString)
            raise Exception (errorString)

        self.ActualArchitectures = list_of_requested_architectures

    def GetWorkspaceRoot (self):
        return CommonPlatform.WorkspaceRoot

    def GetActiveScopes (self):
        return CommonPlatform.Scopes

    def FilterPackagesToTest (self, changedFilesList: list, potentialPackagesList: list) -> list:
        build_these_packages = []
        possible_packages = potentialPackagesList.copy ()

        for f in changedFilesList:
            if "BaseTools" in f:
                if os.path.splitext(f) not in [".txt", ".md"]:
                    build_these_packages = possible_packages
                    break

            if "platform-build-run-steps.yml" in f:
                build_these_packages = possible_packages
                break

        return build_these_packages

    def GetPlatformDscAndConfig (self) -> tuple:
        return ("begoniaPkg/begonia.dsc", {})

    def GetName (self):
        return "begonia"

    def GetPackagesPath (self):
        return CommonPlatform.PackagesPath

# ####################################################################################### #
#                         Actual Configuration for Platform Build                         #
# ####################################################################################### #
class PlatformBuilder (UefiBuilder, BuildSettingsManager):
    def __init__ (self):
        UefiBuilder.__init__ (self)

    def AddCommandLineOptions (self, parserObj):
        parserObj.add_argument('-a', "--arch", dest="build_arch", type=str, default="AARCH64", help="Optional - CSV of architecture to build. AARCH64 is used for PEI and DXE and is the only valid option for this platform.")

    def RetrieveCommandLineOptions (self, args):
        if args.build_arch.upper() != "AARCH64":
            raise Exception("Invalid Arch Specified.  Please see comments in PlatformBuild.py::PlatformBuilder::AddCommandLineOptions")

    def GetWorkspaceRoot (self):
        return CommonPlatform.WorkspaceRoot

    def GetPackagesPath (self):
        result = [ shell_environment.GetBuildVars().GetValue("FEATURE_CONFIG_PATH", "") ]

        for a in CommonPlatform.PackagesPath:
            result.append(a)

        return result

    def GetActiveScopes (self):
        return CommonPlatform.Scopes

    def GetName (self):
        return "begoniaPkg"

    def GetLoggingLevel (self, loggerType):
        return logging.INFO
        return super().GetLoggingLevel(loggerType)

    def GetLatestCommit (self):
        try:
            result = subprocess.run (
                ['git', 'describe', '--tags', '--always'],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            return result.stdout.strip()
        except:
            return "Unknown"

    def SetPlatformEnv (self):
        logging.debug ("PlatformBuilder SetPlatformEnv")

        git_commit = self.GetLatestCommit ()

        self.env.SetValue ("PRODUCT_NAME", "begonia", "Platform Hardcoded")
        self.env.SetValue ("ACTIVE_PLATFORM", "begoniaPkg/begonia.dsc", "Platform Hardcoded")
        self.env.SetValue ("TARGET_ARCH", "AARCH64", "Platform Hardcoded")
        self.env.SetValue ("TOOL_CHAIN_TAG", "CLANG38", "set default to clang38")
        self.env.SetValue ("EMPTY_DRIVE", "FALSE", "Default to false")
        self.env.SetValue ("RUN_TESTS", "FALSE", "Default to false")
        self.env.SetValue ("SHUTDOWN_AFTER_RUN", "FALSE", "Default to false")
        self.env.SetValue ("BLD_*_BUILDID_STRING", "Unknown", "Default")
        self.env.SetValue ("BUILDREPORTING", "TRUE", "Enabling build report")
        self.env.SetValue ("BUILDREPORT_TYPES", "PCD DEPEX FLASH BUILD_FLAGS LIBRARY FIXED_ADDRESS HASH", "Setting build report types")
        self.env.SetValue ("CLANG38_BIN", "/mnt/9f4d8733-a954-435d-b92f-f33c068c3b1e/begoker/kerdevdep/clang/bin/", "Kerdevdep Toolchain")
        self.env.SetValue ("CLANG38_AARCH64_PREFIX", "/mnt/9f4d8733-a954-435d-b92f-f33c068c3b1e/begoker/kerdevdep/gcc/bin/aarch64-linux-android-", "Kerdevdep Toolchain")
        self.env.SetValue ("BLD_*_FD_BASE", "0x50000000", "Default")
        self.env.SetValue ("BLD_*_FD_SIZE", "0x00700000", "Default")
        self.env.SetValue ("BLD_*_FIRMWARE_VER", git_commit, "Latest commit")

        return 0

    def PlatformPreBuild (self):
        return 0

    def PlatformGetOutDir (self):
        target = self.env.GetValue("TARGET")
        toolchain = self.env.GetValue("TOOL_CHAIN_TAG")
        output_dir = self.env.GetValue("OUTPUT_DIRECTORY")
        return os.path.join(output_dir, f"{target}_{toolchain}")

    def PlatformBuildBootShim (self):
        edk2_logging.log_progress("Building BootShim")

        fd_base = self.env.GetValue("BLD_*_FD_BASE")
        fd_size = self.env.GetValue("BLD_*_FD_SIZE")

        try:
            subprocess.run(
                ["make", "-C", "BootShim", f"UEFI_BASE={fd_base}", f"UEFI_SIZE={fd_size}"],
                check=True
            )
        except subprocess.CalledProcessError:
            logging.critical(f"BootShim build failed!")
            return 1

        return 0

    def PlatformPostBuildBootShim (self):
        edk2_logging.log_progress("BootShim PostBuild")

        output_dir = self.PlatformGetOutDir()
        platform = self.env.GetValue("PRODUCT_NAME")
        fd_path = os.path.join(output_dir, "FV", f"{platform.upper()}_UEFI.fd")
        shimmed_output = fd_path + ".shimmed"

        try:
            with open("BootShim/BootShim.bin", 'rb') as shim, \
                open(fd_path, 'rb') as fd, \
                open(shimmed_output, 'wb') as out_file:
                out_file.write(shim.read())
                out_file.write(fd.read())
        except Exception as e:
            logging.critical(f"Failed to create shimmed FD: {e}")
            return 1
        
        return 0

    def PlatformCompressFd (self):
        edk2_logging.log_progress("Compressing FD")

        platform = self.env.GetValue("PRODUCT_NAME")
        output_dir = self.PlatformGetOutDir()
        fd_path = os.path.join(output_dir, "FV", f"{platform.upper()}_UEFI.fd.shimmed")
        fd_gz_path = fd_path + ".gz"

        try:
            with open(fd_path, 'rb') as f_in:
                with gzip.open(fd_gz_path, 'wb') as f_out:
                    shutil.copyfileobj(f_in, f_out)
        except Exception as e:
            logging.critical(f"Gzip compression failed: {e}")
            return 1

        return 0

    def PlatformBuildBootImage (self):
        edk2_logging.log_progress("Building boot.img")

        platform = self.env.GetValue("PRODUCT_NAME")
        bootimg_out = os.path.join("Build", f"boot-{platform}.img")

        fd_path = os.path.join(self.PlatformGetOutDir(), "FV", f"{platform.upper()}_UEFI.fd.shimmed.gz")

        mkbootimg_script = os.path.abspath(os.path.join(CommonPlatform.WorkspaceRoot, "..", "..", "tools", "mkbootimg.py"))
        dtb_path = os.path.abspath(os.path.join(CommonPlatform.WorkspaceRoot, "..", "..", "device_dumps", "unpacked", "dtb"))

        try:
            cmd = [
                "python3", mkbootimg_script,
                "--kernel", fd_path,
                "--header_version", "2",
                "--base", "0x40000000",
                "--kernel_offset", "0x00080000",
                "--ramdisk_offset", "0x07c80000",
                "--tags_offset", "0x0bc80000",
                "--pagesize", "2048",
                "--cmdline", "bootopt=64S3,32N2,64N2 buildvariant=user",
                "-o", bootimg_out
            ]
            if os.path.exists(dtb_path):
                cmd.extend(["--dtb", dtb_path])

            subprocess.run(cmd, check=True)
        except subprocess.CalledProcessError:
            logging.critical(f"boot.img build failed!")
            return 1

        logging.info("=========== SUCCESS! ===========")
        logging.info("Output boot.img written to: " + bootimg_out)
        logging.info("================================")

        return 0

    def PlatformPostBuild (self):
        steps = [
            self.PlatformBuildBootShim,
            self.PlatformPostBuildBootShim,
            self.PlatformCompressFd,
            self.PlatformBuildBootImage
        ]

        for step in steps:
            status = step()
            if status:
                return status

        return 0

    def FlashRomImage (self):
        return 0

if __name__ == "__main__":
    import argparse
    import sys

    from edk2toolext.invocables.edk2_platform_build import Edk2PlatformBuild
    from edk2toolext.invocables.edk2_setup import Edk2PlatformSetup
    from edk2toolext.invocables.edk2_update import Edk2Update

    SCRIPT_PATH = os.path.relpath (__file__)

    parser = argparse.ArgumentParser (add_help=False)

    parse_group = parser.add_mutually_exclusive_group()

    parse_group.add_argument ("--update", "--UPDATE", action='store_true', help="Invokes stuart_update")
    parse_group.add_argument ("--setup",  "--SETUP",  action='store_true', help="Invokes stuart_setup")

    args, remaining = parser.parse_known_args()

    new_args = ["stuart", "-c", SCRIPT_PATH]
    new_args = new_args + remaining

    sys.argv = new_args

    if args.setup:
        Edk2PlatformSetup().Invoke()
    elif args.update:
        Edk2Update().Invoke()
    else:
        Edk2PlatformBuild().Invoke()
