package KasmVNC::TextOption;

use strict;
use warnings;
use v5.10;
use Data::Dumper;

sub new {
    my ($class, $args) = @_;
    my $self = bless {
      description => $args->{description},
      callback => $args->{callback} || sub {},
    }, $class;
}

use overload fallback => 1, q("") => sub {
  my $self = shift;

  $self->stringify();
};

sub stringify {
  my $self = shift;

  $self->{description};
}

sub description {
  my $self = shift;

  $self->{description};
}

sub callback {
  my $self = shift;

  $self->{callback};
}

1;
