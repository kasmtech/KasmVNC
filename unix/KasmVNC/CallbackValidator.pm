package KasmVNC::CallbackValidator;

use strict;
use warnings;
use v5.10;
use Data::Dumper;

use KasmVNC::Utils;

sub new {
    my ($class, $args) = @_;
    my $self = bless {
      isValidCallback => $args->{isValidCallback},
      errorMessage => $args->{errorMessage}
    }, $class;
}

sub validate {
  my $self = shift;
  $self->{configKey} = shift;
  my @values = @{ listify($self->{configKey}->value()) };

  foreach my $value (@values) {
    $self->validateValue($value);
  }
}

sub validateValue {
  my $self = shift;
  my $value = shift;

  unless ($self->{isValidCallback}($value)) {
    $self->{configKey}->addErrorMessage($self->{errorMessage});
  }
}

1;
