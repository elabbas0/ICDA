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
use JSON::PP qw(decode_json);

my ($root, $cache, $piece, @over) = @ARGV;
die "usage: ota-webkit.pl root cache piece [rel=file ...]\n" unless $root && $cache && $piece;
mkdir $cache unless -d $cache;
my %src;
find({ no_chdir => 1, wanted => sub {
    return unless -f $_;
    (my $rel = $_) =~ s{^\Q$root\E/?}{};
    return if $rel =~ m{^usr/share/icda/models/};
    $src{"linux/$rel"} = $_;
} }, $root);
# Model bytes are downloaded by ICDA over HTTP(S) from the official source,
# never packed or uploaded to GitHub. Model rows are compatibility metadata:
# 1.8.23 uses them to preserve installed weights during an upgrade, while newer
# updaters ignore them entirely. The assistant owns all model downloads.
my $catalog = "$root/usr/share/icda/models/sources.json";
if (-f $catalog) {
    open my $in, '<', $catalog or die "$catalog: $!";
    my $text = do { local $/; <$in> }; close $in;
    my $models = decode_json($text);
    $models = [$models] if ref($models) eq 'HASH';
    die 'invalid model catalog' unless ref($models) eq 'ARRAY';
    for my $m (@$models) {
        die 'invalid model source' unless
            $m->{Name} =~ /^[A-Za-z0-9._-]+\.gguf$/ &&
            $m->{Repo} =~ m{^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$} &&
            $m->{Revision} =~ /^[a-f0-9]{40}$/ && $m->{Sha} =~ /^[a-f0-9]{64}$/;
        my $size = $m->{Size} || -s "$root/usr/share/icda/models/$m->{Name}";
        die 'model size missing' unless $size && $size > 0;
        print "model linux/usr/share/icda/models/$m->{Name} $size $m->{Sha} ",
              "https://huggingface.co/$m->{Repo}/resolve/$m->{Revision}/$m->{Name}\n";
    }
}
# loose:linux/REL=file - published on its own (an lfile line), outside the pack,
# so changing it does not make every system fetch the whole pack again
my (@loose, @append);
for (@over) {
    my $a = s/^append://;
    my $l = s/^loose://;
    my ($rel, $file) = split /=/, $_, 2;
    die 'model weights must be downloaded directly over HTTP(S)'
        if $rel =~ m{^linux/usr/share/icda/models/.*\.(?:gguf|bin)$};
    next unless -f $file;
    if ($l) { push @loose, [$rel, $file]; delete $src{$rel}; }
    elsif ($a) { push @append, [$rel, $file]; delete $src{$rel}; }
    else { $src{$rel} = $file; }
}
{
    open my $lo, '>', "$cache/loose.txt" or die;
    for my $f (@loose) {
        my $sha = Digest::SHA->new(256)->addfile($f->[1], 'b')->hexdigest;
        my $size = -s $f->[1];
        # over the host's file size limit: published as REL.part0 ...
        my $parts = $size > $piece ? int(($size + $piece - 1) / $piece) : 0;
        print "lfile $f->[0] $size $sha", ($parts ? " parts $parts" : ''), "\n";
        print $lo "$f->[0]\t$f->[1]\t$parts\n";
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

my $base_stamp = $stamp;
if (@append) {
    my $full = Digest::SHA->new(256);
    $full->add("base $base_stamp\n");
    for my $f (sort { $a->[0] cmp $b->[0] } @append) {
        my @st = stat $f->[1];
        $full->add("append $f->[0] $st[7] $st[9]\n");
    }
    $stamp = $full->hexdigest;
}

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

if (@append) {
    my $base_pack = "$cache/base-linux.pack";
    my $base_lines = "$cache/base-lines.txt";
    my $base_stamp_file = "$cache/base.stamp";
    my $base_ok = 0;
    if (-f $base_stamp_file && -f $base_pack && -f $base_lines) {
        open my $s, '<', $base_stamp_file or die;
        my $old = <$s>;
        close $s;
        $base_ok = defined($old) && $old eq $base_stamp;
    }

    # Adopt the existing WebKit cache as the append base when it exactly
    # matches the files before the appended package. Rename keeps this initial
    # conversion cheap and preserves all existing archive offsets and blobs.
    if (!$base_ok && -f "$cache/stamp" && -f $lines && -f "$cache/linux.pack") {
        open my $s, '<', "$cache/stamp" or die;
        my $old = <$s>;
        close $s;
        if (defined($old) && $old eq $base_stamp) {
            rename "$cache/linux.pack", $base_pack or die "rename base pack: $!";
            rename $lines, $base_lines or die "rename base lines: $!";
            open my $b, '>', $base_stamp_file or die;
            print $b $base_stamp;
            close $b;
            $base_ok = 1;
        }
    }

    # If WebKit itself changed, rebuild its base without the appended Codex
    # members. Future Codex-only changes can then reuse this exact base.
    if (!$base_ok) {
        print STDERR "[ota] WebKit base changed: packing " . scalar(@rels) . " files\n";
        my $tmp_pack = "$base_pack.tmp";
        open my $pack, '>', $tmp_pack or die "$tmp_pack: $!";
        binmode $pack;
        my $whole = Digest::SHA->new(256);
        my ($off, $out) = (0, '');
        for my $rel (@rels) {
            open my $in, '<', $src{$rel} or die "$src{$rel}: $!";
            binmode $in;
            my $sha = Digest::SHA->new(256);
            my ($size, $buf) = (0, '');
            while (my $n = read $in, $buf, 1 << 20) {
                $sha->add($buf); $whole->add($buf); print $pack $buf; $size += $n;
            }
            close $in;
            $out .= "wfile $rel $size " . $sha->hexdigest . " $off\n";
            $off += $size;
        }
        close $pack;
        $out .= "wpack linux.pack $off " . $whole->hexdigest . " " . int(($off + $piece - 1) / $piece) . "\n";
        open my $bl, '>', "$base_lines.tmp" or die;
        print $bl $out; close $bl;
        rename $tmp_pack, $base_pack or die "rename base pack: $!";
        rename "$base_lines.tmp", $base_lines or die "rename base lines: $!";
        open my $bs, '>', $base_stamp_file or die;
        print $bs $base_stamp; close $bs;
    }

    print STDERR "[ota] appending " . scalar(@append) . " package files\n";
    my $tmp_pack = "$cache/linux.pack.tmp";
    open my $base, '<', $base_pack or die "$base_pack: $!";
    binmode $base;
    open my $pack, '>', $tmp_pack or die "$tmp_pack: $!";
    binmode $pack;
    my $whole = Digest::SHA->new(256);
    my $off = 0;
    my $buf = '';
    while (my $n = read $base, $buf, 1 << 20) {
        print $pack $buf; $whole->add($buf); $off += $n;
    }
    close $base;
    open my $base_idx, '<', $base_lines or die "$base_lines: $!";
    my $out = '';
    while (my $line = <$base_idx>) { $out .= $line unless $line =~ /^wpack /; }
    close $base_idx;
    for my $f (sort { $a->[0] cmp $b->[0] } @append) {
        my ($rel, $file) = @$f;
        open my $in, '<', $file or die "$file: $!";
        binmode $in;
        my $sha = Digest::SHA->new(256);
        my $size = 0;
        while (my $n = read $in, $buf, 1 << 20) {
            $sha->add($buf); $whole->add($buf); print $pack $buf; $size += $n;
        }
        close $in;
        $out .= "wfile $rel $size " . $sha->hexdigest . " $off\n";
        $off += $size;
    }
    close $pack;
    $out .= "wpack linux.pack $off " . $whole->hexdigest . " " . int(($off + $piece - 1) / $piece) . "\n";
    open my $l, '>', "$lines.tmp" or die;
    print $l $out; close $l;
    rename $tmp_pack, "$cache/linux.pack" or die "rename appended pack: $!";
    rename "$lines.tmp", $lines or die "rename appended lines: $!";
    open my $s, '>', "$cache/stamp" or die;
    print $s $stamp; close $s;
    unlink "$cache/blobs.txt", "$cache/blobs-stamp";
    print $out;
    exit 0;
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
unlink "$cache/blobs.txt", "$cache/blobs-stamp";
print $out;
