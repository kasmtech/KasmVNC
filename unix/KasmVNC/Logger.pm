package KasmVNC::Logger;

use strict;
use warnings;
use v5.10;
use Data::Dumper;

sub new {
    my ($class, $args) = @_;
    my $self = bless {
    }, $class;
}

sub warn {
  my $self = shift;

  say { *STDERR } @_;
}

1;
