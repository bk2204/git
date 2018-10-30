#!/bin/sh

test_description='range-diff tests'

. ./test-lib.sh

# Note that because of the range-diff's heuristics, test_commit does more
# harm than good.  We need some real history.

test_expect_success 'setup' '
	git fast-import < "$TEST_DIRECTORY"/t3206/history.export &&
	test_oid_cache <<-EOF
	# topic
	t1 sha1:4de457d
	t2 sha1:fccce22
	t3 sha1:147e64e
	t4 sha1:a63e992
	t1 sha256:b89f8b9
	t2 sha256:5f12aad
	t3 sha256:ea8b273
	t4 sha256:14b7336

	# unmodified
	u1 sha1:35b9b25
	u2 sha1:de345ab
	u3 sha1:9af6654
	u4 sha1:2901f77
	u1 sha256:e3731be
	u2 sha256:14fadf8
	u3 sha256:736c4bc
	u4 sha256:673e77d

	# reordered
	r1 sha1:aca177a
	r2 sha1:14ad629
	r3 sha1:ee58208
	r4 sha1:307b27a
	r1 sha256:f59d3aa
	r2 sha256:fb261a8
	r3 sha256:cb2649b
	r4 sha256:958577e

	# removed (deleted)
	d1 sha1:7657159
	d2 sha1:43d84d3
	d3 sha1:a740396
	d1 sha256:e312513
	d2 sha256:eb19258
	d3 sha256:1ccb3c1

	# added
	a1 sha1:2716022
	a2 sha1:b62accd
	a3 sha1:df46cfa
	a4 sha1:3e64548
	a5 sha1:12b4063
	a1 sha256:d724f4d
	a2 sha256:1de7762
	a3 sha256:e159431
	a4 sha256:b3e483c
	a5 sha256:90866a7

	# rebased
	b1 sha1:cc9c443
	b2 sha1:c5d9641
	b3 sha1:28cc2b6
	b4 sha1:5628ab7
	b5 sha1:a31b12e
	b1 sha256:a1a8717
	b2 sha256:20a5862
	b3 sha256:587172a
	b4 sha256:2721c5d
	b5 sha256:7b57864

	# changed
	c1 sha1:a4b3333
	c2 sha1:f51d370
	c3 sha1:0559556
	c4 sha1:d966c5c
	c1 sha256:f8c2b9d
	c2 sha256:3fb6318
	c3 sha256:168ab68
	c4 sha256:3526539

	# changed-message
	m1 sha1:f686024
	m2 sha1:4ab067d
	m3 sha1:b9cb956
	m4 sha1:8add5f1
	m1 sha256:31e6281
	m2 sha256:a06bf1b
	m3 sha256:82dc654
	m4 sha256:48470c5

	# Empty delimiter (included so lines match neatly)
	__ sha1:-------
	__ sha256:-------
	EOF
'

test_expect_success 'simple A..B A..C (unmodified)' '
	git range-diff --no-color master..topic master..unmodified \
		>actual &&
	cat >expected <<-EOF &&
	1:  $(test_oid t1) = 1:  $(test_oid u1) s/5/A/
	2:  $(test_oid t2) = 2:  $(test_oid u2) s/4/A/
	3:  $(test_oid t3) = 3:  $(test_oid u3) s/11/B/
	4:  $(test_oid t4) = 4:  $(test_oid u4) s/12/B/
	EOF
	test_cmp expected actual
'

test_expect_success 'simple B...C (unmodified)' '
	git range-diff --no-color topic...unmodified >actual &&
	# same "expected" as above
	test_cmp expected actual
'

test_expect_success 'simple A B C (unmodified)' '
	git range-diff --no-color master topic unmodified >actual &&
	# same "expected" as above
	test_cmp expected actual
'

test_expect_success 'trivial reordering' '
	git range-diff --no-color master topic reordered >actual &&
	cat >expected <<-EOF &&
	1:  $(test_oid t1) = 1:  $(test_oid r1) s/5/A/
	3:  $(test_oid t3) = 2:  $(test_oid r2) s/11/B/
	4:  $(test_oid t4) = 3:  $(test_oid r3) s/12/B/
	2:  $(test_oid t2) = 4:  $(test_oid r4) s/4/A/
	EOF
	test_cmp expected actual
'

