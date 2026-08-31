#!/usr/bin/perl
#
# Create a CSV file out of the timings of a set of test-tftp.sh logs
#

use strict;

my $delta = 0;
if ($ARGV[0] eq '--delta') {
    $delta = 1;
    shift(@ARGV);
}

my @times = ();

my $nf = 0;
my $nt = 0;
foreach my $f (@ARGV) {
    open(my $fh, '<', $f) or die "$0: $f: $!\n";
    my $it = 0;
    while (defined(my $l = <$fh>)) {
	next if ($l !~ /\btime = ([0-9.]+)/);
	my $t = $1 + 0.0;
	if (!defined($times[$it])) {
	    $times[$it] = [(0.0) x $nf];
	}
	push(@{$times[$it]}, $t);
	$it++;
    }
    close($fh);
    if ($it > $nt) {
	$nt = $it;
    } else {
	while ($it < $nt) {
	    push(@{$times[$it]}, 0.0);
	}
    }
}

if ($delta) {
    foreach my $tl (@times) {
	my $base = shift(@$tl);
	if (!$base) {
	    $tl = [(0.0) x ($nf - 1)];
	} else {
	    $tl = [map { $_/$base - 1.0 } @$tl];
	}
    }
}

foreach my $tl (@times) {
    print join(',', @$tl), "\n";
}
