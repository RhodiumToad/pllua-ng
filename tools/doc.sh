#!/bin/sh

template=$1
shift

(
    printf "<html><body>\n"
    cmark "$@"
    printf "</body></html>\n"
) | xsltproc --html --encoding utf-8 "$template" -
