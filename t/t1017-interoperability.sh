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

verify_oids () {
	local algo="$1"
	local i oid wanted

	if [ -z "$algo" ]
	then
		return
	fi &&

	shift

	for i in "$@"
	do
		wanted=$(test_oid --hash="$algo" "${i##* }") &&
		oid=$(git rev-parse --output-object-format="$algo" "${i%% *}") &&
		test "$wanted" = "$oid" || return 1
	done
}

fetch_verify_oids () {
	verify_oids "$1" "A tagA" \
		"dev1 tagdev1" \
		"dev1^{} commitdev1" \
		"main2 tagmain2" \
		"main2^{} commitmain2" \
		"main2:ghi.txt blobghi"
}

fetch_shallow_verify_oids () {
	verify_oids "$1" "dev^{} commitjkl" \
		"other^{} commitjkl"
}

push_verify_oids () {
	fetch_verify_oids "$1" &&
	verify_oids "$1" "latest latest" \
		"latest^{} commitlatest" \
		"new commitlatest" \
		"mno mno" \
		"mno^{} commitmno" \
		"dev commitjkl"
}

push_shallow_verify_oids () {
	verify_oids "$1" "foobar tagfoobar" \
		"foobar^{} commitfoobar" \
		"other commitfoobar"
}

push_shallow_verify_oids2 () {
	verify_oids "$1" \
		"other commitbarbaz"
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

test_fetch_push () {
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

	# Always unset the prerequisite first because if it's set twice in a
	# row, then unsetting it doesn't work properly.
	test_unset_prereq SHARED_ALGOS
	if test "$source_algo" = "$dest_algo" && test "$source_compat_algo" = "$dest_compat_algo"
	then
		test_set_prereq SHARED_ALGOS
	elif test "$source_algo" = "$dest_compat_algo" && test "$source_compat_algo" = "$dest_algo"
	then
		test_set_prereq SHARED_ALGOS
	fi

	test_unset_prereq COMPAT_ALGO
	if test -n "$source_compat_algo" && test -n "$dest_compat_algo"
	then
		test_set_prereq COMPAT_ALGO
	fi

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
		latest sha1:23c522c7fcb1bb9a2fc8b102691c9b26a3540f52
		latest sha256:35942891caf8b3abe8a68be01426e30af17b5a1352d57cb820fbe2e5fdce77e1
		commitlatest sha1:8a4955326f7287254bd37d524feb0b1d4bf1d9dd
		commitlatest sha256:36bf30a321a13f2d8ec0d8ead6c6f06ba4806a49a1c5721aa6cbd0c9d4f4bfca
		mno sha1:738a4611019ae7c85d153ecc9024c319cd2340c3
		mno sha256:819c23402dc0bfdf1f3e78fec94385f08506d6ee8b28da7bf260c816701bc852
		commitmno sha1:677eaa8bdc7efa7795935bf4a44e4ec6dafac205
		commitmno sha256:d724eb0be0e19448e6f1995a1dfc02f45037c43f2d162bbe15d142d51c1f4bba
		commitjkl sha1:5dce1d8f80fcbf314deccef2b9491229d800f47f
		commitjkl sha256:b8370f6805640ef3c423768beb896f01ba03f38b2ccf18ccfb0f1a1f3d0ff606
		tagfoobar sha1:b45ac8c8ec546b943320a1bdc6df9ad33b394ed6
		tagfoobar sha256:89ab78a11c688b3d511d68450687bee22691a13725b8a07be3a1c7010ac19bb4
		commitfoobar sha1:78a5b255e240c7b6012ba03a380e2b7ba9119b22
		commitfoobar sha256:f41262a3d7fb330b8524be42c8f5354e2347209e704f28f411850107c91f16c6
		tagbarbaz sha1:5dce1d8f80fcbf314deccef2b9491229d800f47f
		tagbarbaz sha256:b8370f6805640ef3c423768beb896f01ba03f38b2ccf18ccfb0f1a1f3d0ff606
		commitbarbaz sha1:36fb0d1c0c4682026416c1def6c629ddcd50c30e
		commitbarbaz sha256:dc47d49053f77149293f2f8e02fb42198ef41e94cf34bd87054a6e69ee64ab72
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
			git fetch ../source dev:dev dev:other 2>err &&
			! grep -E "error|fatal" err &&
			git pull ../source main:main 2>err &&
			! grep -E "error|fatal" err &&
			fetch_verify_oids "$dest_algo" &&
			fetch_verify_oids "$dest_compat_algo"
		)
	'

	test_expect_success "$desc: push to remote" '
		create_repo --bare other "$source_algo" "$source_compat_algo" &&
		(
			cd dest &&
			git checkout dev &&
			create_blob jkl.txt jkl &&
			test_commit --annotate jkl &&
			git checkout main &&
			create_blob mno.txt mno &&
			test_commit --annotate mno &&
			git checkout other &&
			git reset --hard HEAD^ &&
			create_blob pqr.txt pqr &&
			git checkout main &&
			git merge dev &&
			test_commit --annotate latest &&
			git for-each-ref &&
			# Test pushing into an existing repository.
			git push --follow-tags ../source main:new dev +other &&
			# Test pushing into an empty repository.
			git push --follow-tags ../other main:main main:new dev other
		) &&
		(
			cd source &&
			push_verify_oids "$source_algo" &&
			push_verify_oids "$source_compat_algo"
		) &&
		(
			cd other &&
			push_verify_oids "$source_algo" &&
			push_verify_oids "$source_compat_algo"
		)
	'

	test_expect_success SHARED_ALGOS "$desc: fetch from remote into shallow" '
		create_repo shallow "$dest_algo" "$dest_compat_algo" &&
		(
			cd shallow &&
			set_config "$fsck" "$large_blob" "$protocol" &&
			git fetch --depth=1 ../source dev:dev dev:other &&
			fetch_shallow_verify_oids "$dest_algo" &&
			fetch_shallow_verify_oids "$dest_compat_algo" &&
			git fetch --depth=2 ../source dev:dev dev:other &&
			git fetch --unshallow ../source &&
			git pull ../source main:main &&
			fetch_verify_oids "$dest_algo" &&
			fetch_verify_oids "$dest_compat_algo"
		)
	'

	test_expect_success !SHARED_ALGOS "$desc: fetch from remote into shallow" '
		create_repo shallow "$dest_algo" "$dest_compat_algo" &&
		(
			cd shallow &&
			set_config "$fsck" "$large_blob" "$protocol" &&
			test_must_fail git fetch --depth=1 ../source dev:dev dev:other 2>err &&
			grep "remote side does not support shallow clones in compatibility mode" err
		)
	'

	test_expect_success SHARED_ALGOS "$desc: fetch and push with shallow repo" '
		create_repo shallow2 "$dest_algo" "$dest_compat_algo" &&
		create_repo shallow3 "$dest_algo" "$dest_compat_algo" &&
		create_repo source2 "$source_algo" "$source_compat_algo" &&
		(
			cd source2 &&
			git fetch ../source dev:dev
		) &&
		(
			cd shallow3 &&
			git config receive.shallowupdate true &&
			set_config "$fsck" "$large_blob" "$protocol" &&
			git fetch --depth=1 ../source dev:dev
		) &&
		(
			cd shallow2 &&
			git config receive.shallowupdate true &&
			set_config "$fsck" "$large_blob" "$protocol" &&
			git fetch --depth=1 ../source dev:dev dev:other &&
			fetch_shallow_verify_oids "$dest_algo" &&
			fetch_shallow_verify_oids "$dest_compat_algo" &&
			git checkout other &&
			test_commit --annotate foobar &&
			git checkout dev &&
			git push --follow-tags ../source2 +other &&
			git push --follow-tags ../shallow3 +other:dev
		) &&
		(
			cd source2 &&
			push_shallow_verify_oids "$dest_algo" &&
			push_shallow_verify_oids "$dest_compat_algo" &&
			git checkout other &&
			test_commit --annotate barbaz &&
			git push ../shallow2 other &&
			push_shallow_verify_oids2 "$source_algo" &&
			push_shallow_verify_oids2 "$source_compat_algo"
		) &&
		(
			cd shallow2 &&
			push_shallow_verify_oids2 "$dest_algo" &&
			push_shallow_verify_oids2 "$dest_compat_algo"
		)
	'

	test_expect_success "$desc: setup of repository with submodules" '
		create_repo subsource "$source_algo" "$source_compat_algo" &&
		(
			cd subsource &&
			git fetch ../source dev:dev &&
			git checkout dev &&
			create_repo sub "$source_algo" "$source_compat_algo" &&
			test_commit -C sub --annotate sub1 &&
			git submodule add "$(pwd)/sub" sub &&
			test_commit --annotate super
		) &&
		subsource_oid=$(git -C subsource rev-parse HEAD) &&
		sub_commit=$(git -C subsource/sub rev-parse sub1^{commit}) &&
		create_repo subdest "$dest_algo" "$dest_compat_algo" &&
		create_repo subdest2 "$dest_algo" "$dest_compat_algo"
	'

	test_expect_success SHARED_ALGOS "$desc: fetch from submodule repository" '
		test_config_global protocol.file.allow always &&
		(
			cd subdest &&
			GIT_TRACE=1 GIT_TRACE_PACKET=1 git fetch ../subsource dev:dev &&
			git checkout dev &&
			GIT_TRACE=1 GIT_TRACE_PACKET=1 git submodule update --init --recursive &&
			dev=$(git rev-parse --output-object-format="$source_algo" dev) &&
			commit=$(git -C sub rev-parse --output-object-format="$source_algo" HEAD) &&
			submodule=$(git rev-parse --output-object-format="$source_algo" dev:sub) &&
			test "$dev" = "$subsource_oid" &&
			test "$commit" = "$sub_commit" &&
			test "$submodule" = "$sub_commit"
		)
	'

	test_expect_success COMPAT_ALGO,SHARED_ALGOS "$desc: fetch from submodule repository with transfer.allowmappedsubmodules" '
		test_config -C subdest2 transfer.allowmappedsubmodules false &&
		test_config_global protocol.file.allow always &&
		(
			cd subdest2 &&
			test_must_fail git fetch ../subsource dev:unsuccessful 2>err &&
			grep -E "could not map object|cannot map object" err
		)
	'

	test_expect_success !SHARED_ALGOS "$desc: fetch from submodule repository with mismatched algos" '
		test_config_global protocol.file.allow always &&
		(
			cd subdest &&
			test_must_fail git fetch ../subsource dev:dev 2>err &&
			grep "could not map object.*due to missing submodule" err &&
			grep "make sure that the remote side supports" err
		)
	'
}

test_fetch_push sha1-to-sha1 sha1: sha1: --fsck
test_fetch_push sha256-to-sha256 sha256: sha256: --fsck
test_fetch_push sha256-to-sha256-fancy sha256: sha256: --fsck --large-blob 512 --protocol 0
test_fetch_push sha1-to-sha256-same sha1:sha256 sha1:sha256
test_fetch_push sha1-to-sha256-same-fancy sha1:sha256 sha1:sha256 --fsck --large-blob 512 --protocol 0
test_fetch_push sha256-to-sha1-same sha256:sha1 sha256:sha1
test_fetch_push sha256-to-sha1-same-fancy sha256:sha1 sha256:sha1 --fsck --large-blob 512 --protocol 0
test_fetch_push sha256-to-sha1-both sha256:sha1 sha1:sha256
test_fetch_push sha1-to-sha256-both-fancy sha1:sha256 sha256:sha1 --fsck --large-blob 512 --protocol 0
test_fetch_push sha1-to-sha256-main sha1: sha256:sha1
test_fetch_push sha1-to-sha256-main-fancy sha1: sha256:sha1 --fsck --large-blob 512 --protocol 0
test_fetch_push sha1-to-sha256-both sha1:sha256 sha256:sha1

test_done
