#!/bin/sh

(
    printf "<html><body>\n"
    cmark "$@"
    printf "</body></html>\n"
) | xsltproc --html doc/template.xsl -
