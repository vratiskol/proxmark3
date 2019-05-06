BEGIN {
    print "--[["
    print "These are Proxmark command definitions."
    print "This file is automatically generated from pm3_cmd.h - DON'T EDIT MANUALLY."
    print "--]]"
    print "local __commands = {"
}

#$1 ~ /#define/ && $2 ~ /^CMD_([[:alnum:]_])+/ { print $2, "=", $3, "," }
$1 ~ /#define/ && $2 ~ /^CMD_[A-Za-z0-9_]+/ { sub(/\r/, ""); print $2, "=", $3 "," }

END {
    print "}"
    print "return __commands"
}