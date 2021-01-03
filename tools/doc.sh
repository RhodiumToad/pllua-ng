#!/bin/sh

printf "<html><head>\n"

for fn; do
    case "$fn" in
		*.css)	printf '<style id="%s">\n' "${fn##*/}";
				cat -- "$fn";
				printf "</style>\n";;
		*.js)	printf '<script id="%s" type="text/javascript">\n' "${fn##*/}";
				cat -- "$fn";
				printf "</script>\n";;
		*.meta) cat -- "$fn";;
    esac
done

printf "</head><body>\n"

for fn; do
    shift
    case "$fn" in
		*.md)	set -- "$@" "$fn";;
		*.html)	cat -- "$fn";;
    esac
done

cmark --unsafe "$@" || exit 1

printf "</body></html>\n"

exit 0
