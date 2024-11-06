#!/usr/bin/perl -w
use strict;
use warnings;

# simplistic http wrapper for spe6ctrl supporting basic automation primitives:
# set=0..100 (absolute intensity), dec=0..100 (decrease brightness), inc=0..100 (increase brightness),
# rgb=0..255:0..255:0..255:0..100 set color/intensity, pat=(dynamic|music|custom):parms

# core modules
use POSIX;
use Data::Dumper;

# parse command line
my ($opt1,@parm,%opts) = ("");
(($_ =~ /^--([a-z-]+)=(.*)$/ && defined($opts{$1} = $2)) ||
 ($_ =~ /^--([a-z-]+)$/      && defined($opts{$1} = 1)) ||
 ($_ =~ /^\-([a-zA-Z]+)$/    && defined($opt1 .= $1)) ||
 push(@parm,$_)) for (@ARGV);
($opts{$_} += 1) for (split //,$opt1);

# set command line defaults
$opts{v}    //= 1;
$opts{port} //= 8801;
$opts{ctrl} //= "./spe6ctrl";

my %level = map { $_ => int(($_*255)/100) } (0..100);
my %blid = map { lc($_) => 1 } grep(/^[0-9a-f]/i, split(",", $opts{btid}//""));
(keys %blid > 0) || die "$0: no bluetooth addresses defined (--btid=<addr[,addr2][,...]>)";

# change to directory containing script
($0 =~ m|^(.*/)|) && chdir $1;
use lib ".";
use http;
(-x $opts{ctrl}) || die "$0: cannot locate $opts{ctrl}";

# create worker to serialize command execution
pipe(my $ctrl_inp, my $ctrl_out);
my $ctrl_fl = fcntl($ctrl_inp, F_GETFD, 0);
fcntl($ctrl_inp, F_SETFD, $ctrl_fl & ~FD_CLOEXEC);
$ctrl_out->autoflush(1);

# kill children when we die
$SIG{INT} = sub { kill("TERM", 0) };
END { (defined $ctrl_out) && kill("TERM", 0) };

# fork worker (run until killed)
if (fork() == 0) {
  while (my $exec = <$ctrl_inp>) {
    print ">$exec";
    my $r = system(split(" ",$exec));
    print ">result=$r\n";
  }
}

# process http request
sub request
{
  my ($line,$path,$base,$extn,$parm,$head,$body) = @_;
  print "base=$base parm=$parm\n";
  (defined $blid{$base}) || return("400 Bad Request (Invalid BLID)");
  ($parm =~ /^[a-z0-9_=&:]+$/) || return("400 Bad Request (Malformed Query)");
  (-x $opts{ctrl}) || return("400 Bad Request (Server Misconfig)");

  my ($cmd,$val);
  my %opt = map { split("=",$_,2) } split("&", $parm);
  if (($line =~ /^GET /) && (defined $blid{$base})) {
    $val = $level{$opt{set}//""}//"";
    ($val ne "") && ($cmd = "--power=1 --set=$val");
    ($val eq "0") && ($cmd = "--power=0");
    $val = $level{$opt{inc}//""}//"";
    ($val =~ /^[1-9]/) && ($cmd = "--power=1 --inc=$val");
    $val = $level{$opt{dec}//""}//"";
    ($val ne "") && ($cmd = "--dec=$val");
    ($val eq "0") && ($cmd = "--power=0");
    # for rgb, set level as component average to enable subsequent intensity-only changes 
    $val = $opt{rgb}//"";
    if (($val =~ /^(\d+):(\d+):(\d+):(\d+)$/) && (defined $level{$4})) {
      my @max = sort {$b <=> $a} ($1,$2,$3);
      my $i = $level{$4};
      my $r = int(($i*$1)/$max[0]);
      my $g = int(($i*$2)/$max[0]);
      my $b = int(($i*$3)/$max[0]);
      $cmd = "--power=1 --rgb=$r:$g:$b:$i --mode=1:1";
    }
    ($val eq "0:0:0") && ($cmd = "--power=0");
    $val = $opt{pat}//"";
    (length($val) < 1024) && ($val =~ s/^(dynamic|music|custom)://) && ($cmd = "--power=1 --$1='$val'");
    (defined $cmd) || return("404 Not Found (Invalid Request)");

    print $ctrl_out "$opts{ctrl} $base $cmd\n";
    return("200 OK (Request Queued)", "term", "");
  }
  return("406 Not Acceptable");
}

# run the mini http server
$SIG{PIPE} = 'IGNORE';
http_config("dbg"=>$opts{v});
http_start($opts{port});
for (;;) {
  http_poll(\&request, 1.0);
}
http_stop();
exit;

