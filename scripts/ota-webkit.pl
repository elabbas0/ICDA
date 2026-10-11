#!/usr/bin/perl
# The WebKit part of an OTA release (scripts/ota-release.sh): the Linux root's
# files back to back in one pack, and their manifest lines
#   wfile linux/REL SIZE SHA256 OFFSET ...  wpack linux.pack SIZE SHA256 PIECES
#
#   ota-webkit.pl <linux root> <cache dir> <piece bytes> [linux/REL=override ...]
#
# Everything is kept in the cache dir (linux.pack, lines.txt) under a stamp of
# every file's name, size and modification time: an unchanged root - most
# releases - costs a directory walk instead of hashing and copying 550 MB.
# Prints the manifest lines; the pack is <cache dir>/linux.pack.
use strict;
use warnings;
use File::Find;
use Digest::SHA;

my ($root, $cache, $piece, @over) = @ARGV;
die "usage: ota-webkit.pl root cache piece [rel=file ...]\n" unless $root && $cache && $piece;
mkdir $cache unless -d $cache;
my %src;
find({ no_chdir => 1, wanted => sub {
    return unless -f $_;
    (my $rel = $_) =~ s{^\Q$root\E/?}{};
    $src{"linux/$rel"} = $_;
} }, $root);
# loose:linux/REL=file - published on its own (an lfile line), outside the pack,
# so changing it does not make every system fetch the whole pack again
my @loose;
for (@over) {
    my $l = s/^loose://;
    my ($rel, $file) = split /=/, $_, 2;
    next unless -f $file;
    if ($l) { push @loose, [$rel, $file]; delete $src{$rel}; } else { $src{$rel} = $file; }
}
{
    open my $lo, '>', "$cache/loose.txt" or die;
    for my $f (@loose) {
        my $sha = Digest::SHA->new(256)->addfile($f->[1], 'b')->hexdigest;
        print "lfile $f->[0] ", -s $f->[1], " $sha\n";
        print $lo "$f->[0]\t$f->[1]\n";
    }
    close $lo;
}
# names the kernel accepts in a patch: letters, digits and . _ - / + =
my @rels = sort grep { /^[A-Za-z0-9._\/+=-]+$/ } keys %src;

my $stamp = Digest::SHA->new(256);
for my $rel (@rels) {
    my @st = stat $src{$rel};
    $stamp->add("$rel $st[7] $st[9]\n");
}
$stamp = $stamp->hexdigest;

my $lines = "$cache/lines.txt";
if (-f "$cache/stamp" && -f $lines && -f "$cache/linux.pack") {
    open my $s, '<', "$cache/stamp" or die;
    my $old = <$s>;
    close $s;
    if (defined $old && $old eq $stamp) {
        open my $l, '<', $lines or die;
        print while <$l>;
        print STDERR "[ota] WebKit unchanged: cached pack\n";
        exit 0;
    }
}

print STDERR "[ota] WebKit changed: packing " . scalar(@rels) . " files\n";
open my $pack, '>', "$cache/linux.pack" or die "$cache/linux.pack: $!";
binmode $pack;
my $whole = Digest::SHA->new(256);
my ($off, $out) = (0, '');
for my $rel (@rels) {
    open my $in, '<', $src{$rel} or die "$src{$rel}: $!";
    binmode $in;
    my $sha = Digest::SHA->new(256);
    my ($size, $buf) = (0, '');
    while (my $n = read $in, $buf, 1 << 20) {
        $sha->add($buf);
        $whole->add($buf);
        print $pack $buf;
        $size += $n;
    }
    close $in;
    $out .= "wfile $rel $size " . $sha->hexdigest . " $off\n";
    $off += $size;
}
close $pack;
$out .= "wpack linux.pack $off " . $whole->hexdigest . " " . int(($off + $piece - 1) / $piece) . "\n";
open my $l, '>', $lines or die;
print $l $out;
close $l;
open my $s, '>', "$cache/stamp" or die;
print $s $stamp;
close $s;
unlink "$cache/blobs.txt";
print $out;
