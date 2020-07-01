#!/bin/bash

new_commit=${1:-HEAD}
new_tag=$(git describe --tags --always $new_commit)
new_ver=${2:-$new_tag}
old_commit=${3:-$(git log --format=format:%H -1 debian/changelog)}


git log ${old_commit}..$new_commit --not --reverse --no-merges --format='format: %s' | \
    while read msg; do
	echo $msg
	dch -v "${new_ver}" --  "$msg"
    done
dch -r -D unstable -u low done
git add debian/changelog
echo "Do not forget to do: git commit -m 'new release $new_ver'"
