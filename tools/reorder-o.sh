#!/bin/sh

cmd="$1"
shift 1

findpos() {
    while :; do
	[ "$#" -lt 2 ] && { echo "no -o option found" >&2; return 1; }
	[ "x$1" = "x-o" ] && { echo "$#"; return 0; }
	shift 1;
    done
}

# cmd x x x -o y z z z

nx=$(findpos "$@")
[ -z "$nx" ] && exit 1

n_before=$(expr "$#" - "$nx")
n_after=$(expr "$nx" - 2)
outfile=$(shift $n_before; printf '%s:' "$2")
outfile="${outfile%?}"

while :; do
    if [ "$n_before" -gt 0 ]
    then
	set -- "$@" "$1";
	shift 1;
	n_before=$(expr "$n_before" - 1);
    elif [ "$n_before" -eq 0 ]
    then
	shift 2;
	n_before="-1";
    elif [ "$n_after" -gt 0 ]
    then
	set -- "$@" "$1"
	shift 1
	n_after=$(expr "$n_after" - 1)
    else
	exec "$cmd" "$@" "$outfile"
	exit 127
    fi
done
echo "something went badly wrong" >&2; exit 1;
