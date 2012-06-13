#!/bin/sh

test_description='reset can handle submodules'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-submodule-update.sh

KNOWN_FAILURE_RECURSE_SUBMODULE_SERIES_BREAKS_REPLACE_SUBMODULE_TEST=1
test_submodule_switch "git reset --keep"

test_submodule_switch "git reset --merge"

KNOWN_FAILURE_RECURSE_SUBMODULE_SERIES_BREAKS_REPLACE_SUBMODULE_TEST=
test_submodule_forced_switch "git reset --hard"

test_done
