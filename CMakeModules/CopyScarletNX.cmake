# SPDX-FileCopyrightText: 2026 citron-neo Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copies the unpacked ScarletNX bundle next to the citron binary, once per bundle.
# Run as a POST_BUILD step (-P) from src/citron/CMakeLists.txt with
#   -DSRC=<bundle dir> -DDST=<citron bin>/scarletnx -DID=<tag and hash>
# A copy_directory on every link would move 150-350 MB each time.
set(stamp "${DST}/.citron-bundle-stamp")
set(have "")
if (EXISTS "${stamp}")
    file(READ "${stamp}" have)
endif()
if (NOT have STREQUAL ID)
    file(REMOVE_RECURSE "${DST}")
    file(COPY "${SRC}/" DESTINATION "${DST}" USE_SOURCE_PERMISSIONS)
    file(WRITE "${stamp}" "${ID}")
endif()
