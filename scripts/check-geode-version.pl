#!/usr/bin/env perl
use strict;
use warnings;
use JSON::PP qw(decode_json);

my ($path) = @ARGV;
die "Usage: check-geode-version.pl loader-library\n" unless defined $path && @ARGV == 1;
open my $file, '<:raw', $path or die "Cannot read Geode: $!\n";
my $size = -s $file;
die "Invalid Geode library size\n" unless $size && $size <= 256 * 1024 * 1024;
local $/;
my $data = <$file>;
my $magic = substr($data, 0, 4);
if (substr($magic, 0, 2) eq 'MZ') {
    die "Invalid PE loader\n" unless length($data) >= 64;
    my $pe = unpack('V', substr($data, 60, 4));
    die "Invalid PE loader\n" unless $pe >= 64 && $pe + 24 <= length($data)
        && substr($data, $pe, 4) eq "PE\0\0";
} else {
    my %mach_magic = map { $_ => 1 } qw(feedface feedfacf cefaedfe cffaedfe cafebabe bebafeca cafebabf bfbafeca);
    die "Geode is not a PE or Mach-O library\n" unless length($data) >= 32 && $mach_magic{unpack('H*', $magic)};
}

my %versions;
for my $string (split /\0/, $data) {
    next unless $string =~ /^\s*\{/ && $string =~ /"geode\.loader"/;
    my $metadata = eval { decode_json($string) };
    next if $@ || ref($metadata) ne 'HASH' || ($metadata->{id} // '') ne 'geode.loader';
    my $version = $metadata->{version} // '';
    die "Invalid embedded Geode version\n" if ref($version)
        || ref($metadata->{geode}) || ($metadata->{geode} // '') ne $version
        || $version !~ /\A(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\z/;
    $versions{$version} = 1;
}
die "Could not verify a unique embedded Geode version\n" unless keys(%versions) == 1;
my ($version) = keys %versions;
my ($major, $minor) = split /\./, $version;
die "Geode $version is incompatible; OBS Jukebox requires Geode 5.10.x or a later 5.x release\n"
    unless $major == 5 && $minor >= 10;
print "$version\n";
