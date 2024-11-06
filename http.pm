# simplistic http server for internal low-security apps
package http;
use strict;
use warnings;
use 5.010;

# core modules
use Socket;
use POSIX;
use Data::Dumper;

require Exporter;
our @ISA = ("Exporter");
our @EXPORT = ("http_config", "http_start", "http_stop", "http_poll", '%http_cfg');

our $log = *STDERR;
our %cfg;
our %conn;
our $sel_wait = "";
our $sel_send = "";
our $sock_http;

# default options
$cfg{dbg}     //= 0;                  # enable debugging (higher == more)
$cfg{port}    //= 8080;               # http listen port
$cfg{maxconn} //= 25;                 # maximum concurrent http connections (drops extras)
$cfg{maxdata} //= 32000;              # maximum http request size (drops if oversized)
$cfg{maxtime} //= 30;                 # maximum http request time (drops on timeout)
$cfg{chunk}   //= 512*1024;
$cfg{server}  //= "micro-http/1.0";
$cfg{read_term} = \&http_term;
$cfg{read_data} = \&http_data;
$cfg{read_file} = \&http_file;

our %mimetype = (
  "avi"  => "video/x-msvideo",
  "css"  => "text/css",
  "csv"  => "text/csv",
  "gif"  => "image/gif",
  "hls"  => "video/x-mpegurl",
  "htm"  => "text/html",
  "html" => "text/html",
  "jpg"  => "image/jpeg",
  "jpeg" => "image/jpeg",
  "js"   => "text/javascript",
  "m2ts" => "video/mpeg",
  "m3u"  => "video/x-mpegurl",
  "m3u8" => "video/x-mpegurl",
  "mkv"  => "video/x-matroska",
  "mov"  => "video/quicktime",
  "mp3"  => "audio/mpeg",
  "mp4"  => "video/mp4",
  "mpg"  => "video/mpeg",
  "mpeg" => "video/mpeg",
  "png"  => "image/png",
  "svg"  => "image/svg+xml",
  "ts"   => "video/mp2t",
  "ttm"  => "font/ttf",
  "vob"  => "video/dvd",
  "xml"  => "application/xml",
);

# quickie packed address to addr:port converter
sub inet_ntoap { my ($p,$a) = sockaddr_in($_[0]); return(inet_ntoa($a).":$p") }
# quickie indent formatter (for easier output/log review)
sub indent { return(" ".join("\n ",split(/[\n\r]+/,$_[0]))."\n"); }

# return a termination object
sub http_term
{
  my ($seek,$data,$priv) = @_;
  ($seek eq "size") && return(0);
  ($seek =~ /^\d+/) && return("");
}

# return a data-blob object
sub http_data
{
  my ($seek,$data,$priv) = @_;
  ($cfg{dbg} > 2) && printf $log ("http_data: seek=%s data=%d\n", $seek, length($data));
  # size equals size of data blob
  ($seek eq "size") && return(length($data));
  # return a data chunk
  ($seek =~ /^\d+/) && return(substr($data, $seek, 1<<20));
}

# return a file object
sub http_file
{
  my ($seek,$file,$priv) = @_;

  # size equals filesize
  ($seek eq "size") && return(-s $file);

  # return the data
  if ($seek =~ /^\d+/) {
    my $data = "";
    if (open(my $fd, "<", $file)) {
      sysseek($fd, $seek, 0);
      sysread($fd, $data, 1<<20);
      close($fd);
    } else {
     ($cfg{dbg} > 0) && printf $log ("$0: cannot open $file: $!\n");
    }
    return($data);
  }
}

sub http_config
{
  while (@_ >= 2) {
    my ($k,$v) = (shift,shift);
    $cfg{$k} = $v;
  }
}

# setup http listen port
sub http_start
{
  my ($port) = @_;
  $SIG{PIPE} = 'IGNORE';
  (defined $cfg{log}) && ($log = $cfg{log});

  socket($sock_http, PF_INET, SOCK_STREAM|Socket::SOCK_NONBLOCK, getprotobyname('tcp')) || die "$0: socket $!\n";
  setsockopt($sock_http, SOL_SOCKET, SO_REUSEADDR, 1) || die "$0: setsockopt SO_REUSEADDR $!\n";
  bind($sock_http, sockaddr_in($port, INADDR_ANY)) || die "$0: bind $port $!\n";
  listen($sock_http, 10) || die "$0: `listen: $!";
}

sub http_stop
{
}

