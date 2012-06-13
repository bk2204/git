#!/bin/sh

test_description='read-tree can handle submodules'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-submodule-update.sh

KNOWN_FAILURE_RECURSE_SUBMODULE_SERIES_BREAKS_REPLACE_SUBMODULE_TEST=1
test_submodule_switch "git read-tree -u -m"

KNOWN_FAILURE_RECURSE_SUBMODULE_SERIES_BREAKS_REPLACE_SUBMODULE_TEST=
test_submodule_forced_switch "git read-tree -u --reset"

test_done
