#!/usr/bin/env perl
use strict;
use warnings;
use JSON::PP;
use Digest::SHA;
use File::Find;
use File::Copy qw(copy);
use File::Path qw(make_path remove_tree);

# Receipts contain only installer-owned targets. Changed files are left in place;
# keeping their receipt allows a later retry after the user resolves the change.
sub digest {
    my ($path) = @_;
    die "Symbolic link in installation: $path\n" if -l $path;
    my $sha = Digest::SHA->new(256);
    my @paths;
    if (-d $path) { find({ no_chdir => 1, wanted => sub { push @paths, $File::Find::name } }, $path); }
    elsif (-f $path) { @paths = ($path); }
    else { return ''; }
    for my $item (sort @paths) {
        die "Symbolic link in installation: $item\n" if -l $item;
        $sha->add(substr($item, length($path)), "\0", (-d $item ? 'd' : 'f'), "\0", (stat($item))[2] & 07777, "\0");
        if (-f $item) { open my $in, '<:raw', $item or die "$item: $!\n"; $sha->addfile($in); }
    }
    return $sha->hexdigest;
}
sub restore {
    my ($source, $target) = @_;
    if ($^O eq 'darwin') {
        system('/usr/bin/ditto', $source, $target) == 0 or die "Restore $target failed\n";
        return;
    }
    if (-d $source) {
        make_path($target);
        opendir my $dir, $source or die "$source: $!\n";
        for my $name (grep { $_ ne '.' && $_ ne '..' } readdir $dir) { restore("$source/$name", "$target/$name"); }
    } else { copy($source, $target) or die "Restore $target: $!\n"; }
    chmod((stat($source))[2] & 07777, $target) or die "Restore permissions: $!\n";
}
sub read_text {
    my ($path) = @_;
    return '' unless defined($path) && -f $path;
    open my $in, '<', $path or die "$path: $!\n";
    local $/; return <$in>;
}
sub filesystems {
    my ($text) = @_;
    return () unless $text =~ /^filesystems=([^\r\n]*)/m;
    return grep { length } split /;/, $1;
}
sub added_grants {
    my ($target, $backup) = @_;
    my %before = map { $_ => 1 } filesystems(read_text($backup));
    return [grep { !$before{$_} } filesystems(read_text($target))];
}
sub removed_grants {
    my ($target, $backup) = @_;
    my %after = map { $_ => 1 } filesystems(read_text($target));
    return [grep { !$after{$_} } filesystems(read_text($backup))];
}
sub grant_root { my ($value) = @_; $value =~ s/:(?:ro|rw|create)$//; return $value; }
sub undo_grants {
    my ($entry) = @_;
    return 0 unless @{$entry->{grants} // []};
    my $text = read_text($entry->{target});
    my %added = map { $_ => 1 } @{$entry->{grants}};
    my @remaining = grep { !$added{$_} } filesystems($text);
    my %roots = map { grant_root($_) => 1 } @remaining;
    push @remaining, grep { !$roots{grant_root($_)} } @{$entry->{removed_grants} // []};
    my $line = 'filesystems=' . join(';', @remaining) . ';';
    # If the user removed the complete key/section, leave that decision intact.
    $text =~ s/^filesystems=[^\r\n]*/$line/m;
    open my $out, '>', "$entry->{target}.obs-jukebox-undo" or die "$!\n";
    print $out $text; close $out or die "$!\n";
    chmod((stat($entry->{target}))[2] & 07777, "$entry->{target}.obs-jukebox-undo");
    rename "$entry->{target}.obs-jukebox-undo", $entry->{target} or die "$!\n";
    return 1;
}
sub save {
    my ($path, $entries) = @_;
    open my $out, '>', "$path.tmp" or die "$path: $!\n";
    print $out JSON::PP->new->canonical->encode($entries);
    close $out or die "$path: $!\n";
    rename "$path.tmp", $path or die "$path: $!\n";
}
my ($action, $directory, $target, $backup) = @ARGV;
die "Usage: install-state.pl record|undo|undo-latest directory [target backup]\n" unless defined $directory;
if ($action eq 'undo-latest') {
    opendir my $dir, $directory or die "No installation receipts found.\n";
    my @receipts = sort grep { -f "$directory/$_/receipt.json" && -f "$directory/$_/complete" } readdir $dir;
    @receipts or die "No installation receipts found.\n";
    $directory .= '/' . $receipts[-1];
    $action = 'undo';
}
my $receipt = "$directory/receipt.json";
my $entries = [];
if (-f $receipt) { open my $in, '<', $receipt or die "$receipt: $!\n"; local $/; $entries = decode_json(<$in>); }
if ($action eq 'record') {
    my $before = defined($backup) && -e $backup ? digest($backup) : '';
    my $flatpak = $target =~ m{[\\/]flatpak[\\/]overrides[\\/]com\.obsproject\.Studio$};
    push @$entries, { target => $target, backup => $backup, before => $before, installed => digest($target), grants => ($flatpak ? added_grants($target, $backup) : []), removed_grants => ($flatpak ? removed_grants($target, $backup) : []) };
    save($receipt, $entries);
} elsif ($action eq 'undo') {
    my $preserved = 0;
    for my $entry (reverse @$entries) {
        next if $entry->{undone};
        my $path = $entry->{target};
        if (!-l $path && -f $path && digest($path) ne $entry->{installed} && undo_grants($entry)) {
            $entry->{undone} = JSON::PP::true; save($receipt, $entries);
            print "Removed installed Flatpak grants; preserved other settings: $path\n"; next;
        }
        if (-l $path || digest($path) ne $entry->{installed}) { print "Preserved changed file: $path\n"; $preserved++; next; }
        if ($entry->{before} ne '') { die "Backup changed: $entry->{backup}\n" unless digest($entry->{backup}) eq $entry->{before}; }
        if (-d $path) { remove_tree($path, { error => \my $errors }); die "Cannot remove $path\n" if @$errors; }
        else { unlink $path or die "Cannot remove $path: $!\n"; }
        restore($entry->{backup}, $path) if $entry->{before} ne '';
        $entry->{undone} = JSON::PP::true;
        save($receipt, $entries);
        print "Restored: $path\n";
    }
    print "Undo complete. $preserved changed file(s) preserved; backups: $directory\n";
    rename $receipt, "$directory/receipt.undone.json" or die "$!\n" unless $preserved;
} else { die "Unknown action: $action\n"; }
