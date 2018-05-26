#!/bin/sh

test_description='errors in upload-pack'

. ./test-lib.sh

D=$(pwd)

corrupt_repo () {
	object_sha1=$(git rev-parse "$1") &&
	ob=$(expr "$object_sha1" : "\(..\)") &&
	ject=$(expr "$object_sha1" : "..\(..*\)") &&
	rm -f ".git/objects/$ob/$ject"
}

test_expect_success 'setup and corrupt repository' '

	echo file >file &&
	git add file &&
	git rev-parse :file &&
	git commit -a -m original &&
	test_tick &&
	echo changed >file &&
	git commit -a -m changed &&
	corrupt_repo HEAD:file

'

test_expect_success 'fsck fails' '
	test_must_fail git fsck
'

test_expect_success 'upload-pack fails due to error in pack-objects packing' '
	head=$(git rev-parse HEAD) &&
	oidlen=$(printf "%s" $head | wc -c) &&
	printf "%04xwant %s\n00000009done\n0000" \
		$((oidlen + 10)) $head >input &&
	test_must_fail git upload-pack . <input >/dev/null 2>output.err &&
	test_i18ngrep "unable to read" output.err &&
	test_i18ngrep "pack-objects died" output.err
'

test_expect_success 'corrupt repo differently' '

	git hash-object -w file &&
	corrupt_repo HEAD^^{tree}

'

test_expect_success 'fsck fails' '
	test_must_fail git fsck
'
test_expect_success 'upload-pack fails due to error in rev-list' '

	printf "%04xwant %s\n%04xshallow %s00000009done\n0000" \
		$((oidlen + 10)) $(git rev-parse HEAD) \
		$((oidlen + 12)) $(git rev-parse HEAD^) >input &&
	test_must_fail git upload-pack . <input >/dev/null 2>output.err &&
	grep "bad tree object" output.err
'

test_expect_success 'upload-pack error message when bad ref requested' '
	invalid=$(echo $ZERO_OID | sed -e "s/00000000/deadbeef/g") &&
	printf "%04xwant %s multi_ack_detailed\n00000009done\n0000" \
		$((oidlen + 29)) $invalid >input &&
	test_must_fail git upload-pack . <input >output 2>output.err &&
	grep -q "not our ref" output.err &&
	! grep -q multi_ack_detailed output.err
'

test_expect_success 'upload-pack fails due to error in pack-objects enumeration' '

	printf "%04xwant %s\n00000009done\n0000" \
		$((oidlen + 10)) $(git rev-parse HEAD) >input &&
	test_must_fail git upload-pack . <input >/dev/null 2>output.err &&
	grep "bad tree object" output.err &&
	grep "pack-objects died" output.err
'

test_expect_success 'create empty repository' '

	mkdir foo &&
	cd foo &&
	git init

'

test_expect_success 'fetch fails' '

	test_must_fail git fetch .. master

'

test_done
