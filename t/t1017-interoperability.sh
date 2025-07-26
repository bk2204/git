#!/bin/sh
#
# Copyright © 2025 brian m. carlson
#
# Licensed under the GNU General Public License v2, dated June 1991.

test_description='Test interoperability between hash algorithms'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

if ! test_have_prereq RUST
then
	skip_all='interoperability requires a Git built with Rust'
	test_done
fi

create_repo () {
	local bare=""

	if [ "$1" = "--bare" ]
	then
		bare="--bare"
		shift;
	fi

	local path="$1"
	local algo="$2"
	local compat_algo="$3"

	git init $bare --object-format="$algo" "$path" &&
	if [ -n "$compat_algo" ]
	then
		git -C "$path" config core.repositoryformatversion 1 &&
		git -C "$path" config extensions.compatobjectformat "$compat_algo"
	fi
}

# This creates a reasonably large blob with a known prefix so that it will delta
# well with other blobs.  This lets us test deltification properly.
create_blob () {
	local name="$1"
	local body="${2:-$1}"
	local i

	: >"$name" &&
	for i in A B C D E F G H I J K L M N O P Q R S T U V W X Y Z
	do
		echo "${i}abcdefghijklmnopqrstuvwxyz" >>"$name"
	done &&
	echo "$body" >>"$name"
}

fetch_verify_oids () {
	local algo="$1"
	local i oid wanted

	if [ -z "$algo" ]
	then
		return
	fi &&

	for i in "A tagA" \
		"dev1 tagdev1" \
		"dev1^{} commitdev1" \
		"main2 tagmain2" \
		"main2^{} commitmain2" \
		"main2:ghi.txt blobghi" \
		"HEAD commitmain2"
	do
		wanted=$(test_oid --hash="$algo" "${i##* }") &&
		oid=$(git rev-parse --output-object-format="$algo" "${i%% *}") &&
		test "$wanted" = "$oid" || return 1
	done
}

set_config () {
	local i

	for i in "$@"
	do
		if [ -n "$i" ]
		then
			git config "${i%%=*}" "${i#*=}" || return 1
		fi
	done
}

test_fetch () {
	local desc="$1"
	local source_algo="${2%%:*}"
	local source_compat_algo="${2##*:}"
	local dest_algo="${3%%:*}"
	local dest_compat_algo="${3##*:}"
	local fsck=
	local large_blob=
	local protocol=

	shift 3

	while test $# -gt 0
	do
		case "$1" in
		--fsck)
			fsck="transfer.fsckobjects=true"
			shift
			;;
		--large-blob)
			large_blob="core.bigfilethreshold=$2"
			shift 2
			;;
		--protocol)
			protocol="protocol.version=$2"
			shift 2
			;;
		*)
			break
			;;
		esac
	done

	test_expect_success "$desc: setup" '
		sane_unset GIT_DEFAULT_HASH &&
		cd &&
		mkdir "$desc" &&
		cd "$desc" &&
		create_repo source "$source_algo" "$source_compat_algo" &&
		(
			cd source &&
			set_config "$large_blob" &&
			create_blob abc.txt abc &&
			git add abc.txt &&
			test_commit --annotate A &&
			git checkout -b dev &&
			create_blob def.txt def &&
			git add def.txt &&
			test_commit --annotate dev1 &&
			git checkout main &&
			create_blob ghi.txt ghi &&
			git add ghi.txt &&
			test_commit --annotate main2
		)
	'

	test_expect_success "$desc: verify object IDs" '
		test_oid_cache <<-EOF &&
		tagA sha1:7d4adaf29ef26c232a42a19b52ad7e2eeb7ef431
		tagA sha256:a7f0edc6ea5e4662846e0afaa7ac9e89f1a1c5882e690f8a8019121b1df84c6c
		tagdev1 sha1:03f79eae14fdae3af26d515daeb55b073c383755
		tagdev1 sha256:c653dd97186ce94c85fd0df0013172de84aa47c482d9fddb422e3995fc63cb7d
		commitdev1 sha1:6ca1cbe2650ceef982dcf198eb587b4cd0bfdb96
		commitdev1 sha256:dc53305c7849999519843043631667cca95b330e0ca0feaa49e84e7f46f37e31
		tagmain2 sha1:197cc4ab5136b5643469798ad46f6a4482bce468
		tagmain2 sha256:590dc7d6c2582dcbdf983d6760137d10054a37f983521bfb3c96b308aa2a19c1
		commitmain2 sha1:ab92abc97ba4b8645b7b136ee1fc443b4500b86a
		commitmain2 sha256:818f622b2306b7438c623ff54f94761f70f731c0b146177a139a2f6d40c03899
		blobghi sha1:6633368e305a8d228cc9b068ba0933a3806af129
		blobghi sha256:2239363ba3c3edddb1578724cf10ac8602837b2ca2cbdf2b83a06eb1ab4078e2
		EOF
		(
			cd source &&
			fetch_verify_oids "$source_algo" &&
			fetch_verify_oids "$source_compat_algo"
		)
	'

	test_expect_success "$desc: fetch from remote" '
		create_repo dest "$dest_algo" "$dest_compat_algo" &&
		(
			cd dest &&
			set_config "$fsck" "$large_blob" "$protocol" &&
			git fetch ../source dev:dev 2>err &&
			! grep -E "error|fatal" err &&
			git pull ../source main:main 2>err &&
			! grep -E "error|fatal" err &&
			fetch_verify_oids "$dest_algo" &&
			fetch_verify_oids "$dest_compat_algo"
		)
	'
}

test_fetch sha1-to-sha1 sha1: sha1: --fsck
test_fetch sha256-to-sha256 sha256: sha256: --fsck
test_fetch sha256-to-sha256-fancy sha256: sha256: --fsck --large-blob 512 --protocol 0
test_fetch sha1-to-sha256-main sha1: sha256:sha1

test_done
