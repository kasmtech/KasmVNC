package KasmVNC::EnumValidator;

use strict;
use warnings;
use v5.10;
use List::MoreUtils qw(any);
use Data::Dumper;
use KasmVNC::Utils;

sub new {
    my ($class, $args) = @_;
    my $self = bless {
      allowedValues => $args->{allowedValues}
    }, $class;
}

sub validate {
  my $self = shift;
  my $configKey = shift;
  my @values = @{ listify($configKey->value()) };

  foreach my $value (@values) {
    unless (any { $_ eq $value } @{ $self->{allowedValues} }) {
      $configKey->addErrorMessage($self->errorMessage());
    }
  }
}

sub errorMessage {
  my $self = shift;

  my $allowedValuesText = join ", ", @{ $self->{allowedValues} };
  "must be one of [$allowedValuesText]"
}

1;