sub http_poll
{
  my ($request,$timeout) = @_;

  if ($sel_wait eq "") {
    vec($sel_wait,fileno($sock_http),1) = 1;
    ($sel_wait eq "") && die "still empty";
    my $when = POSIX::strftime("%F %T", localtime);
    ($cfg{dbg} > 0) && printf $log ("%s: listening for http on %s(%d)\n", $when, inet_ntoap(getsockname($sock_http)), fileno($sock_http));
  }

  my $n = select(my $wait0=$sel_wait, my $send0=$sel_send, undef, $timeout);
  ($n < 0) && ($cfg{dbg} > 0) && printf $log ("select error: n=$n %s,%s $!\n", unpack("b*",$wait0), unpack("b*",$sel_wait));

  my $time = time();
  my $when = POSIX::strftime("%F %T", localtime($time));
  my $date = POSIX::strftime("%a, %d %b %Y %H:%M:%S GMT", gmtime($time));

  # accept http connections
  my $sock_conn;
  if ((defined $sock_http) && (vec($wait0,fileno($sock_http),1)) && (my $from = accept($sock_conn, $sock_http)))
  {
    (keys %conn > $cfg{maxconn}) && close($sock_conn) && next;

    my $sock = fileno($sock_conn);
    vec($sel_wait,$sock,1) = 1;
    $conn{$sock}{sock} = $sock_conn;
    $conn{$sock}{from} = join('.',unpack('x4 CCCC',$from)).":".unpack('x2 n',$from);
    $conn{$sock}{time} = $time;
    $conn{$sock}{size} = -1;
    $conn{$sock}{head} = "";
    $conn{$sock}{body} = "";
    $conn{$sock}{bind} = inet_ntoap(getsockname($sock_conn));
    ($cfg{dbg} > 1) && print $log "$when: http($sock) accept $conn{$sock}{from} on $conn{$sock}{bind}\n";
  }

  # process incoming http request
  foreach my $sock (grep {vec($wait0,$_,1)} keys %conn)
  {
    # receive new data (use sysread for eof/close detection)
    my $data = "";
    my $r = sysread($conn{$sock}{sock}, $data, 8192) // -1;
    ($cfg{dbg} > 1) && print $log "$when: http($sock): read $r\n";
    $conn{$sock}{head} .= $data;
    if ($r <= 0) {
      ($cfg{dbg} > 1) && print $log "$when: http($sock) peer close: $!\n";
      $conn{$sock}{read} = \&http_term;
    }
    #($r > 0) && print "$0: read $r\n";

    # check for head complete (and overflow extra to body)
    my $head = index($conn{$sock}{head},"\r\n\r\n");
    if ($head > 0) {
      $conn{$sock}{body} .= substr($conn{$sock}{head},$head+4);
      $conn{$sock}{head} = substr($conn{$sock}{head},0,$head+4);
      $conn{$sock}{size} = ($conn{$sock}{head} =~ /\sContent-Length:\s*(\d+)/i ? $1 : 0);
    }

    # check for body complete
    if (($conn{$sock}{size} >= 0) && (length($conn{$sock}{body}) >= $conn{$sock}{size})) {
      vec($sel_wait,$sock,1) = 0;

      # log the request
      ($cfg{dbg} > 0) && print $log "$when: http($sock) recv $conn{$sock}{from}\n";
      ($cfg{dbg} > 1) && print $log indent($conn{$sock}{head}.$conn{$sock}{body});
      #printf("  %s\n", join("\n  ",split(/[\n\r]+/,"$conn{$sock}{head}$conn{$sock}{body}")));
    }
  }

  # process completed http requests
  my $fork = 0;
  foreach my $sock (grep {!vec($sel_wait,$_,1)} keys %conn)
  {
    # skip if response is ready
    (defined $conn{$sock}{read}) && next;
    ($cfg{dbg} > 1) && print "$when: http($sock) process\n";

    my $from = $conn{$sock}{from};
    my $bind = $conn{$sock}{bind};
    my $head = $conn{$sock}{head};
    my $body = $conn{$sock}{body};

    # remove carriage returns and other non-printable (preserve linefeeds)
    $head =~ s/[^\x20-\x7e\n]//g;
    # parse url special
    my $line = substr($head,0,index($head,"\n"));
    $line =~ s| HTTP/\d\.\d$||;
    $line =~ tr/+/ /;
    # percent url decode
    $line =~ s|%([A-Fa-f0-9]{2})|chr(hex $1)|eg;
    my $parm = "";
    if (index($line,"?") > 0) {
      $parm = substr($line,index($line,"?")+1);
      $line = substr($line,0,index($line,"?"));
    }
    my $path = substr($line,index($line," ")+1);

    # remove weird paths (strange chars, periods after slash, etc)
    if (($path =~ m|^[^/A-Za-z0-9]|) || ($path =~ m|/\.|)) {
      ($cfg{dbg} > 0) && print $log "$when: invalid path: $path\n";
      $path = "";
    }

    # extract filename 
    my $file = ($path =~ s|([-A-Za-z0-9_.:]+)$||) ? $1 : "";
    # extract extension
    my $extn = ($file =~ s|\.([A-Za-z0-9]+)$||) ? $1 : "";
    ($cfg{dbg} > 2) && print $log "path=$path file=$file extn=$extn line=$line\n";
    # parse the request
    my $t0 = time();
    my ($resp,$kind,$data) = &$request($line,$path,$file,$extn,$parm,$head,$body);
    my $t1 = time();
    ($cfg{dbg} > 0) && ($t1-$t0 > 1) && print $log "slow request processing: t0=$t0 t1=$t1\n";
    if (defined $resp) {
      (substr($resp,0,4) ne "HTTP") && ($resp = "HTTP/1.1 $resp");
      (substr($resp,-2) ne "\r\n") && ($resp .= "\r\n");
    }
    $kind //= "term";
    $data //= "";

    # should probably be dynamic
    if ($line =~ /OPTIONS/) {
      $resp //= "HTTP/1.1 204 No Content\r\n".
        "Allow: GET, POST, PROPFIND, OPTIONS\r\n".
        "Accept-Ranges: bytes\r\n";
    }

    # find the reader else use null (likely a 404)
    my $read = $cfg{"read_$kind"};
    ($kind ne "") && (!defined $read) && ($cfg{dbg} > 0) && print "warning: http_config(read_$kind) not defined\n";
    $read //= \&http_term;

    # handle unknown request
    $resp //= "HTTP/1.1 404 Not Found\r\n".
        "Connection: Close\r\n".
        "Content-Type: text/plain\r\n".
        "Content-Length: 0\r\n";

    # figure out content length
    my $size = &$read("size", $data//"", undef);
    #print "req: kind=$kind size=$size resp=$resp data=$data\n";
    #print "size=$size\n";
    $conn{$sock}{rang} = "0,$size";
    $conn{$sock}{read} = $read;
    $conn{$sock}{data} = $data;
    $conn{$sock}{priv} = {};

    # if a range request
    if ((defined $size) && ($head =~ /Range:\s*bytes=(\d*)-(\d*)/i)) {
      my ($beg,$end) = (0,$size-1);
      ($1 ne "") && ($1 >= $beg) && ($1 <= $end) && ($beg = $1);
      ($2 ne "") && ($2 >= $beg) && ($2 <= $end) && ($end = $2);
      $conn{$sock}{rang} = join(",", $beg, $end-$beg+1);

      # enable nonblocking mode and increase send buffer
      my $fl = 0;
      fcntl($conn{$sock}{sock}, F_GETFL, $fl);
      fcntl($conn{$sock}{sock}, F_SETFL, $fl|O_NONBLOCK);
      setsockopt($conn{$sock}{sock}, SOL_SOCKET, SO_SNDBUF, $cfg{chunk}) || die "$0: setsockopt SO_SNDBUF $!\n";

      # add partial header
      $resp = "HTTP/1.1 206 Partial Content\r\n".
        "Content-Length: ".($end-$beg+1)."\r\n".
        "Content-Range: bytes $beg-$end/$size\r\n".
        $resp;
    }

    # complete the header
    ($resp =~ /^HTTP/) || ($resp = "HTTP/1.1 200 OK\r\n".$resp);
    my $keep = ($head =~ m| HTTP/1.1|) ? "keep-alive" : "close";
    ($head =~ /Connection:\s*keep-alive/i) && ($keep = "keep-alive");
    ($head =~ /Connection:\s*close/i)      && ($keep = "close");
    # always close connect after error
    ($resp =~ m|^HTTP/\d\.\d 2\d\d|) || ($keep = "close");
    ($resp =~ /Date:/) || ($resp .= "Date: $date\r\n");
    ($resp =~ /Server:/) || ($resp .= "Server: $cfg{server}\r\n");
    ($resp =~ /Connection:/) || ($resp .= "Connection: $keep\r\n");
    ($resp =~ /Content-Type:/) || ($resp .= "Content-Type: ".($mimetype{$extn}//"text/plain")."\r\n");
    ($resp !~ /Content-Length:/) && (defined $size) && ($resp .= "Content-Length: $size\r\n");
    ($resp !~ /Accept-Ranges:/) && (defined $conn{$sock}{rang}) && ($resp .= "Accept-Ranges: bytes\r\n");
    ($resp =~ /Cache-Control:/) || ($resp .= "Cache-Control: no-cache\r\n");
    ($resp =~ /Access-Control-Allow-Origin/) || ($resp .= "Access-Control-Allow-Origin: *\r\n");
    $resp .= "\r\n";

    # send the header immediately (blocking)
    ($cfg{dbg} > 1) && printf $log ("%s: sending header (%d bytes)\n", $when, length($resp));
    ($cfg{dbg} > 1) && print $log indent($resp);
    my $r = send($conn{$sock}{sock},$resp,0) // -1;
    ($r < 0) && ($cfg{dbg} > 0) && print $log "$when: http($sock) send error $!\n";
    ($r != length($resp)) && die "$0: header send failed: r=$r\n";

    # if forked, only process this session
    ($fork) && last;
  }

  # send/close http transactions
  foreach my $sock (keys %conn)
  {
    my $read = $conn{$sock}{read};
    my $stat = (($read//"") eq \&http_term) ? 1 : 0;

    # send some data
    if (defined $read) {
      # load data for send
      my ($seek,$size) = (defined $conn{$sock}{rang}) ? split(',',$conn{$sock}{rang}) : (0,0);
      my $send = &$read($seek, $conn{$sock}{data}, $conn{$sock}{priv});
      (length($send) > $size) && ($send = substr($send, 0, $size));

      my $r = send($conn{$sock}{sock}, $send, 0) // -1;
      ($cfg{dbg} > 2) && printf $log ("$when: http($sock) send %d of %d\n", $r, length($send));
      ($r < 0) && ($! == EAGAIN) && ($r = 0);
      ($r < 0) && ($cfg{dbg} > 0) && print $log "$when: http($sock) data send error $!\n";
      ($r < 0) && ($stat = $r);
      ($r >= 0) && ($conn{$sock}{time} = $time);

      # advance seek pointer
      ($r > 0) && ($seek += $r);
      # decrease size and check for eof
      ($r > 0) && ($size -= $r);
      ($size < 0) && die "size overrun! size=$size r=$r";
      ($size <= 0) && ($stat = 1);
      # save updated seek/size for next pass
      $conn{$sock}{rang} = join(',',$seek,$size);
      # if more data, add socket to wakeup list
      ($size > 0) && (vec($sel_send,$sock,1) = 1);
    }

    # check for errors
    my $elap = $time-$conn{$sock}{time};
    #print "$when: http($sock): elap=$elap\n";
    ($elap > $cfg{maxtime}) && ($stat = -2) && ($cfg{dbg} > 0) && print $log "$when: http timeout ($elap secs) $conn{$sock}{from}($sock)\n";
    my $rsiz = length($conn{$sock}{head}.$conn{$sock}{body});
    ($rsiz > $cfg{maxdata}) && ($stat = -3) && ($cfg{dbg} > 0) && print $log "$when: http overflow ($rsiz bytes) $conn{$sock}{from}($sock)\n";

    # allow connection keep-alive
    if (!$fork && ($stat > 0) && ($conn{$sock}{head} =~ /Connection: keep-alive/i)) {
      ($cfg{dbg} > 2) && print $log "$when: http($sock) keep $conn{$sock}{from} on $conn{$sock}{bind}\n";
      $conn{$sock}{time} = $time;
      $conn{$sock}{size} = -1;
      $conn{$sock}{head} = "";
      $conn{$sock}{body} = "";
      delete $conn{$sock}{read};
      delete $conn{$sock}{data};
      delete $conn{$sock}{rang};
      delete $conn{$sock}{priv};
      vec($sel_wait,$sock,1) = 1;
      vec($sel_send,$sock,1) = 0;
      # disable nonblocking mode
      my $fl = 0;
      fcntl($conn{$sock}{sock}, F_GETFL, $fl);
      $fl |= O_NONBLOCK;
      $fl ^= O_NONBLOCK;
      fcntl($conn{$sock}{sock}, F_SETFL, $fl);
      $stat = 0;
    }

    # done with socket
    if ($stat != 0) {
      vec($sel_wait,$sock,1) = 0;
      vec($sel_send,$sock,1) = 0;
      close($conn{$sock}{sock});
      ($cfg{dbg} > 2) && print $log "$when: http($sock) close $stat $conn{$sock}{from}\n\n";
      delete $conn{$sock};
    }

    # forked responses are single-shot
    ($fork) && exit(0);
  }
}
return(1);


