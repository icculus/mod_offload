#!/usr/bin/perl -w

use warnings;
use strict;

# unbuffered output.
$| = 1;

require LWP;
require LWP::UserAgent;
require HTTP::Status;
require HTTP::Date;
my $ua = new LWP::UserAgent;  # we create a global UserAgent object
$ua->env_proxy;

sub dumpHttpHeads {
    foreach (@_) {
        my $url = $_;
        my $request = HTTP::Request->new(HEAD => $url);
        my $response = $ua->request($request);
        if ($response->is_error()) {
            print("HEAD request to $url failed.\n");
        } else {
            print("$url:\n");
            my @keys = $response->header_field_names();
            foreach (@keys) {
                print("  $_: " . $response->header($_) . "\n");
            }
            print("\n\n");
        }
    }
}


sub loadMetadata {
    my $fname = shift;
    return undef if not open(FH, '<', $fname);
    my %retval;
    while (not eof(FH)) {
        my $key = <FH>;
        my $val = <FH>;
        chomp($key) if (defined $key);
        chomp($val) if (defined $val);
        #print("'$key' == '$val'\n");
        $retval{$key} = $val;
    }
    close(FH);

    if (not defined $retval{'X-Offload-Hostname'}) {
        $retval{'X-Offload-Hostname'} = 'icculus.org';
    }
    return(%retval);
}

sub usage {
    die("USAGE: $0 <offloaddir> [--outputurls] [--nukeshortfiles] [--youngerthan=X]\n")
}

my $youngerthan = undef;
my $nukeshortfiles = 0;
my $outputurls = 0;
my $offloaddir = undef;
foreach (@ARGV) {
    $nukeshortfiles = 1, next if ($_ eq '--nukeshortfiles');
    $outputurls = 1, next if ($_ eq '--outputurls');
    $youngerthan = $1, next if (/\A--youngerthan=(\d+)\Z/);
    $offloaddir = $_, next if not defined $offloaddir;
    usage();
}

usage() if (not defined $offloaddir);

opendir(DIRH, $offloaddir) || die("Couldn't open directory [$offloaddir]: $!");

my $diskrecovered = 0;
my $headrequests = 0;
my $filesseen = 0;
my $filesdelete = 0;
my $totalfilespace = 0;

print("\n");
print("mod_offload cleanup script starting up...\n");
if (defined $youngerthan) {
    print("Only checking files younger than $youngerthan days.\n");
    $youngerthan *= 24 * 60 * 60;  # convert days to seconds.
} else {
    print("Checking all files.\n");
}

while (my $f = readdir(DIRH)) {
    # '7' is the file size info in stat().
    # '9' is the mtime info in stat().
    my $filespace = 0;

    $filesseen++;

    if ($f =~ /\Adebug-/) {
        print(" - Deleting debug file '$f'.\n");
        my @statbuf = (stat($f));
        my $size = 0;
        $size = $statbuf[7] if @statbuf;
        $diskrecovered += $size;
        $totalfilespace += $size;
        $filesdelete++;
        unlink("$offloaddir/$f");
    }

    next if (not $f =~ /\A(meta|file)data-/);
    my ($filetype, $etag) = ($f =~ /\A(meta|file)data-(.*)\Z/);
    my $metadatapath = $offloaddir . '/metadata-' . $etag;
    my $filedatapath = $offloaddir . '/filedata-' . $etag;

    my $filecachesize = (stat($filedatapath))[7];

    my @metastat = stat($metadatapath);
    my $filecachemtime = $metastat[9];

    $filespace += $metastat[7] if (-f $metadatapath);
    $filespace += $filecachesize if (-f $filedatapath);

    $totalfilespace += $filespace;

    if ((not -f $filedatapath) || (not -f $metadatapath)) {
        unlink $metadatapath;
        unlink $filedatapath;
        $filesdelete++;
        $diskrecovered += $filespace;
        next;
    }

    next if ($filetype eq 'file');
    next if ((defined $youngerthan) && ((time()-$metastat[9]) > $youngerthan));

    my %metadata = loadMetadata($metadatapath);
    next if (not %metadata);

    my $tmp = $metadata{'ETag'};
    $tmp = '"BOGUSSTRING"' if (not defined $tmp);
    $tmp =~ s/\A\"(.*?)\"\Z/$1/;
    if ($tmp ne $etag) {
        print("File '$metadatapath' is bogus.\n");
        $diskrecovered += $filespace;
        unlink $metadatapath;
        unlink $filedatapath;
        $filesdelete++;
        next;
    }

    my $len = $metadata{'Content-Length'};
    my $hostname = $metadata{'X-Offload-Hostname'};
    my $origurl = $metadata{'X-Offload-Orig-URL'};
    my $url = 'http://' . $hostname . $origurl;

    if ($outputurls) {
        print "$url\n";
        next;
    }

    $headrequests++;
    my $request = HTTP::Request->new(HEAD => $url);
    my $response = $ua->request($request);

    print(" - $url ($etag) ... ");

    my $dokill = 0;
    my $httpcode = $response->code();
    if ($httpcode == 404) {
        print("is no longer on base server.");
        $dokill = 1;
    } elsif ($response->is_error()) {
        # everything else we ignore for now.
        print("status unknown (HTTP error $httpcode).");
    } elsif (($nukeshortfiles) && ($len != $filecachesize)) {
        $dokill = 1;
        print("Cached file is wrong size.");
    } else {
        my $hetag = $response->header('ETag');
        $hetag = '' if (not defined $hetag);
        $dokill = 1 if ($hetag ne "\"$etag\"");
        # !!! FIXME: check other attributes...
        print("out of date in some way.") if ($dokill);
    }

    if ($dokill) {
        print("  DELETE!\n");
        $diskrecovered += $filespace;
        unlink $metadatapath;
        unlink $filedatapath;
        $filesdelete++;
    } else {
        print("KEEP!\n");
    }
}

closedir(DIRH);

if (not $outputurls) {
    print("Recovered $diskrecovered bytes of $totalfilespace.\n");
    print("$filesseen files seen, $filesdelete deleted.\n");
    print("$headrequests HTTP HEAD requests.\n");
}

exit 0;

