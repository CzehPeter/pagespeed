#!/bin/bash
#
# Copyright 2012 Google Inc. All Rights Reserved.
# Author: jefftk@google.com (Jeff Kaufman)
#
# Runs all Apache-specific experiment framework tests that depend on AnalyticsID
# being unset.
#
# See apache_experiment_test for usage.
#
this_dir=$(dirname $0)
source "$this_dir/apache_experiment_test.sh" || exit 1

EXAMPLE="$1/mod_pagespeed_example"
EXTEND_CACHE="$EXAMPLE/extend_cache.html"

start_test Analytics javascript is not added for any group.
OUT=$($WGET_DUMP --header='Cookie: PageSpeedExperiment=2' $EXTEND_CACHE)
check_not_from "$OUT" fgrep -q 'Experiment:'
OUT=$($WGET_DUMP --header='Cookie: PageSpeedExperiment=7' $EXTEND_CACHE)
check_not_from "$OUT" fgrep -q 'Experiment:'
OUT=$($WGET_DUMP --header='Cookie: PageSpeedExperiment=0' $EXTEND_CACHE)
check_not_from "$OUT" fgrep -q 'Experiment:'

check_failures_and_exit
