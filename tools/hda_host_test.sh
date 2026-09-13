#!/bin/bash
# Host-side test for the HDA codec layer (mirrors tools/pkg_host_test.sh):
# compiles src/kernel/drivers/hda.c with -DHDA_HOST_TEST and drives its
# discovery/path setup against a canned Realtek ALC662 topology, asserting the
# chosen pin/DAC, the sibling-jack handling and the emitted amp/EAPD verbs.
# Usage (WSL): bash tools/hda_host_test.sh
cd "$(dirname "$0")/.." || exit 1
gcc -O1 -Wall -Wextra -Wno-unused-function -DHDA_HOST_TEST \
    -o /tmp/hda_host_test tools/hda_host_test.c || exit 1
/tmp/hda_host_test
