#!/bin/sh

(
    printf "<html><body>\n"
    cmark "$@"
    printf "</body></html>\n"
) | xsltproc --html --encoding utf-8 doc/template.xsl -
