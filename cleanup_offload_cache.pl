#!/usr/bin/perl -w

use warnings;
use strict;

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


my $offloaddir = shift;
die("USAGE: $0 <offloaddir>\n") if (not defined $offloaddir);

opendir(DIRH, $offloaddir) || die("Couldn't open directory [$offloaddir]: $!");
my @dirfiles = readdir(DIRH);
closedir(DIRH);

# unbuffered output.
$| = 1;

foreach (@dirfiles) {
    next if not /\A(meta|file)data-/;
    my ($filetype, $etag) = /\A(meta|file)data-(.*)\Z/;
    my $metadatapath = $offloaddir . '/metadata-' . $etag;
    my $filedatapath = $offloaddir . '/filedata-' . $etag;

    if ((not -f $filedatapath) || (not -f $metadatapath)) {
        unlink $metadatapath;
        unlink $filedatapath;
        next;
    }

    next if ($filetype eq 'file');

    my %metadata = loadMetadata($metadatapath);
    next if (not %metadata);

    my $tmp = $metadata{'ETag'};
    $tmp = '"BOGUSSTRING"' if (not defined $tmp);
    $tmp =~ s/\A\"(.*?)\"\Z/$1/;
    if ($tmp ne $etag) {
        print("File '$metadatapath' is bogus.\n");
        unlink $metadatapath;
        unlink $filedatapath;
        next;
    }

    my $hostname = $metadata{'X-Offload-Hostname'};
    my $origurl = $metadata{'X-Offload-Orig-URL'};
    my $url = 'http://' . $hostname . $origurl;
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
    } else {
        my $hetag = $response->header('ETag');
        $hetag = '' if (not defined $hetag);
        $dokill = 1 if ($hetag ne "\"$etag\"");
        # !!! FIXME: check other attributes...
        print("out of date in some way.") if ($dokill);
    }

    if ($dokill) {
        print("  DELETE!\n");
        unlink $metadatapath;
        unlink $filedatapath;
    } else {
        print("KEEP!\n");
    }
}

exit 0;

