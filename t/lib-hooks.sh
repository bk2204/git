create_multihooks () {
	mkdir -p "$MULTIHOOK_DIR"
	for i in "a $1" "b $2" "c $3"
	do
		echo "$i" | (while read script ex
		do
			mkdir -p "$MULTIHOOK_DIR"
			write_script "$MULTIHOOK_DIR/$script" <<-EOF
			mkdir -p "$OUTPUTDIR"
			touch "$OUTPUTDIR/$script"
			exit $ex
			EOF
		done)
	done
}

# Run the multiple hook tests.
# Usage: test_multiple_hooks [--ignore-exit-status] HOOK COMMAND [SKIP-COMMAND]
# HOOK:  the name of the hook to test
# COMMAND: command to test the hook for; takes a single argument indicating test
# name.
# SKIP-COMMAND: like $1, except the hook should be skipped.
# --ignore-exit-status: the command does not fail if the exit status from the
# hook is nonzero.
test_multiple_hooks () {
	local must_fail cmd skip_cmd hook
	if test "$1" = "--ignore-exit-status"
	then
		shift
	else
		must_fail="test_must_fail"
	fi
	hook="$1"
	cmd="$2"
	skip_cmd="$3"

	HOOKDIR="$(git rev-parse --absolute-git-dir)/hooks"
	OUTPUTDIR="$(git rev-parse --absolute-git-dir)/../hook-output"
	HOOK="$HOOKDIR/$hook"
	MULTIHOOK_DIR="$HOOKDIR/$hook.d"
	rm -f "$HOOK" "$MULTIHOOK_DIR" "$OUTPUTDIR"

	test_expect_success "$hook: with no hook" '
		$cmd foo
	'

	if test -n "$skip_cmd"
	then
		test_expect_success "$hook: skipped hook with no hook" '
			$skip_cmd bar
		'
	fi

	test_expect_success 'setup' '
		mkdir -p "$HOOKDIR" &&
		write_script "$HOOK" <<-EOF
		mkdir -p "$OUTPUTDIR"
		touch "$OUTPUTDIR/simple"
		exit 0
		EOF
	'

	test_expect_success "$hook: with succeeding hook" '
		test_when_finished "rm -fr \"$OUTPUTDIR\"" &&
		$cmd more &&
		test -f "$OUTPUTDIR/simple"
	'

	if test -n "$skip_cmd"
	then
		test_expect_success "$hook: skipped but succeeding hook" '
			test_when_finished "rm -fr \"$OUTPUTDIR\"" &&
			$skip_cmd even-more &&
			! test -f "$OUTPUTDIR/simple"
		'
	fi

	test_expect_success "$hook: with both simple and multiple hooks, simple success" '
		test_when_finished "rm -fr \"$OUTPUTDIR\"" &&
		create_multihooks 0 1 0 &&
		$cmd yet-more &&
		test -f "$OUTPUTDIR/simple" &&
		! test -f "$OUTPUTDIR/a" &&
		! test -f "$OUTPUTDIR/b" &&
		! test -f "$OUTPUTDIR/c"
	'

	test_expect_success 'setup' '
		rm -fr "$MULTIHOOK_DIR" &&

		# now a hook that fails
		write_script "$HOOK" <<-EOF
		mkdir -p "$OUTPUTDIR"
		touch "$OUTPUTDIR/simple"
		exit 1
		EOF
	'

	test_expect_success "$hook: with failing hook" '
		test_when_finished "rm -fr \"$OUTPUTDIR\"" &&
		$must_fail $cmd another &&
		test -f "$OUTPUTDIR/simple"
	'

	if test -n "$skip_cmd"
	then
		test_expect_success "$hook: skipped but failing hook" '
			test_when_finished "rm -fr \"$OUTPUTDIR\"" &&
			$skip_cmd stuff &&
			! test -f "$OUTPUTDIR/simple"
		'
	fi

	test_expect_success "$hook: with both simple and multiple hooks, simple failure" '
		test_when_finished "rm -fr \"$OUTPUTDIR\"" &&
		create_multihooks 0 1 0 &&
		$must_fail $cmd more-stuff &&
		test -f "$OUTPUTDIR/simple" &&
		! test -f "$OUTPUTDIR/a" &&
		! test -f "$OUTPUTDIR/b" &&
		! test -f "$OUTPUTDIR/c"
	'

	test_expect_success "$hook: multiple hooks, all successful" '
		test_when_finished "rm -fr \"$OUTPUTDIR\"" &&
		rm -f "$HOOK" &&
		create_multihooks 0 0 0 &&
		$cmd content &&
		test -f "$OUTPUTDIR/a" &&
		test -f "$OUTPUTDIR/b" &&
		test -f "$OUTPUTDIR/c"
	'

	test_expect_success "$hook: hooks after first failure not executed" '
		test_when_finished "rm -fr \"$OUTPUTDIR\"" &&
		create_multihooks 0 1 0 &&
		$must_fail $cmd more-content &&
		test -f "$OUTPUTDIR/a" &&
		test -f "$OUTPUTDIR/b" &&
		! test -f "$OUTPUTDIR/c"
	'

	test_expect_success POSIXPERM "$hook: non-executable hook not executed" '
		test_when_finished "rm -fr \"$OUTPUTDIR\"" &&
		create_multihooks 0 1 0 &&
		chmod -x "$MULTIHOOK_DIR/b" &&
		$cmd things &&
		test -f "$OUTPUTDIR/a" &&
		! test -f "$OUTPUTDIR/b" &&
		test -f "$OUTPUTDIR/c"
	'

	test_expect_success POSIXPERM "$hook: multiple hooks not executed if simple hook present" '
		test_when_finished "rm -fr \"$OUTPUTDIR\" && rm -f \"$HOOK\"" &&
		write_script "$HOOK" <<-EOF &&
		mkdir -p "$OUTPUTDIR"
		touch "$OUTPUTDIR/simple"
		exit 0
		EOF
		create_multihooks 0 1 0 &&
		chmod -x "$HOOK" &&
		$cmd other-things &&
		! test -f "$OUTPUTDIR/simple" &&
		! test -f "$OUTPUTDIR/a" &&
		! test -f "$OUTPUTDIR/b" &&
		! test -f "$OUTPUTDIR/c"
	'

	test_expect_success 'cleanup' '
		rm -fr "$MULTIHOOK_DIR"
	'
}
