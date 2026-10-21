import argparse
import base64
import binascii
import copy
import hashlib
import inspect
import io
import os
import shlex
import struct
import sys
import time
import zlib
import string


__version__ = "0.1 dev"

PYTHON2 = sys.version_info[0] < 3  # True if on pre-Python 3


def add_crc(args):
    fp = open(args.filename, "r+b")
    fp.seek(0, 0)
    file_content = fp.read()
    crc32_result = zlib.crc32(file_content)

    fp.seek(0, 2)
    fp.write(struct.pack('>I', crc32_result))

    print("Firmware CRC32: " + hex(crc32_result))

    fp.close()

# NOTE (this fork): the upstream `combine` subcommand (concatenate uart_boot and
# firmware into a *_with_boot.bin image, manually invoked via `make combine_fw`)
# was removed: its argv-driven write-to-any-path pattern is rejected by the
# security tooling used to publish this repository, and it is not part of the
# default build (all: pre-build main-build -> add_crc only). If you need it,
# take make/tl_firmware_tools.py from upstream atc1441/ATC_TLSR_Paper and
# re-add the combine_fw make target.


def main(custom_commandline=None):

    parser = argparse.ArgumentParser(description='tl_fireware_tools.py v%s - Telink BLE Firmware Utility' % __version__)

    subparsers = parser.add_subparsers(dest='operation', help='Run tl_fireware_tools.py -h for additional help')

    add_crc = subparsers.add_parser('add_crc', help='Add CRC32 check to the file tail')
    add_crc.add_argument('filename', help='Firmware image')


    args = parser.parse_args(custom_commandline)

    print('tl_fireware_tools.py v%s' % __version__)

    if args.operation is None:
        parser.print_help()
        sys.exit(1)

    operation_func = globals()[args.operation]

    operation_func(args)


def _main():
    #try:
    main()
    # except FatalError as e:
    #     print('\nA fatal error occurred: %s' % e)
    #     sys.exit(2)


if __name__ == '__main__':
    _main()
