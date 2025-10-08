package KasmVNC::ConfigKey;

use strict;
use warnings;
use v5.10;
use Switch;
use Data::Dumper;

use KasmVNC::Utils;

our $ConfigValue;

use constant {
  INT => 0,
  STRING => 1,
  BOOLEAN => 2,
  ANY => 4
};

sub new {
    my ($class, $args) = @_;
    my $self = bless {
      name => $args->{name},
      type => $args->{type},
      validator => $args->{validator}
    }, $class;
}

sub validate {
  my $self = shift;
  $self->{cliOption} = shift;

  return if $self->isValueBlank();

  switch($self->{type}) {
    case INT {
      $self->validateInt();
    }
    case BOOLEAN {
      $self->validateBoolean();
    }
  }

  if ($self->{validator}) {
    $self->resolveValidatorFromFunction() if (ref $self->{validator} eq "CODE");

    $self->{validator}->validate($self);
    return;
  }
}

sub resolveValidatorFromFunction {
  my $self = shift;

  $self->{validator} = $self->{validator}();
}

sub addErrorMessage {
  my $self = shift;

  my $errorMessage = $self->constructErrorMessage($_[0]);
  $self->{cliOption}->addErrorMessage($errorMessage);
}

# private

sub validateBoolean {
  my $self = shift;

  return if $self->isValidBoolean();
  $self->addErrorMessage("must be true or false");
}

sub validateInt {
  my $self = shift;

  return if $self->isValidInt();

  $self->addErrorMessage("must be an integer");
}

sub isValueBlank {
  my $self = shift;

  my $value = $self->value();
  !defined($value) || $value eq "";
}

sub constructErrorMessage {
  my $self = shift;
  my $staticErrorMessage = shift;

  my $name = $self->{name};
  my $value = join ", ", @{ listify($ConfigValue->($name)) };

  "$name '$value': $staticErrorMessage";
}

sub isValidInt {
  my $self = shift;

  $self->value() =~ /^(-)?\d+$/;
}

sub isValidBoolean {
  my $self = shift;

  $self->value() =~ /^true|false$/;
}

sub value {
  my $self = shift;

  $ConfigValue->($self->{name});
}

our @EXPORT_OK = ('INT', 'STRING', 'BOOLEAN');
