package KasmVNC::Logger;

use strict;
use warnings;
use v5.10;
use Data::Dumper;

sub new {
    my ($class, $args) = @_;
    my $self = bless {
      level => $args->{level} // "warn"
    }, $class;
}

sub debug {
  my $self = shift;

  return unless ($self->{level} eq "debug");

  say { *STDERR } @_;
}

sub warn {
  my $self = shift;

  say { *STDERR } @_;
}

1;
