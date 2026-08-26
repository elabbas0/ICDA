#!/usr/bin/perl
# Generic HMP driver for interactive testing of ICDA.
# Args: sequence of ops:
#   k:<keys>        -> sendkey <keys>
#   m:<dx>,<dy>     -> mouse_move dx dy
#   r:<n>x<dx>,<dy> -> n mouse_move steps of dx,dy each
#   b:<mask>        -> mouse_button <mask>
#   s:<name>        -> screendump /workspace/<name>.ppm
#   w:<secs>        -> sleep
use strict;
use warnings;
use IO::Socket::INET;
$| = 1;

my $s = IO::Socket::INET->new(PeerAddr => '127.0.0.1', PeerPort => 4444,
                              Proto => 'tcp', Timeout => 5) or die "no qmp";
sub qread {
    my $d = ''; my $b;
    while (1) {
        my $r = sysread($s, $b, 65536);
        last unless defined $r && $r > 0;
        $d .= $b;
        last if $d =~ /\}\r?\n/;
    }
    return $d;
}
qread();
print $s qq'{"execute":"qmp_capabilities"}\n';
qread();

sub hmp {
    my ($cmd) = @_;
    my $j = '{"execute":"human-monitor-command","arguments":{"command-line":"' . $cmd . '"}}';
    print $s "$j\n";
    return qread();
}

for my $op (@ARGV) {
    if ($op =~ /^k:(.+)/) {
        hmp("sendkey $1");
        select(undef, undef, undef, 0.05);
    } elsif ($op =~ /^m:(-?\d+),(-?\d+)/) {
        hmp("mouse_move $1 $2");
        select(undef, undef, undef, 0.01);
    } elsif ($op =~ /^r:(\d+)x(-?\d+),(-?\d+)/) {
        my ($n, $dx, $dy) = ($1, $2, $3);
        for (1..$n) {
            hmp("mouse_move $dx $dy");
            select(undef, undef, undef, 0.008);
        }
    } elsif ($op =~ /^b:(\d+)/) {
        hmp("mouse_button $1");
        select(undef, undef, undef, 0.05);
    } elsif ($op =~ /^w:([\d.]+)/) {
        select(undef, undef, undef, $1);
    } elsif ($op =~ /^s:(\w[\w-]*)/) {
        hmp("screendump /workspace/$1.ppm");
        select(undef, undef, undef, 0.9);
        print "shot $1\n";
    }
}
print "ops complete\n";