test_expect_success 'removed a commit' '
	git range-diff --no-color master topic removed >actual &&
	cat >expected <<-EOF &&
	1:  $(test_oid t1) = 1:  $(test_oid d1) s/5/A/
	2:  $(test_oid t2) < -:  $(test_oid __) s/4/A/
	3:  $(test_oid t3) = 2:  $(test_oid d2) s/11/B/
	4:  $(test_oid t4) = 3:  $(test_oid d3) s/12/B/
	EOF
	test_cmp expected actual
'

test_expect_success 'added a commit' '
	git range-diff --no-color master topic added >actual &&
	cat >expected <<-EOF &&
	1:  $(test_oid t1) = 1:  $(test_oid a1) s/5/A/
	2:  $(test_oid t2) = 2:  $(test_oid a2) s/4/A/
	-:  $(test_oid __) > 3:  $(test_oid a3) s/6/A/
	3:  $(test_oid t3) = 4:  $(test_oid a4) s/11/B/
	4:  $(test_oid t4) = 5:  $(test_oid a5) s/12/B/
	EOF
	test_cmp expected actual
'

test_expect_success 'new base, A B C' '
	git range-diff --no-color master topic rebased >actual &&
	cat >expected <<-EOF &&
	1:  $(test_oid t1) = 1:  $(test_oid b1) s/5/A/
	2:  $(test_oid t2) = 2:  $(test_oid b2) s/4/A/
	3:  $(test_oid t3) = 3:  $(test_oid b3) s/11/B/
	4:  $(test_oid t4) = 4:  $(test_oid b4) s/12/B/
	EOF
	test_cmp expected actual
'

test_expect_success 'new base, B...C' '
	# this syntax includes the commits from master!
	git range-diff --no-color topic...rebased >actual &&
	cat >expected <<-EOF &&
	-:  $(test_oid __) > 1:  $(test_oid b5) unrelated
	1:  $(test_oid t1) = 2:  $(test_oid b1) s/5/A/
	2:  $(test_oid t2) = 3:  $(test_oid b2) s/4/A/
	3:  $(test_oid t3) = 4:  $(test_oid b3) s/11/B/
	4:  $(test_oid t4) = 5:  $(test_oid b4) s/12/B/
	EOF
	test_cmp expected actual
'

test_expect_success 'changed commit' '
	git range-diff --no-color topic...changed >actual &&
	cat >expected <<-EOF &&
	1:  $(test_oid t1) = 1:  $(test_oid c1) s/5/A/
	2:  $(test_oid t2) = 2:  $(test_oid c2) s/4/A/
	3:  $(test_oid t3) ! 3:  $(test_oid c3) s/11/B/
	    @@ -10,7 +10,7 @@
	      9
	      10
	     -11
	    -+B
	    ++BB
	      12
	      13
	      14
	4:  $(test_oid t4) ! 4:  $(test_oid c4) s/12/B/
	    @@ -8,7 +8,7 @@
	     @@
	      9
	      10
	    - B
	    + BB
	     -12
	     +B
	      13
	EOF
	test_cmp expected actual
'

test_expect_success 'changed commit with --no-patch diff option' '
	git range-diff --no-color --no-patch topic...changed >actual &&
	cat >expected <<-EOF &&
	1:  4de457d = 1:  a4b3333 s/5/A/
	2:  fccce22 = 2:  f51d370 s/4/A/
	3:  147e64e ! 3:  0559556 s/11/B/
	4:  a63e992 ! 4:  d966c5c s/12/B/
	EOF
	test_cmp expected actual
'

test_expect_success 'changed commit with --stat diff option' '
	git range-diff --no-color --stat topic...changed >actual &&
	cat >expected <<-EOF &&
	1:  4de457d = 1:  a4b3333 s/5/A/
	     a => b | 0
	     1 file changed, 0 insertions(+), 0 deletions(-)
	2:  fccce22 = 2:  f51d370 s/4/A/
	     a => b | 0
	     1 file changed, 0 insertions(+), 0 deletions(-)
	3:  147e64e ! 3:  0559556 s/11/B/
	     a => b | 0
	     1 file changed, 0 insertions(+), 0 deletions(-)
	4:  a63e992 ! 4:  d966c5c s/12/B/
	     a => b | 0
	     1 file changed, 0 insertions(+), 0 deletions(-)
	EOF
	test_cmp expected actual
'

