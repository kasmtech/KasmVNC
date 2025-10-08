package KasmVNC::SettingValidation;

use strict;
use warnings;
use v5.10;

sub isValid {
  my $self = shift;

  $self->validate() unless $self->{validated};

  $self->isNoErrorsPresent();
}

sub errorMessages {
  my $self = shift;

  join "\n", @{ $self->{errors} };
}

sub isNoErrorsPresent {
  my $self = shift;

  scalar @{ $self->{errors} } == 0;
}

sub addErrorMessage {
  my ($self, $errorMessage) = @_;

  push @{ $self->{errors} }, $errorMessage;
}

1;
