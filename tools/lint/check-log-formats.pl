use strict;
use warnings;

# Counts printf conversions in each log::Write format string and compares them
# with the number of arguments that follow. A mismatch means the call reads
# past its arguments and prints whatever is on the stack.
my $examined = 0;
my $bad = 0;

foreach my $path (@ARGV) {
    open(my $fh, '<', $path) or next;
    local $/;
    my $src = <$fh>;
    close($fh);

    while ($src =~ /log::Write\(/g) {
        my $i = pos($src);
        my $depth = 1;
        my $buf = '';
        while ($depth > 0 && $i < length($src)) {
            my $c = substr($src, $i, 1);
            if ($c eq '"') {
                # Copy the whole literal, honouring escapes, so a backslash or
                # a bracket inside a string cannot unbalance the scan.
                $buf .= $c; $i++;
                while ($i < length($src)) {
                    my $s = substr($src, $i, 1);
                    $buf .= $s; $i++;
                    last if $s eq '"';
                    if ($s eq "\\") { $buf .= substr($src, $i, 1); $i++; }
                }
                next;
            }
            if ($c eq '(') { $depth++; }
            elsif ($c eq ')') { $depth--; last if $depth == 0; }
            $buf .= $c;
            $i++;
        }

        my $fmt = '';
        my $rest = $buf;
        while ($rest =~ s/\A\s*"((?:[^"\\]|\\.)*)"\s*//s) {
            $fmt .= $1;
        }
        next if $fmt eq '';
        $examined++;

        my $specs = 0;
        while ($fmt =~ /%[-+ #0-9.*lhzjt]*[diouxXeEfgGaAcspn]/g) { $specs++; }
        # %% is a literal percent, not a conversion.
        my $literal = 0;
        while ($fmt =~ /%%/g) { $literal++; }
        $specs -= $literal;

        # `rest` begins with the comma separating the format from the first
        # argument, so arguments are the top-level commas that remain.
        # Comments can hold commas, which would read as extra arguments.
        $rest =~ s{/\*.*?\*/}{}gs;
        $rest =~ s{//[^\n]*}{}g;
        $rest =~ s/\A\s*,//;
        my $args = ($rest =~ /\S/) ? 1 : 0;
        my $d = 0;
        my $inq = 0;
        my @chars = split //, $rest;
        for (my $k = 0; $k < @chars; $k++) {
            my $c = $chars[$k];
            if ($inq) {
                $inq = 0 if $c eq '"';
                $k++ if $c eq "\\";
                next;
            }
            if ($c eq '"') { $inq = 1; next; }
            if ($c =~ /[\(\[\{]/) { $d++; }
            elsif ($c =~ /[\)\]\}]/) { $d--; }
            elsif ($c eq ',' && $d == 0) { $args++; }
        }

        if ($specs != $args) {
            $bad++;
            printf("MISMATCH %s specs=%d args=%d :: %.60s\n",
                $path, $specs, $args, $fmt);
        }
    }
}

print "examined=$examined mismatched=$bad\n";