test_expect_success 'changed commit with sm config' '
	git range-diff --no-color --submodule=log topic...changed >actual &&
	cat >expected <<-EOF &&
	1:  4de457d = 1:  a4b3333 s/5/A/
	2:  fccce22 = 2:  f51d370 s/4/A/
	3:  147e64e ! 3:  0559556 s/11/B/
	    @@ -10,7 +10,7 @@
	      9
	      10
	     -11
	    -+B
	    ++BB
	      12
	      13
	      14
	4:  a63e992 ! 4:  d966c5c s/12/B/
	    @@ -8,7 +8,7 @@
	     @@
	      9
	      10
	    - B
	    + BB
	     -12
	     +B
	      13
	EOF
	test_cmp expected actual
'

test_expect_success 'no commits on one side' '
	git commit --amend -m "new message" &&
	git range-diff master HEAD@{1} HEAD
'

test_expect_success 'changed message' '
	git range-diff --no-color topic...changed-message >actual &&
	sed s/Z/\ /g >expected <<-EOF &&
	1:  $(test_oid t1) = 1:  $(test_oid m1) s/5/A/
	2:  $(test_oid t2) ! 2:  $(test_oid m2) s/4/A/
	    @@ -2,6 +2,8 @@
	    Z
	    Z    s/4/A/
	    Z
	    +    Also a silly comment here!
	    +
	    Z diff --git a/file b/file
	    Z --- a/file
	    Z +++ b/file
	3:  $(test_oid t3) = 3:  $(test_oid m3) s/11/B/
	4:  $(test_oid t4) = 4:  $(test_oid m4) s/12/B/
	EOF
	test_cmp expected actual
'

test_expect_success 'dual-coloring' '
	sed -e "s|^:||" >expect <<-EOF &&
	:<YELLOW>1:  $(test_oid c1) = 1:  $(test_oid m1) s/5/A/<RESET>
	:<RED>2:  $(test_oid c2) <RESET><YELLOW>!<RESET><GREEN> 2:  $(test_oid m2)<RESET><YELLOW> s/4/A/<RESET>
	:    <REVERSE><CYAN>@@ -2,6 +2,8 @@<RESET>
	:     <RESET>
	:         s/4/A/<RESET>
	:     <RESET>
	:    <REVERSE><GREEN>+<RESET><BOLD>    Also a silly comment here!<RESET>
	:    <REVERSE><GREEN>+<RESET>
	:      diff --git a/file b/file<RESET>
	:      --- a/file<RESET>
	:      +++ b/file<RESET>
	:<RED>3:  $(test_oid c3) <RESET><YELLOW>!<RESET><GREEN> 3:  $(test_oid m3)<RESET><YELLOW> s/11/B/<RESET>
	:    <REVERSE><CYAN>@@ -10,7 +10,7 @@<RESET>
	:      9<RESET>
	:      10<RESET>
	:    <RED> -11<RESET>
	:    <REVERSE><RED>-<RESET><FAINT;GREEN>+BB<RESET>
	:    <REVERSE><GREEN>+<RESET><BOLD;GREEN>+B<RESET>
	:      12<RESET>
	:      13<RESET>
	:      14<RESET>
	:<RED>4:  $(test_oid c4) <RESET><YELLOW>!<RESET><GREEN> 4:  $(test_oid m4)<RESET><YELLOW> s/12/B/<RESET>
	:    <REVERSE><CYAN>@@ -8,7 +8,7 @@<RESET>
	:    <CYAN> @@<RESET>
	:      9<RESET>
	:      10<RESET>
	:    <REVERSE><RED>-<RESET><FAINT> BB<RESET>
	:    <REVERSE><GREEN>+<RESET><BOLD> B<RESET>
	:    <RED> -12<RESET>
	:    <GREEN> +B<RESET>
	:      13<RESET>
	EOF
	git range-diff changed...changed-message --color --dual-color >actual.raw &&
	test_decode_color >actual <actual.raw &&
	test_cmp expect actual
'

for prev in topic master..topic
do
	test_expect_success "format-patch --range-diff=$prev" '
		git format-patch --stdout --cover-letter --range-diff=$prev \
			master..unmodified >actual &&
		grep "= 1: .* s/5/A" actual &&
		grep "= 2: .* s/4/A" actual &&
		grep "= 3: .* s/11/B" actual &&
		grep "= 4: .* s/12/B" actual
	'
done

test_expect_success 'format-patch --range-diff as commentary' '
	git format-patch --stdout --range-diff=HEAD~1 HEAD~1 >actual &&
	test_i18ngrep "^Range-diff:$" actual
'

test_done
