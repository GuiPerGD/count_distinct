#!/bin/bash
set -e
set -x

RPMDIR="$PWD/packages/artifacts"

if [ ! -z "$BUILD_NUMBER" ]; then
    git clean -fd
fi

rm -rf "$RPMDIR" hiera.fragment.txt
mkdir -p "$RPMDIR"

[ -z "$GDCVERSION" ] && GDCVERSION=local.$(git rev-parse --short HEAD)

# Archive whole git repo
tar czf count-distinct.tar.gz $(git ls-tree --name-only HEAD)

# Assume we build in jenkins if quality/util/hudsonjobs/gdc_mock_legacy exists
if ! [ -f gdc-ci/quality/util/hudsonjobs/gdc_mock_legacy ]; then
    rpmbuild -bb \
        --define "_rpmdir $RPMDIR" \
        --define "_builddir $PWD" \
        --define "_sourcedir $PWD" \
        --define "_rpmfilename %{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}.rpm" \
        --define "gdcversion $GDCVERSION" \
        specs/postgresql95-count_distinct.spec
else
    . gdc-ci/quality/util/hudsonjobs/gdc_mock_legacy
fi
