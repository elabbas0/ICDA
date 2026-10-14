use strict;
use warnings;
use File::Temp qw(tempdir);
use File::Path qw(make_path);
use Digest::SHA qw(sha256_hex);
use JSON::PP qw(encode_json);
my $tmp = tempdir(CLEANUP => 1);
make_path("$tmp/root/usr/bin", "$tmp/root/usr/share/icda/models", "$tmp/cache");
open my $base, '>', "$tmp/root/usr/bin/base" or die $!; print $base 'base'; close $base;
my $data = 'GGUF' . ('x' x 33);
open my $model, '>', "$tmp/root/usr/share/icda/models/test.gguf" or die $!; print $model $data; close $model;
open my $catalog, '>', "$tmp/root/usr/share/icda/models/sources.json" or die $!;
print $catalog encode_json([{Name=>'test.gguf',Repo=>'Qwen/test',Revision=>('a' x 40),Sha=>sha256_hex($data),Size=>length($data)}]);
close $catalog;
for (1..2) {
    open my $out, '-|', $^X, 'scripts/ota-webkit.pl', "$tmp/root", "$tmp/cache", 10 or die $!;
    my $manifest = do { local $/; <$out> }; close $out or die 'packaging failed';
    die 'missing direct HTTP source' unless $manifest =~ /^model linux\/usr\/share\/icda\/models\/test.gguf 37 @{[sha256_hex($data)]} https:\/\/huggingface.co\/Qwen\/test\/resolve\/a{40}\/test.gguf$/m;
    die 'models were packed' if $manifest =~ /^(?:lfile|wfile).*models\/.*gguf/m;
    die 'model bytes in WebKit pack' unless -s "$tmp/cache/linux.pack" == 4;
    open my $loose, '<', "$tmp/cache/loose.txt" or die $!;
    die 'model bytes in GitHub payload' if grep /\.gguf/, <$loose>;
    close $loose;
}
print "Direct model URLs, pinned checksums, cache reuse and exclusion from GitHub payload passed\n";
